#include "../include/pdf_indexer.h"
#include "../include/pdf.h"
#include "../include/pdf_preprocess.h"
#include "../include/vfs.h"

#include <inttypes.h>
#include <poppler/glib/poppler.h>
#include <solidc/defer.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>
#include <stdio.h>   // for fprintf, printf
#include <stdlib.h>  // for malloc, free, strdup, atoll
#include <string.h>  // for strcmp, strcasecmp, strrchr, strlen

/** SQL queries used throughout this file. */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) VALUES($1, $2, $3) "
    "ON CONFLICT(name, path) DO UPDATE SET num_pages = EXCLUDED.num_pages "
    "RETURNING id";

static const char* page_insert_query =
    "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3) "
    "ON CONFLICT (file_id, page_num) DO NOTHING";

static const char* file_id_query = "SELECT id FROM files WHERE path = $1";

// Virtual file system from main.c (if not NULL, indexing reads from VFS)
extern vfs_t* vfs;

/* =========================================================================
 * Per-Thread Connection Slots
 *
 * Each worker thread is assigned one dedicated pgconn_t for its lifetime.
 * Slots are populated once before the threadpool starts and released after
 * threadpool_wait() returns. Workers index into this array via their
 * thread-local slot id, so there is zero lock contention on DB access.
 * ======================================================================= */

/**
 * One connection slot assigned to a single worker thread.
 *
 * worker_id is set before the first task runs and never changes. conn is
 * opened once and reused across all tasks that land on this thread.
 */
typedef struct {
    pgconn_t* conn; /** Dedicated DB connection for this worker. */
    int worker_id;  /** Index into g_worker_slots[]. */
} WorkerSlot;

/** Global array of per-thread connection slots. Indexed by worker_id. */
static WorkerSlot g_worker_slots[WORKER_COUNT];

/**
 * Thread-local worker id.
 *
 * Assigned once by the trampoline task submitted to each thread before
 * real work begins. Workers use it to look up their connection slot without
 * any locking.
 */
static _Thread_local int tl_worker_id = -1;

/** Unref a GObject safely on scope exit. */
#define defer_g_object_unref(obj)         \
    defer {                               \
        if ((obj)) g_object_unref((obj)); \
    }

/**
 * Opens a PDF document.
 *
 * Routes through the VFS layer when the global vfs pointer is set, otherwise
 * falls back to the host filesystem.
 *
 * @param path      Absolute path to the PDF (VFS or host).
 * @param num_pages Output: total page count of the opened document.
 * @return Open PopplerDocument on success, NULL on failure.
 */
static PopplerDocument* open_pdf_document(const char* path, int* num_pages) {
    if (vfs != NULL) return open_vfs_document(vfs, path, num_pages);
    return open_document(path, num_pages);
}

/* =========================================================================
 * Parameters passed from the walk/scan callback to each worker task.
 *
 * The walk callback allocates this struct and submits it to the threadpool.
 * Ownership transfers entirely to the worker, which frees path, name, and
 * the struct itself before returning.
 *
 * Page counting is intentionally deferred to the worker (see process_pdfs
 * design notes). npages is therefore initialised to 0 by the callback.
 * ======================================================================= */

/**
 * Task parameters for a single PDF file.
 *
 * Ownership of path, name, and the struct itself is transferred to the
 * worker task on submission.
 */
typedef struct {
    char* path;            /** Absolute path to the PDF. Freed by worker. */
    char* name;            /** Basename of the PDF. Freed by worker. */
    _Atomic bool* success; /** Shared flag; worker stores false on error. */
    bool dryrun;           /** When true, only print; no DB writes. */
    int min_pages;         /** Skip files with fewer pages than this. */
} PDFTaskParams;

/* =========================================================================
 * Indexing Core
 * ======================================================================= */

