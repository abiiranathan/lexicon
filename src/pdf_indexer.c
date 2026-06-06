#include "../include/pdf_indexer.h"
#include "../include/pdf_preprocess.h"

#include <inttypes.h>
#include <poppler/glib/poppler.h>
#include <solidc/defer.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>

/** SQL queries used throughout this file. */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) VALUES($1, $2, $3) ON CONFLICT(name, path) DO UPDATE "
    "SET num_pages = EXCLUDED.num_pages RETURNING id";

static const char* page_insert_query =
    "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3) ON CONFLICT (file_id, page_num) "
    "DO NOTHING";

static const char* file_id_query = "SELECT id FROM files WHERE path = $1";

/**
 * Parameters for processing an entire PDF file.
 *
 * The walk callback populates this struct and transfers ownership to the
 * worker. The worker is responsible for freeing path, name, and the struct
 * itself via process_pdf_task.
 *
 * file_id is intentionally absent: the worker resolves it inside its own
 * transaction so that the INSERT into files and the INSERTs into pages are
 * on the same connection and are visible to each other without requiring a
 * cross-connection commit first.
 */
typedef struct {
    char* path;            /** Full path to the PDF file. Ownership transferred to worker. */
    char* name;            /** Filename (basename). Ownership transferred to worker. */
    int npages;            /** Page count verified during the directory walk. */
    _Atomic bool* success; /** Shared success flag; worker clears on any error. */
} PDFProcessParams;

/** Unref a GObject safely on scope exit. */
#define defer_g_object_unref(obj)         \
    defer {                               \
        if ((obj)) g_object_unref((obj)); \
    };

/**
 * Extracts text from every page of @p doc and inserts it into the database.
 *
 * The caller must have already inserted the file row and obtained @p file_id
 * within the same transaction on @p conn. Transaction management is entirely
 * the caller's responsibility.
 *
 * Each INSERT is wrapped in a savepoint so that a server-side error (e.g. a
 * constraint violation) does not abort the surrounding transaction. On INSERT
 * failure the savepoint is rolled back — clearing PostgreSQL's aborted-
 * transaction state — and processing continues. This means a single bad page
 * does not poison the connection for the pages that follow.
 *
 * @param doc      Open Poppler document to read pages from.
 * @param file_id  Database file ID that every page row references. Must
 *                 already be visible on @p conn (i.e. inserted earlier in the
 *                 same transaction).
 * @param name     Filename used in error messages only.
 * @param npages   Number of pages to process (loop bound).
 * @param conn     Database connection with an active transaction.
 * @return true if every page was processed without error, false if at least
 *         one page could not be read or inserted.
 */
static bool process_all_pages(PopplerDocument* doc, int64_t file_id, const char* name, int npages, pgconn_t* conn) {
    bool all_ok = true;
    for (int page_num = 0; page_num < npages; page_num++) {
        PopplerPage* page = poppler_document_get_page(doc, page_num);
        if (!page) {
            fprintf(stderr, ">>> Failed to get page %d of %s\n", page_num + 1, name);
            all_ok = false;
            continue;
        }
        defer_g_object_unref(page);

        DEFER_VAR char* text = poppler_page_get_text(page);
        defer_free(text);

        // Empty page — not an error, just nothing to index.
        if (!text || text[0] == '\0') { continue; }

        size_t len = strlen(text);

        // Hard-truncate to stay within the tokenizer's input limit.
        if (len >= 2047) {
            text[2046] = '\0';
            len = 2046;
        }
        pdf_text_clean(text, len, false);

        // Format integer parameters as strings for the libpq parameterised query.
        char file_id_str[32];
        char page_num_str[16];
        snprintf(file_id_str, sizeof(file_id_str), "%" PRId64, file_id);
        snprintf(page_num_str, sizeof(page_num_str), "%d", page_num + 1);

        const char* values[] = {file_id_str, page_num_str, text};

        // Set a savepoint before each INSERT. If the server returns an error
        // (e.g. FK violation, unique conflict not covered by DO NOTHING),
        // PostgreSQL transitions the connection into an aborted-transaction
        // state and rejects every subsequent command until a ROLLBACK. Rolling
        // back to the savepoint clears that state while keeping the rest of
        // the transaction intact so we can continue processing remaining pages.
        if (!pgpool_execute(conn, "SAVEPOINT page_insert", 5000)) {
            fprintf(stderr, ">>> Failed to set SAVEPOINT for page %d of %s: %s\n", page_num + 1, name,
                    pgpool_error_message(conn));
            all_ok = false;
            continue;
        }

        if (!pgpool_execute_params(conn, page_insert_query, 3, values, 5000)) {
            fprintf(stderr, ">>> Failed to insert page %d of %s: %s\n", page_num + 1, name, pgpool_error_message(conn));

            // Clear the aborted-transaction state before the next iteration.
            pgpool_execute(conn, "ROLLBACK TO SAVEPOINT page_insert", 5000);
            all_ok = false;
        } else {
            // Release the savepoint to free the server-side snapshot it holds.
            pgpool_execute(conn, "RELEASE SAVEPOINT page_insert", 5000);
        }
    }

    return all_ok;
}