/**
 * Extracts and inserts the text of every page in @p doc.
 *
 * All INSERTs run on @p conn which must already be inside an open
 * transaction. On a page-level INSERT failure the error is logged and
 * processing continues — the outer transaction will still commit any pages
 * that succeeded. The DO NOTHING conflict clause means re-indexing an
 * already-indexed page is a silent no-op.
 *
 * Savepoints are intentionally absent: they cost three round-trips per page
 * (SAVEPOINT / INSERT / RELEASE) and a single constraint violation from
 * DO NOTHING never aborts the transaction anyway.
 *
 * @param doc      Open Poppler document.
 * @param file_id  files.id already inserted on @p conn in this transaction.
 * @param name     Filename used in error messages only.
 * @param npages   Page count (loop bound).
 * @param conn     Connection with an active transaction.
 * @return true if every page was inserted without error, false if any failed.
 */
static bool process_all_pages(PopplerDocument* doc, int64_t file_id, const char* name, int npages, pgconn_t* conn) {
    // Pre-format the file_id string once; it is the same for every page.
    char file_id_str[32];
    snprintf(file_id_str, sizeof(file_id_str), "%" PRId64, file_id);

    bool all_ok = true;

    for (int page_num = 0; page_num < npages; page_num++) {
        PopplerPage* page = poppler_document_get_page(doc, page_num);
        if (!page) {
            fprintf(stderr, ">>> Failed to get page %d of %s\n", page_num + 1, name);
            all_ok = false;
            continue;
        }
        defer_g_object_unref(page);

        char* text = poppler_page_get_text(page);
        defer {
            free(text);
        };

        // Empty page — nothing to index, not an error.
        if (!text || text[0] == '\0') continue;

        size_t len = strlen(text);

        // Hard-truncate to stay within the tokenizer's input limit.
        if (len >= 2047) {
            text[2046] = '\0';
            len = 2046;
        }
        pdf_text_clean(text, len, false);

        char page_num_str[16];
        snprintf(page_num_str, sizeof(page_num_str), "%d", page_num + 1);

        const char* values[] = {file_id_str, page_num_str, text};

        if (!pgpool_execute_params(conn, page_insert_query, 3, values, 10000)) {
            fprintf(stderr, ">>> Failed to insert page %d of %s: %s\n", page_num + 1, name, pgpool_error_message(conn));
            all_ok = false;
            // Continue — other pages on this connection are unaffected because
            // DO NOTHING never transitions the transaction into an error state.
        }
    }

    return all_ok;
}

/**
 * Threadpool task: opens, reads, and indexes one PDF end-to-end.
 *
 * ## Design
 *
 * The worker opens the PDF itself (removing the serial bottleneck that
 * existed when the walk callback counted pages on the main thread). All DB
 * work executes on the thread's dedicated connection — no pool acquire/release
 * overhead, no mutex contention.
 *
 * A single transaction wraps the file INSERT and all page INSERTs. This keeps
 * the FK constraint between pages.file_id and files.id satisfied at COMMIT
 * time and means a failed file leaves no partial state in the database.
 *
 * ## Ownership
 *
 * Takes ownership of the heap-allocated PDFTaskParams pointed to by @p arg.
 * Always frees it before returning.
 *
 * @param arg Heap-allocated PDFTaskParams. Must not be NULL.
 */
static void process_pdf_task(void* arg) {
    PDFTaskParams* params = arg;
    defer {
        free(params->path);
        free(params->name);
        free(params);
    };

    // --- Open PDF and count pages (parallel, on the worker thread) ---
    int npages = 0;
    PopplerDocument* doc = open_pdf_document(params->path, &npages);
    defer_g_object_unref(doc);

    if (!doc) {
        fprintf(stderr, ">>> Failed to open PDF: %s\n", params->path);
        atomic_store(params->success, false);
        return;
    }

    if (npages == 0 || npages < params->min_pages) {
        // File is below the page threshold or empty — skip silently.
        return;
    }

    if (params->dryrun) {
        printf(">>> Found %s (%d pages)\n", params->path, npages);
        return;
    }

    // --- Retrieve this thread's dedicated connection ---
    //
    // tl_worker_id is initialised by the trampoline submitted to each thread
    // before any real tasks run. A value of -1 here means the trampoline was
    // never executed, which indicates a programming error.
    if (tl_worker_id < 0 || tl_worker_id >= WORKER_COUNT) {
        fprintf(stderr, ">>> Worker has no connection slot (id=%d) for %s\n", tl_worker_id, params->name);
        atomic_store(params->success, false);
        return;
    }

    pgconn_t* conn = g_worker_slots[tl_worker_id].conn;
    if (!conn) {
        fprintf(stderr, ">>> NULL connection in slot %d for %s\n", tl_worker_id, params->name);
        atomic_store(params->success, false);
        return;
    }

    // --- One transaction: file INSERT + all page INSERTs ---
    bool in_txn = false;
    defer {
        // Roll back if we exit before a successful COMMIT (error paths).
        if (in_txn) pgpool_rollback(conn, 15000);
    };

    if (!pgpool_begin(conn, 5000)) {
        fprintf(stderr, ">>> BEGIN failed for %s: %s\n", params->name, pgpool_error_message(conn));
        atomic_store(params->success, false);
        return;
    }
    in_txn = true;

    // Insert (or upsert) the file row and retrieve its id.
    char npages_str[16];
    snprintf(npages_str, sizeof(npages_str), "%d", npages);
    const char* file_values[] = {params->name, params->path, npages_str};

    PGresult* res = pgpool_query_params(conn, file_insert_query, 3, file_values, 10000);
    if (!res) {
        fprintf(stderr, ">>> File INSERT failed for %s: %s\n", params->path, pgpool_error_message(conn));
        atomic_store(params->success, false);
        return;  // defer rolls back
    }

    int64_t file_id = 0;
    if (PQntuples(res) > 0) {
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    } else {
        // UPSERT returned zero rows: the row existed and num_pages was
        // unchanged so the DO UPDATE SET produced no net change. Fall back
        // to a point query to retrieve the existing id.
        PQclear(res);
        const char* path_param[] = {params->path};
        res = pgpool_query_params(conn, file_id_query, 1, path_param, 10000);
        if (!res || PQntuples(res) == 0) {
            fprintf(stderr, ">>> Could not retrieve file_id for %s\n", params->path);
            if (res) PQclear(res);
            atomic_store(params->success, false);
            return;  // defer rolls back
        }
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    // Insert all pages within the same transaction.
    bool pages_ok = process_all_pages(doc, file_id, params->name, npages, conn);

    if (pages_ok) {
        if (!pgpool_commit(conn, 15000)) {
            fprintf(stderr, ">>> COMMIT failed for %s: %s\n", params->name, pgpool_error_message(conn));
            atomic_store(params->success, false);
            // in_txn stays true; defer issues the ROLLBACK.
        } else {
            in_txn = false;  // Committed cleanly — suppress the deferred ROLLBACK.
            printf(">>> Indexed %s (%d pages)\n", params->path, npages);
        }
    } else {
        fprintf(stderr, ">>> Rolling back %s (page errors)\n", params->name);
        atomic_store(params->success, false);
        // in_txn stays true; defer issues the ROLLBACK.
    }
}

/* =========================================================================
 * Worker Slot Initialisation
 *
 * Before any PDF task runs, a trampoline task is submitted to every thread
 * in the pool to bind tl_worker_id to the thread's slot index. Because the
 * threadpool dispatches one task per thread in order, submitting WORKER_COUNT
 * trampolines and then calling threadpool_wait() guarantees that every thread
 * has its id set before real tasks are submitted.
 * ======================================================================= */

/** Argument for the trampoline task. */
typedef struct {
    int slot_id; /** Index into g_worker_slots[] to assign to this thread. */
} TrampolineArg;

/**
 * Threadpool trampoline: assigns tl_worker_id for the calling thread.
 *
 * @param arg Pointer to a heap-allocated TrampolineArg. Freed here.
 */
static void worker_init_trampoline(void* arg) {
    TrampolineArg* ta = arg;
    tl_worker_id = ta->slot_id;
    free(ta);
}

/* =========================================================================
 * Directory Walk / VFS Scan Callbacks
 * ======================================================================= */

/**
 * Shared state threaded through both walk/scan callbacks.
 *
 * The callbacks do no DB work — they just discover paths and submit tasks.
 */
typedef struct {
    Threadpool* thpool;    /** Receives one task per eligible PDF. */
    _Atomic bool* success; /** Cleared by workers on unrecoverable error. */
    bool dryrun;           /** Print-only mode; no DB writes. */
    int min_pages;         /** PDFs below this page count are skipped. */
} WalkDirUserData;

/**
 * Allocates and submits a PDFTaskParams for @p path.
 *
 * Centralises the allocation + submit logic shared by both callbacks.
 *
 * @param path      Full path to the PDF (VFS or host).
 * @param name      Basename of the PDF.
 * @param data      Shared walk state.
 * @return true on success, false on OOM (shared success flag also cleared).
 */
static bool submit_pdf_task(const char* path, const char* name, WalkDirUserData* data) {
    PDFTaskParams* params = malloc(sizeof(*params));
    if (!params) {
        fprintf(stderr, ">>> OOM allocating task for %s\n", path);
        atomic_store(data->success, false);
        return false;
    }

    params->path = strdup(path);
    params->name = strdup(name);
    params->success = data->success;
    params->dryrun = data->dryrun;
    params->min_pages = data->min_pages;

    if (!params->path || !params->name) {
        fprintf(stderr, ">>> OOM duplicating strings for %s\n", path);
        free(params->path);
        free(params->name);
        free(params);
        atomic_store(data->success, false);
        return false;
    }

    threadpool_submit(data->thpool, process_pdf_task, params);
    return true;
}

/**
 * VFS scan callback: submits one task per PDF found in the virtual filesystem.
 *
 * PDF opening and page counting are deferred to the worker so that the scan
 * loop is non-blocking and all Poppler work proceeds in parallel.
 *
 * @param path     VFS path to the current entry.
 * @param st       VFS stat (unused).
 * @param userdata Pointer to WalkDirUserData.
 * @return true to continue scanning, false to abort on OOM.
 */
static bool vfs_list_callback(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    WalkDirUserData* data = userdata;

    const char* ext = strrchr(path, '.');
    if (!ext || ext == path) return true;
    if (strcasecmp(ext + 1, "pdf") != 0) return true;

    const char* base = strrchr(path, '/');
    const char* name = base ? base + 1 : path;

    return submit_pdf_task(path, name, data);
}

/**
 * Host filesystem walk callback: submits one task per PDF found on disk.
 *
 * PDF opening and page counting are deferred to the worker. The walk
 * callback's only job is to identify candidate paths and submit tasks,
 * keeping the main thread free of Poppler work.
 *
 * @param attr     File attributes (unused).
 * @param path     Absolute path to the current entry.
 * @param name     Basename of the current entry.
 * @param userdata Pointer to WalkDirUserData.
 * @return DirContinue normally; DirSkip for ignored directories;
 *         DirStop on OOM.
 */
static WalkDirOption walk_dir_callback(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;

    // Skip well-known noise directories.
    static const char* skip_dirs[] = {
        "node_modules", ".git",   ".svn",    ".hg",       "__pycache__", ".pytest_cache", ".mypy_cache",
        ".tox",         "venv",   ".venv",   "env",       ".env",        "vendor",        "build",
        "dist",         "target", ".gradle", ".idea",     ".vscode",     ".cache",        "coverage",
        ".next",        ".nuxt",  ".turbo",  ".DS_Store", NULL,
    };

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) return DirSkip;
    }

    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) return DirContinue;
    if (strcasecmp(ext + 1, "pdf") != 0) return DirContinue;

    WalkDirUserData* data = userdata;
    return submit_pdf_task(path, name, data) ? DirContinue : DirStop;
}