/**
 * Threadpool task that indexes one PDF file end-to-end in a single transaction.
 *
 * ## Transaction model
 *
 * Both the file-level INSERT and all page INSERTs execute on the same
 * connection within one transaction. This is the key design decision that
 * avoids cross-connection FK visibility races: because the files row and the
 * pages rows are created on the same connection in the same transaction, the
 * FK constraint is satisfied at COMMIT time without any inter-connection
 * synchronisation.
 *
 * On any failure (cannot open PDF, file INSERT fails, page errors) the whole
 * transaction is rolled back atomically and the shared success flag is cleared.
 *
 * Ownership of @p arg (a heap-allocated PDFProcessParams) is transferred to
 * this function; it is always freed before returning.
 *
 * @param arg Pointer to a heap-allocated PDFProcessParams. Must not be NULL.
 */
static void process_pdf_task(void* arg) {
    PDFProcessParams* params = arg;

    // Free owned strings and the struct itself on every exit path.
    defer {
        free(params->path);
        free(params->name);
        free(params);
    };

    // Build the URI Poppler expects from a filesystem path.
    char* uri = g_filename_to_uri(params->path, NULL, NULL);
    ASSERT(uri != NULL);
    defer {
        g_free(uri);
    };

    GError* error = NULL;
    PopplerDocument* doc = poppler_document_new_from_file(uri, NULL, &error);
    defer_g_object_unref(doc);

    if (!doc) {
        fprintf(stderr, ">>> Worker failed to open PDF %s: %s\n", params->path, error->message);
        g_error_free(error);
        atomic_store(params->success, false);
        return;
    }

    // Guard against the file being modified between the walk and this task
    // being picked up by a worker thread.
    int actual_pages = poppler_document_get_n_pages(doc);
    if (actual_pages != params->npages) {
        fprintf(stderr, ">>> Page count mismatch for %s: expected %d, got %d\n", params->name, params->npages,
                actual_pages);
        atomic_store(params->success, false);
        return;
    }

    pgconn_t* conn = pgpool_acquire(pool, -1);
    if (!conn) {
        fprintf(stderr, ">>> Failed to acquire DB connection for %s\n", params->name);
        atomic_store(params->success, false);
        return;
    }
    defer_release(pool, conn);

    if (!pgpool_begin(conn, 1000)) {
        fprintf(stderr, ">>> Failed to BEGIN transaction for %s: %s\n", params->name, pgpool_error_message(conn));
        atomic_store(params->success, false);
        return;
    }

    // --- INSERT the file row first, inside this transaction ---
    //
    // Doing this here (rather than on the main thread) means the files row and
    // all pages rows share one connection and one transaction. The FK on pages
    // is therefore satisfied at COMMIT without needing the main thread to have
    // committed first.
    char num_pages_str[16];
    snprintf(num_pages_str, sizeof(num_pages_str), "%d", params->npages);

    const char* file_values[] = {params->name, params->path, num_pages_str};
    PGresult* res = pgpool_query_params(conn, file_insert_query, 3, file_values, -1);
    if (!res) {
        fprintf(stderr, ">>> Failed to insert file record for %s: %s\n", params->path, pgpool_error_message(conn));
        pgpool_rollback(conn, 2000);
        atomic_store(params->success, false);
        return;
    }

    // The UPSERT returns the id on INSERT or UPDATE. If the row already existed
    // and num_pages was identical, some servers return zero tuples; fall back to
    // a point query in that case.
    int64_t file_id = 0;
    if (PQntuples(res) > 0) {
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    } else {
        PQclear(res);
        const char* path_param[] = {params->path};

        res = pgpool_query_params(conn, file_id_query, 1, path_param, -1);
        if (!res || PQntuples(res) == 0) {
            fprintf(stderr, ">>> Could not retrieve file_id for %s\n", params->path);
            if (res) PQclear(res);
            pgpool_rollback(conn, 1000);
            atomic_store(params->success, false);
            return;
        }

        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    }

    // --- INSERT all pages within the same transaction ---
    bool pages_ok = process_all_pages(doc, file_id, params->name, params->npages, conn);

    if (pages_ok) {
        if (!pgpool_commit(conn, 1000)) {
            fprintf(stderr, ">>> Failed to COMMIT transaction for %s: %s — rolling back\n", params->name,
                    pgpool_error_message(conn));
            pgpool_rollback(conn, 1000);
            atomic_store(params->success, false);
        }
    } else {
        fprintf(stderr, ">>> Rolling back transaction for %s due to page errors\n", params->name);
        pgpool_rollback(conn, 1000);
        atomic_store(params->success, false);
    }
}