/* =========================================================================
 * Public Entry Point
 * ======================================================================= */

/**
 * Scans @p root_dir (or the active VFS) for PDFs and indexes their text.
 *
 * ## Threading model
 *
 * Four worker threads are created. Each is assigned a dedicated pgconn_t
 * from the global pool so there is no connection acquisition overhead or
 * mutex contention during indexing. PDF opening, page extraction, text
 * cleaning, and all DB writes happen entirely on worker threads — the main
 * thread only walks the directory tree and submits tasks.
 *
 * ## Transaction model
 *
 * Each PDF is wrapped in a single transaction (BEGIN … COMMIT). The file row
 * INSERT and all page INSERTs share one connection, eliminating the FK
 * visibility race. Per-page savepoints are removed; the DO NOTHING conflict
 * clause on the page INSERT handles duplicates without aborting the
 * transaction.
 *
 * ## Success semantics
 *
 * The shared _Atomic bool starts true. Any worker that fails an unrecoverable
 * operation clears it. The function returns the flag's value after all workers
 * finish. A false return does not mean nothing was indexed — committed
 * transactions are durable regardless of what other workers did.
 *
 * @param root_dir  Directory to scan recursively (ignored when VFS is active).
 * @param min_pages Skip PDFs with fewer pages than this.
 * @param dryrun    When true, only print discovered files; no DB writes.
 * @return true if every PDF was processed without error, false otherwise.
 */
bool process_pdfs(const char* root_dir, int min_pages, bool dryrun) {
    _Atomic bool success = true;

    // --- Pre-allocate one DB connection per worker ---
    //
    // Connections are acquired before the threadpool starts so that workers
    // can access them without any locking. On failure we release whatever
    // was acquired and bail out.
    if (!dryrun) {
        for (int i = 0; i < WORKER_COUNT; i++) {
            g_worker_slots[i].worker_id = i;
            g_worker_slots[i].conn = pgpool_acquire(pool, 10000);
            if (!g_worker_slots[i].conn) {
                fprintf(stderr, ">>> Failed to acquire DB connection for worker %d\n", i);
                // Release whatever was acquired before this failure.
                for (int j = 0; j < i; j++) {
                    pgpool_release(pool, g_worker_slots[j].conn);
                    g_worker_slots[j].conn = NULL;
                }
                return false;
            }
        }
    }

    // Release all worker connections on every exit path.
    defer {
        if (!dryrun) {
            for (int i = 0; i < WORKER_COUNT; i++) {
                if (g_worker_slots[i].conn) {
                    pgpool_release(pool, g_worker_slots[i].conn);
                    g_worker_slots[i].conn = NULL;
                }
            }
        }
    };

    Threadpool* threadpool = threadpool_create(WORKER_COUNT);
    if (!threadpool) {
        fprintf(stderr, ">>> Failed to create threadpool\n");
        return false;
    }
    defer {
        threadpool_destroy(threadpool, 60000);
    };

    // --- Bind tl_worker_id on every thread before real tasks run ---
    //
    // Submit one trampoline per worker slot, then wait. After the wait every
    // thread has tl_worker_id set and knows which connection to use.
    if (!dryrun) {
        for (int i = 0; i < WORKER_COUNT; i++) {
            TrampolineArg* ta = malloc(sizeof(*ta));
            if (!ta) {
                fprintf(stderr, ">>> OOM allocating trampoline %d\n", i);
                atomic_store(&success, false);
                return false;
            }
            ta->slot_id = i;
            threadpool_submit(threadpool, worker_init_trampoline, ta);
        }
        threadpool_wait(threadpool);
    }

    // --- Walk / scan and submit one task per PDF ---
    WalkDirUserData data = {
        .thpool = threadpool,
        .success = &success,
        .dryrun = dryrun,
        .min_pages = min_pages,
    };

    if (vfs != NULL) {
        if (dryrun) printf("Performing VFS index dry run\n");
        vfs_list(vfs, NULL, vfs_list_callback, &data);
    } else {
        if (dryrun) printf("Performing host index dry run on %s\n", root_dir);
        int rc = dir_walk(root_dir, walk_dir_callback, &data);
        if (rc != 0) atomic_store(&success, false);
    }

    threadpool_wait(threadpool);

    return atomic_load(&success);
}