/**
 * User data threaded through the directory-walking callback.
 *
 * The main thread no longer owns a database connection: all DB work has been
 * moved into the workers. The walk callback's only job is to discover PDFs,
 * count their pages, and submit tasks.
 */
typedef struct {
    Threadpool* thpool;    /** Threadpool that receives per-PDF tasks. */
    int min_pages;         /** PDFs with fewer pages than this are skipped. */
    _Atomic bool* success; /** Shared success flag; set false by workers on error. */
    bool dryrun;           /** When true, only print discovered files; no DB writes. */
} WalkDirUserData;

/**
 * Directory-walking callback that discovers PDF files and submits them for indexing.
 *
 * Each eligible PDF is opened briefly to verify its page count, then a
 * PDFProcessParams struct is allocated and submitted to the threadpool. The
 * worker that picks up the task is responsible for all database operations
 * (both the file INSERT and all page INSERTs) within a single transaction.
 *
 * The main thread performs no database writes, so no main-thread transaction
 * is needed and there is no cross-connection FK visibility issue.
 *
 * @param attr     File attributes provided by dir_walk (unused).
 * @param path     Absolute path to the current entry.
 * @param name     Basename of the current entry.
 * @param userdata Pointer to WalkDirUserData.
 * @return DirContinue normally; DirSkip to skip a directory subtree; DirStop
 *         to abort the entire walk on an unrecoverable error.
 */
static WalkDirOption walk_dir_callback(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;

    // Skip well-known build / VCS / tool directories to avoid wasted work.
    // NULL-terminated so new entries can be appended without changing the loop.
    static const char* skip_dirs[] = {
        "node_modules", ".git",   ".svn",    ".hg",       "__pycache__", ".pytest_cache", ".mypy_cache",
        ".tox",         "venv",   ".venv",   "env",       ".env",        "vendor",        "build",
        "dist",         "target", ".gradle", ".idea",     ".vscode",     ".cache",        "coverage",
        ".next",        ".nuxt",  ".turbo",  ".DS_Store", NULL,
    };

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) { return DirSkip; }
    }

    // Only process files whose extension is ".pdf" (case-insensitive).
    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) { return DirContinue; }
    ext++;  // Advance past the dot.
    if (strcasecmp(ext, "pdf") != 0) { return DirContinue; }

    WalkDirUserData* data = userdata;

    // Open the PDF on the main thread solely to count pages. The document is
    // discarded immediately; workers reopen it from params->path.
    gchar* uri = g_filename_to_uri(path, NULL, NULL);
    ASSERT(uri != NULL);
    defer {
        g_free(uri);
    };

    GError* error = NULL;
    PopplerDocument* doc = poppler_document_new_from_file(uri, NULL, &error);
    defer_g_object_unref(doc);

    if (!doc) {
        fprintf(stderr, ">>> Error opening PDF %s: %s. Skipping.\n", path, error->message);
        g_error_free(error);
        return DirContinue;
    }

    int npages = poppler_document_get_n_pages(doc);
    if (npages == 0 || npages < data->min_pages) { return DirContinue; }

    if (data->dryrun) {
        printf(">>> Found %s (%d pages)\n", path, npages);
        return DirContinue;
    }

    // Allocate task parameters; ownership transfers entirely to the worker.
    PDFProcessParams* params = malloc(sizeof(*params));
    if (!params) {
        fprintf(stderr, ">>> OOM allocating task params for %s\n", path);
        atomic_store(data->success, false);
        return DirStop;
    }

    params->path = strdup(path);
    params->name = strdup(name);
    params->npages = npages;
    params->success = data->success;

    if (!params->path || !params->name) {
        fprintf(stderr, ">>> OOM duplicating strings for %s\n", path);
        free(params->path);
        free(params->name);
        free(params);
        atomic_store(data->success, false);
        return DirStop;
    }

    threadpool_submit(data->thpool, process_pdf_task, params);
    return DirContinue;
}

/**
 * Scans @p root_dir for PDF files and indexes their text content into the
 * database.
 *
 * ## Threading model
 *
 * The main thread walks the directory tree and submits one task per eligible
 * PDF. Each worker thread handles a single PDF end-to-end: it opens the
 * document, inserts the file metadata row, inserts all page rows, and either
 * commits or rolls back — all on its own connection within one transaction.
 *
 * Keeping the file INSERT and page INSERTs on the same connection in the same
 * transaction eliminates the FK visibility race that would otherwise occur if
 * file rows were committed by the main thread and page rows were inserted by
 * workers on separate connections.
 *
 * The main thread performs no database writes and therefore needs no
 * transaction of its own.
 *
 * ## Success semantics
 *
 * The shared _Atomic bool success flag starts true. Any worker that encounters
 * an unrecoverable error clears it. The function returns the flag's final
 * value after all workers have finished. A false return does not mean nothing
 * was indexed — successfully committed worker transactions are durable
 * regardless of what other workers did.
 *
 * @param root_dir  Directory to scan recursively.
 * @param min_pages Skip PDFs with fewer than this many pages.
 * @param dryrun    When true, print discovered files and return without
 *                  writing to the database.
 * @return true if every PDF was processed without error, false otherwise.
 */
bool process_pdfs(const char* root_dir, int min_pages, bool dryrun) {
    _Atomic bool success = true;

    Threadpool* threadpool = threadpool_create(4);
    if (!threadpool) {
        fprintf(stderr, ">>> Failed to create threadpool\n");
        return false;
    }
    defer {
        threadpool_destroy(threadpool, -1);
    };

    if (dryrun) { printf("Performing index dry run on %s\n", root_dir); }

    WalkDirUserData data = {
        .thpool = threadpool,
        .min_pages = min_pages,
        .success = &success,
        .dryrun = dryrun,
    };

    int walk_result = dir_walk(root_dir, walk_dir_callback, &data);
    if (walk_result != 0) {
        // The walk itself failed; mark failure without clobbering a false
        // already stored by a worker.
        atomic_store(&success, false);
    }

    // Wait for all workers to finish before reading the final success state.
    // threadpool_destroy (via defer above) would also wait, but being explicit
    // here makes the intent clear and lets us return the correct value.
    threadpool_wait(threadpool);

    return atomic_load(&success);
}
