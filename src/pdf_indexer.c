/*
 * pdf_indexer.c — high-throughput PDF text extractor / PostgreSQL ingester.
 *
 * Architecture
 * ============
 *
 *   dir_walk / vfs_list
 *       │  (path strings, skips already-known paths before any I/O)
 *       ▼
 *   ┌──────────────────────────────────────────┐
 *   │  Extractor pool  (nCPU threads)           │
 *   │  mmap → Poppler → pdf_text_clean → arena  │
 *   └───────────────────┬──────────────────────┘
 *                       │ PageBatch* (lock-free MPMC ring)
 *                       ▼
 *   ┌──────────────────────────────────────────┐
 *   │  Writer pool  (WORKER_COUNT threads)      │
 *   │  libpq COPY FROM STDIN                   │
 *   │  single round-trip per N pages            │
 *   └───────────────────┬──────────────────────┘
 *                       │
 *                       ▼
 *                  PostgreSQL
 *
 * Why this is fast
 * ----------------
 *   • Extraction and DB ingestion are fully decoupled.  Poppler runs at
 *     100 % CPU while PostgreSQL drains the queue in parallel.
 *   • COPY via PQputCopyData has no parameter serialization overhead and
 *     sustains hundreds of thousands of rows/second on loopback.
 *   • DB connections are held only by writers; extractors never wait on them.
 *   • Path dedup is checked in the walk callback — no file I/O for docs that
 *     are already indexed.
 *   • Per-thread scratch arenas are reset (not destroyed) across tasks,
 *     eliminating thousands of mmap/munmap syscalls.
 *   • The file record INSERT uses a single RETURNING query; no fallback
 *     SELECT is ever needed.
 */

#include "../include/pdf_indexer.h"
#include "../include/pdf.h"
#include "../include/pdf_preprocess.h"

#include <fcntl.h>    /* open, O_RDONLY, O_CLOEXEC */
#include <inttypes.h> /* PRId64 */
#include <libpq-fe.h> /* PGconn, PQputCopyData, etc. */
#include <poppler/glib/poppler.h>
#include <pthread.h>  /* pthread_t, pthread_mutex_t, pthread_cond_t */
#include <signal.h>   /* sigaction, struct sigaction, SIGINT, SIGTERM */
#include <solidc/arena.h>
#include <solidc/defer.h>
#include <solidc/map.h>
#include <solidc/thread.h> /* get_ncpus */
#include <solidc/threadpool.h>
#include <stdatomic.h>
#include <stdio.h>    /* fprintf, printf, snprintf */
#include <stdlib.h>   /* malloc, free, strdup, atoll */
#include <string.h>   /* memset, strcmp, strcasecmp, strrchr, strlen */
#include <sys/mman.h> /* mmap, munmap, madvise, MAP_FAILED */
#include <sys/stat.h> /* fstat, struct stat */
#include <unistd.h>   /* close, read, sysconf */
#include <vfs.h>

/* =========================================================================
 * SQL
 * ======================================================================= */

/*
 * The ON CONFLICT clause ensures idempotency.  RETURNING id is always
 * populated — either the freshly inserted row or the existing one after
 * the DO UPDATE touches num_pages.  A separate SELECT is never needed.
 */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) "
    "VALUES($1, $2, $3) "
    "ON CONFLICT(name, path) DO UPDATE "
    "  SET num_pages = EXCLUDED.num_pages "
    "RETURNING id";

static const char* all_paths_query = "SELECT path FROM files";

/* Virtual filesystem from main.c (NULL → use host filesystem). */
extern vfs_t* vfs;
extern char* conn_info;

/* Global flag for graceful termination via Ctrl+C / external signal */
static _Atomic bool g_cancel_requested = false;

static void sigint_handler(int sig) {
    (void)sig;
    g_cancel_requested = true;
}

/* =========================================================================
 * Constants
 * ======================================================================= */

#define MAX_PAGE_TEXT_LEN 2047

/*
 * Number of pages accumulated in a single COPY session.  Larger values
 * reduce per-COPY overhead; smaller values improve tail latency when a
 * document has few pages.  4096 pages ≈ 8 MB of text at average page size —
 * well within a single TCP segment burst.
 */
#define COPY_BATCH_PAGES 4096

/*
 * Capacity of the MPMC ring buffer between extractor and writer pools.
 * Must be a power of two.  At COPY_BATCH_PAGES pages/batch and
 * WORKER_COUNT writers, this gives ~256 MB of in-flight text headroom
 * before extractors back-pressure.
 */
#define RING_CAPACITY 64u

/* =========================================================================
 * Per-page extracted text
 * ======================================================================= */

/**
 * A single extracted page owned by a PageBatch arena.
 * The text pointer is valid until the batch's arena is destroyed.
 */
typedef struct {
    int64_t file_id; /**< Foreign key into files table. */
    int page_num;    /**< 1-based page number. */
    /** Null-terminated page text; length ≤ MAX_PAGE_TEXT_LEN. */
    const char* text;
} ExtractedPage;

/**
 * A heap-allocated batch of extracted pages.  Produced by an extractor
 * thread, consumed and freed by a writer thread.
 *
 * The arena owns all text pointers inside pages[].  Writers MUST call
 * page_batch_free() after the COPY completes.
 */
typedef struct {
    ExtractedPage* pages; /**< Array of valid_count entries. */
    int valid_count;
    Arena* arena;         /**< Bump allocator backing all text. */
} PageBatch;

/* Global lock to protect non-thread-safe arena lifecycle operations */
static pthread_mutex_t g_arena_mutex = PTHREAD_MUTEX_INITIALIZER;

static Arena* safe_arena_create(size_t budget) {
    pthread_mutex_lock(&g_arena_mutex);
    Arena* a = arena_create(budget);
    pthread_mutex_unlock(&g_arena_mutex);
    return a;
}

static void safe_arena_destroy(Arena* a) {
    if (!a) return;
    pthread_mutex_lock(&g_arena_mutex);
    arena_destroy(a);
    pthread_mutex_unlock(&g_arena_mutex);
}

static PageBatch* page_batch_create(int capacity) {
    Arena* arena = safe_arena_create((size_t)capacity * (MAX_PAGE_TEXT_LEN + 64));
    if (!arena) return NULL;

    PageBatch* b = malloc(sizeof(*b));
    if (!b) {
        safe_arena_destroy(arena);
        return NULL;
    }

    b->pages = malloc(sizeof(ExtractedPage) * (size_t)capacity);
    if (!b->pages) {
        safe_arena_destroy(arena);
        free(b);
        return NULL;
    }

    b->valid_count = 0;
    b->arena = arena;
    return b;
}

static void page_batch_free(PageBatch* b) {
    if (!b) return;
    safe_arena_destroy(b->arena);
    free(b->pages);
    free(b);
}

/* =========================================================================
 * MPMC ring buffer (lock-based, simple and correct)
 *
 * The ring connects extractor threads (producers) to writer threads
 * (consumers).  A NULL sentinel pushed by the coordinator signals each
 * writer that no more batches will arrive.
 * ======================================================================= */

typedef struct {
    PageBatch* slots[RING_CAPACITY];
    _Atomic uint32_t head; /**< Next write position (producers). */
    _Atomic uint32_t tail; /**< Next read position (consumers).  */
    pthread_mutex_t mu;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    bool closed; /**< Set to true after all producers are done. */
} BatchRing;

static bool ring_push(BatchRing* r, PageBatch* batch) {
    pthread_mutex_lock(&r->mu);
    while (true) {
        if (r->closed || g_cancel_requested) {
            pthread_mutex_unlock(&r->mu);
            return false;
        }

        uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
        uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
        if (h - t < RING_CAPACITY) {
            r->slots[h & (RING_CAPACITY - 1)] = batch;
            atomic_store_explicit(&r->head, h + 1, memory_order_release);
            pthread_cond_signal(&r->not_empty);
            pthread_mutex_unlock(&r->mu);
            return true;
        }
        pthread_cond_wait(&r->not_full, &r->mu);
    }
}

/**
 * Pops the next batch from the ring.  Returns NULL and sets *done=true
 * when the ring is closed and drained.
 */
static PageBatch* ring_pop(BatchRing* r, bool* done) {
    pthread_mutex_lock(&r->mu);
    while (true) {
        if (g_cancel_requested) {
            pthread_mutex_unlock(&r->mu);
            *done = true;
            return NULL;
        }
        uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
        uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
        if (h != t) {
            PageBatch* b = r->slots[t & (RING_CAPACITY - 1)];
            atomic_store_explicit(&r->tail, t + 1, memory_order_release);
            pthread_cond_signal(&r->not_full);
            pthread_mutex_unlock(&r->mu);
            *done = false;
            return b;
        }
        if (r->closed) {
            pthread_mutex_unlock(&r->mu);
            *done = true;
            return NULL;
        }
        pthread_cond_wait(&r->not_empty, &r->mu);
    }
}

static void ring_close(BatchRing* r) {
    pthread_mutex_lock(&r->mu);
    r->closed = true;
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

static void ring_init(BatchRing* r) {
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->not_full, NULL);
    pthread_cond_init(&r->not_empty, NULL);
}

static void ring_destroy(BatchRing* r) {
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->not_full);
    pthread_cond_destroy(&r->not_empty);

    /* Free remaining items in the queue to prevent memory leaks on exit */
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    for (uint32_t i = t; i != h; i++) {
        PageBatch* b = r->slots[i & (RING_CAPACITY - 1)];
        if (b) { page_batch_free(b); }
    }
}

/* =========================================================================
 * File loader (mmap on host, malloc+read on VFS)
 * ======================================================================= */

/**
 * GBytes custom data container; freed via gbytes_file_map_free().
 */
typedef struct {
    void* data;
    size_t size;
    bool is_mmap; /**< If true, data must be munmap'd; else free'd. */
} GBytesFileMap;

static void gbytes_file_map_free(gpointer data) {
    GBytesFileMap* m = data;
    if (!m) return;
    if (m->data) {
        if (m->is_mmap) {
            munmap(m->data, m->size);
        } else {
            free(m->data);
        }
    }
    free(m);
}

/**
 * Loads a file into a GBytesFileMap using mmap when possible.
 *
 * @param path  Absolute path to the file.
 * @return      Heap-allocated GBytesFileMap on success, NULL on error.
 *              Caller must pass to gbytes_file_map_free() or wrap in GBytes.
 */
static GBytesFileMap* load_file_map(const char* path) {
    GBytesFileMap* m = malloc(sizeof(*m));
    if (!m) return NULL;
    *m = (GBytesFileMap){0};

    if (vfs != NULL) {
        m->data = vfs_read_file(vfs, path, &m->size);
        if (!m->data) {
            free(m);
            return NULL;
        }
        m->is_mmap = false;
        return m;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        free(m);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        free(m);
        return NULL;
    }

    m->size = (size_t)st.st_size;
    if (m->size == 0) {
        close(fd);
        m->data = malloc(1);
        if (!m->data) {
            free(m);
            return NULL;
        }
        return m;
    }

    void* map = mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map != MAP_FAILED) {
        madvise(map, m->size, MADV_SEQUENTIAL);
        m->data = map;
        m->is_mmap = true;
    } else {
        /* mmap failed (e.g. tmpfs on some kernels): fall back to read(2). */
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            free(m);
            return NULL;
        }

        void* buf = malloc(m->size);
        if (!buf) {
            close(fd);
            free(m);
            return NULL;
        }

        size_t total = 0;
        while (total < m->size) {
            ssize_t n = read(fd, (char*)buf + total, m->size - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        close(fd);
        m->data = buf;
        m->is_mmap = false;
    }
    return m;
}

/* =========================================================================
 * PostgreSQL COPY helpers (raw libpq — no pgpool wrapper overhead)
 *
 * COPY FROM STDIN row format: tab-separated, newline-terminated.
 * Fields: file_id  page_num  text
 * Text is escaped: backslash, tab, and newline → \\ \t \n.
 * ======================================================================= */

/**
 * Escapes a string for PostgreSQL text-format COPY (in-place acceptable
 * since we're writing to a separate scratch buffer anyway).
 *
 * @param src   Source string (null-terminated).
 * @param dst   Destination buffer.
 * @param cap   Capacity of dst in bytes.
 * @return      Number of bytes written (not including null terminator),
 *              or -1 if dst was too small.
 */
static ssize_t copy_escape(const char* src, char* dst, size_t cap) {
    size_t out = 0;
    for (const char* p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        /* Each character expands to at most 2 bytes. */
        if (out + 2 >= cap) return -1;
        if (c == '\\') {
            dst[out++] = '\\';
            dst[out++] = '\\';
        } else if (c == '\t') {
            dst[out++] = '\\';
            dst[out++] = 't';
        } else if (c == '\n') {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if (c == '\r') {
            dst[out++] = '\\';
            dst[out++] = 'r';
        } else {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
    return (ssize_t)out;
}

/*
 * Enough for one COPY row: 20 (file_id) + 1 + 12 (page_num) + 1 +
 * MAX_PAGE_TEXT_LEN*2 (worst-case escaped) + 2 (newline + NUL).
 */
#define COPY_ROW_BUF (MAX_PAGE_TEXT_LEN * 2 + 64)

/**
 * Bulk-inserts all pages in a batch using PostgreSQL COPY FROM STDIN.
 * Opens and finalises the COPY stream in one call.
 *
 * @param conn        A libpq connection in autocommit mode (or an active txn).
 * @param batch       Batch of pages to ingest.  Not modified.
 * @return            true on success, false on any DB error.
 */
static bool bulk_copy_pages(PGconn* conn, const PageBatch* batch) {
    if (batch->valid_count == 0) return true;

    PGresult* res = PQexec(conn, "COPY pages(file_id, page_num, text) FROM STDIN");
    if (PQresultStatus(res) != PGRES_COPY_IN) {
        fprintf(stderr, ">>> COPY start failed: %s\n", PQerrorMessage(conn));
        PQclear(res);
        return false;
    }
    PQclear(res);

    char row[COPY_ROW_BUF];
    char escaped[MAX_PAGE_TEXT_LEN * 2 + 4];

    for (int i = 0; i < batch->valid_count; i++) {
        const ExtractedPage* p = &batch->pages[i];

        if (copy_escape(p->text, escaped, sizeof(escaped)) < 0) {
            /* Text too long after escaping — truncate at the safe boundary. */
            escaped[sizeof(escaped) - 1] = '\0';
        }

        int len = snprintf(row, sizeof(row), "%" PRId64 "\t%d\t%s\n", p->file_id, p->page_num, escaped);
        if (len <= 0 || (size_t)len >= sizeof(row)) {
            fprintf(stderr, ">>> Row format overflow for file_id=%" PRId64 " page %d — skipping\n", p->file_id,
                    p->page_num);
            continue;
        }

        if (PQputCopyData(conn, row, len) != 1) {
            fprintf(stderr, ">>> PQputCopyData failed: %s\n", PQerrorMessage(conn));
            PQputCopyEnd(conn, "client error");
            return false;
        }
    }

    if (PQputCopyEnd(conn, NULL) != 1) {
        fprintf(stderr, ">>> PQputCopyEnd failed: %s\n", PQerrorMessage(conn));
        return false;
    }

    res = PQgetResult(conn);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) { fprintf(stderr, ">>> COPY finalise failed: %s\n", PQresultErrorMessage(res)); }
    PQclear(res);
    return ok;
}

/* =========================================================================
 * Path snapshot (dedup before file I/O)
 * ======================================================================= */

typedef struct {
    HashMap* map;
    Arena* arena;
    bool populated;
} PathSnapshot;

/**
 * Loads all known file paths into a hash map for O(1) dedup lookup.
 * Uses a single query; the map is read-only after this call.
 *
 * @param snap  Output snapshot; caller must call path_snapshot_destroy().
 * @param conn  A valid libpq connection.
 */
static void load_path_snapshot(PathSnapshot* snap, PGconn* conn) {
    memset(snap, 0, sizeof(*snap));

    snap->arena = safe_arena_create(0);
    if (!snap->arena) {
        fprintf(stderr, ">>> OOM creating path snapshot arena\n");
        return;
    }

    snap->map = map_create(MapConfigStr);
    if (!snap->map) {
        fprintf(stderr, ">>> OOM creating path snapshot map\n");
        arena_destroy(snap->arena);
        snap->arena = NULL;
        return;
    }

    PGresult* res = PQexec(conn, all_paths_query);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, ">>> Failed to fetch existing paths: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        return;
    }

    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; i++) {
        const char* path = PQgetvalue(res, i, 0);
        if (!path) continue;
        char* key = arena_strdup(snap->arena, path);
        if (!key) continue;
        map_set(snap->map, key, strlen(key), key);
    }
    PQclear(res);
    snap->populated = true;
    printf(">>> Loaded %d existing file paths\n", nrows);
}

static void path_snapshot_destroy(PathSnapshot* snap) {
    if (snap->map) map_destroy(snap->map);
    if (snap->arena) safe_arena_destroy(snap->arena);
    memset(snap, 0, sizeof(*snap));
}

/* =========================================================================
 * Shared run-time state
 * ======================================================================= */

/**
 * State shared between the coordinator, extractor threads, and writer
 * threads.  Allocated on the stack of process_pdfs() and passed by pointer.
 */
typedef struct {
    BatchRing ring;       /**< MPMC queue: extractors → writers. */
    _Atomic bool success; /**< Cleared on any unrecoverable error. */
    const PathSnapshot* snapshot;
    bool dryrun;
    int min_pages;
    /*
     * DB connection string kept for writer threads.  Each writer opens its
     * own PGconn so that COPY streams never interleave.
     */
    const char* conn_info;
    _Atomic int active_extractors; /**< Decremented as extractors finish. */
} RunState;

/* =========================================================================
 * Extractor task (submitted to Threadpool)
 *
 * Each task:
 *   1. Loads and mmaps the PDF
 *   2. Opens it with Poppler
 *   3. Extracts and cleans all page text into a fresh PageBatch
 *   4. Pushes the batch into the ring for a writer to consume
 *
 * The task never touches the database.
 * ======================================================================= */

typedef struct {
    char* path;      /**< heap-owned; freed by extractor task. */
    char* name;      /**< heap-owned; freed by extractor task. */
    int64_t file_id; /**< Retrieved by coordinator before submission. */
    RunState* state;
} ExtractorTask;

static void extractor_task(void* arg) {
    ExtractorTask* task = arg;
    RunState* state = task->state;
    char* path = task->path;
    char* name = task->name;
    int64_t fid = task->file_id;
    free(task); /* task struct is consumed immediately */

    GBytesFileMap* m = NULL;
    GBytes* gbytes = NULL;
    PopplerDocument* doc = NULL;
    PageBatch* batch = NULL;

    if (g_cancel_requested || !atomic_load(&state->success)) { goto done; }

    m = load_file_map(path);
    if (!m) {
        fprintf(stderr, ">>> Failed to load %s\n", path);
        atomic_store(&state->success, false);
        goto done;
    }

    gbytes = g_bytes_new_with_free_func(m->data, m->size, gbytes_file_map_free, m);
    if (!gbytes) {
        gbytes_file_map_free(m);
        atomic_store(&state->success, false);
        goto done;
    }
    m = NULL; /* ownership transferred to gbytes */

    GError* gerr = NULL;
    doc = poppler_document_new_from_bytes(gbytes, NULL, &gerr);
    g_bytes_unref(gbytes);
    gbytes = NULL;

    if (!doc) {
        fprintf(stderr, ">>> Poppler open failed for %s: %s\n", path, gerr ? gerr->message : "(unknown)");
        if (gerr) g_error_free(gerr);
        atomic_store(&state->success, false);
        goto done;
    }

    int npages = poppler_document_get_n_pages(doc);
    if (npages <= 0 || npages < state->min_pages) goto done;

    if (state->dryrun) {
        printf(">>> Found %s (%d pages)\n", path, npages);
        goto done;
    }

    batch = page_batch_create(npages);
    if (!batch) {
        fprintf(stderr, ">>> OOM allocating batch for %s\n", name);
        atomic_store(&state->success, false);
        goto done;
    }

    /* Extract and clean all pages into the batch arena. */
    for (int i = 0; i < npages; i++) {
        if (g_cancel_requested || !atomic_load(&state->success)) { break; }

        PopplerPage* page = poppler_document_get_page(doc, i);
        if (!page) continue;

        char* raw = poppler_page_get_text(page);
        g_object_unref(page);

        if (!raw || raw[0] == '\0') {
            free(raw);
            continue;
        }

        size_t len = strlen(raw);
        if (len >= (size_t)MAX_PAGE_TEXT_LEN) {
            raw[MAX_PAGE_TEXT_LEN - 1] = '\0';
            len = (size_t)MAX_PAGE_TEXT_LEN - 1;
        }
        pdf_text_clean(raw, len, false);

        const char* text = arena_strdup(batch->arena, raw);
        free(raw);
        if (!text) continue;

        int idx = batch->valid_count;
        batch->pages[idx].file_id = fid;
        batch->pages[idx].page_num = i + 1;
        batch->pages[idx].text = text;
        batch->valid_count++;
    }

    if (batch->valid_count > 0 && !g_cancel_requested && atomic_load(&state->success)) {
        if (!ring_push(&state->ring, batch)) {
            page_batch_free(batch);
        } else {
            batch = NULL; /* ownership transferred to ring */
            printf(">>> Extracted %s (%d pages)\n", path, npages);
        }
    }

done:
    if (batch) page_batch_free(batch);
    if (doc) g_object_unref(doc);
    if (gbytes) g_bytes_unref(gbytes);
    free(path);
    free(name);
}

/* =========================================================================
 * Writer thread
 *
 * Each writer opens its own PGconn, drains batches from the ring, and
 * bulk-inserts them via COPY.  Writers run as native pthreads (not in the
 * Threadpool) so they can block indefinitely on ring_pop without consuming
 * a threadpool worker slot.
 * ======================================================================= */

typedef struct {
    RunState* state;
} WriterArg;

static void* writer_thread(void* arg) {
    WriterArg* wa = arg;
    RunState* state = wa->state;
    free(wa);

    PGconn* conn = PQconnectdb(state->conn_info);
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, ">>> Writer: DB connect failed: %s\n", conn ? PQerrorMessage(conn) : "(OOM)");
        if (conn) PQfinish(conn);
        atomic_store(&state->success, false);
        return NULL;
    }

    /* COPY is not transactional per-row — on retry or partial failure the
     * ON CONFLICT DO NOTHING on the INSERT covers idempotency for files;
     * for pages we rely on the unique index on (file_id, page_num). */
    PQexec(conn, "SET synchronous_commit = off");

    bool done = false;
    while (true) {
        if (g_cancel_requested) break;

        PageBatch* batch = ring_pop(&state->ring, &done);
        if (done) break;
        if (!batch) continue;

        if (!bulk_copy_pages(conn, batch)) {
            fprintf(stderr, ">>> Writer: COPY failed — batch dropped\n");
            atomic_store(&state->success, false);
        }
        page_batch_free(batch);
    }

    PQfinish(conn);
    return NULL;
}

/* =========================================================================
 * Coordinator: inserts file records and submits extractor tasks
 *
 * The coordinator holds a single pgpool connection used only for the
 * lightweight file INSERT (RETURNING id).  All heavy page I/O and DB
 * writes are off-loaded to the pool threads.
 * ======================================================================= */

typedef struct {
    Threadpool* thpool;
    RunState* state;
    pgconn_t* coord_conn; /**< pgpool connection for file INSERTs. */
} WalkState;

/**
 * Inserts a file record and returns its id, or -1 on error.
 * Uses pgpool to reuse the coordinator's existing connection wrapper.
 *
 * @param conn      Coordinator's pgpool connection.
 * @param name      File base name.
 * @param path      Absolute path.
 * @param npages    Total number of pages.
 * @return          The file id, or -1 on failure.
 */
static int64_t upsert_file_record(pgconn_t* conn, const char* name, const char* path, int npages) {
    char npages_str[16];
    snprintf(npages_str, sizeof(npages_str), "%d", npages);
    const char* params[] = {name, path, npages_str};

    PGresult* res = pgpool_query_params(conn, file_insert_query, 3, params, -1);
    if (!res) {
        fprintf(stderr, ">>> File INSERT failed for %s: %s\n", path, pgpool_error_message(conn));
        return -1;
    }

    int64_t fid = -1;
    if (PQntuples(res) > 0) {
        fid = atoll(PQgetvalue(res, 0, 0));
    } else {
        fprintf(stderr, ">>> File INSERT returned no rows for %s\n", path);
    }
    PQclear(res);
    return fid;
}

/**
 * Submits an extractor task for the given PDF after upserting its file
 * record.  Called from both the VFS and host-filesystem walk callbacks.
 *
 * @return true to continue the walk, false to stop on allocation failure.
 */
static bool submit_extractor(const char* path, const char* name, int npages, WalkState* ws) {
    int64_t fid = upsert_file_record(ws->coord_conn, name, path, npages);
    if (fid < 0) {
        atomic_store(&ws->state->success, false);
        return true; /* non-fatal: skip this file, continue walk */
    }

    ExtractorTask* task = malloc(sizeof(*task));
    if (!task) {
        fprintf(stderr, ">>> OOM allocating task for %s\n", path);
        atomic_store(&ws->state->success, false);
        return false;
    }

    *task = (ExtractorTask){
        .path = strdup(path),
        .name = strdup(name),
        .file_id = fid,
        .state = ws->state,
    };
    if (!task->path || !task->name) {
        free(task->path);
        free(task->name);
        free(task);
        atomic_store(&ws->state->success, false);
        return false;
    }

    threadpool_submit(ws->thpool, extractor_task, task);
    return true;
}

/* =========================================================================
 * Walk callbacks
 * ======================================================================= */

/*
 * Probe the page count cheaply without loading the full PDF into Poppler.
 * We open the file, hand it to Poppler, check the page count, and close it.
 * This is only done in the walk callback (coordinator thread) where we have
 * the pgpool connection; the extractor will re-open for actual extraction.
 *
 * On most systems this is fast because the OS page cache retains the mmap
 * for the immediately subsequent extractor open.
 */
static int probe_page_count(const char* path) {
    if (g_cancel_requested) return -1;

    GBytesFileMap* m = load_file_map(path);
    if (!m) return -1;

    GBytes* gb = g_bytes_new_with_free_func(m->data, m->size, gbytes_file_map_free, m);
    if (!gb) {
        gbytes_file_map_free(m);
        return -1;
    }

    GError* err = NULL;
    PopplerDocument* doc = poppler_document_new_from_bytes(gb, NULL, &err);
    g_bytes_unref(gb);

    if (!doc) {
        if (err) g_error_free(err);
        return -1;
    }
    int n = poppler_document_get_n_pages(doc);
    g_object_unref(doc);
    return n;
}

static bool vfs_list_callback(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    WalkState* ws = userdata;

    if (g_cancel_requested || !atomic_load(&ws->state->success)) return false;

    const char* ext = strrchr(path, '.');
    if (!ext || ext == path) return true;
    if (strcasecmp(ext + 1, "pdf") != 0) return true;

    /* Dedup check before any file I/O. */
    if (ws->state->snapshot->populated && map_get(ws->state->snapshot->map, (void*)path, strlen(path))) { return true; }

    if (ws->state->dryrun) {
        const char* base = strrchr(path, '/');
        printf(">>> Found %s\n", base ? base + 1 : path);
        return true;
    }

    const char* base = strrchr(path, '/');
    const char* name = base ? base + 1 : path;

    int npages = probe_page_count(path);
    if (npages < ws->state->min_pages) return true;

    return submit_extractor(path, name, npages, ws);
}

static const char* const skip_dirs[] = {
    "node_modules", ".git",   ".svn",     ".hg",    "__pycache__", ".pytest_cache", ".mypy_cache", ".tox",    "venv",
    ".venv",        "env",    ".env",     "vendor", "build",       "dist",          "target",      ".gradle", ".idea",
    ".vscode",      ".cache", "coverage", ".next",  ".nuxt",       ".turbo",        ".DS_Store",   NULL,
};

static WalkDirOption walk_dir_callback(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;

    if (g_cancel_requested || !atomic_load(&((WalkState*)userdata)->state->success)) return DirStop;

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) return DirSkip;
    }

    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) return DirContinue;
    if (strcasecmp(ext + 1, "pdf") != 0) return DirContinue;

    WalkState* ws = userdata;

    /* Dedup check before any file I/O. */
    if (ws->state->snapshot->populated && map_get(ws->state->snapshot->map, (void*)path, strlen(path))) {
        return DirContinue;
    }

    if (ws->state->dryrun) {
        printf(">>> Found %s\n", name);
        return DirContinue;
    }

    int npages = probe_page_count(path);
    if (npages < ws->state->min_pages) return DirContinue;

    return submit_extractor(path, name, npages, ws) ? DirContinue : DirStop;
}

/* =========================================================================
 * Public entry point
 * ======================================================================= */

bool process_pdfs(const char* root_dir, int min_pages, bool dryrun) {
    /*
     * Determine thread counts.
     *   Extractors: one per logical CPU — Poppler is CPU-bound.
     *   Writers:    WORKER_COUNT — bounded by max DB connections.
     */
    int nprocs = get_ncpus();
    if (nprocs < 1) nprocs = WORKER_COUNT;
    int n_extractors = nprocs;
    int n_writers = dryrun ? 0 : WORKER_COUNT;

    /* Set up clean signal handling for interruption. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    struct sigaction old_int, old_term;
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    defer {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
    };

    /*
     * Acquire the coordinator's pgpool connection (used only for file
     * INSERTs — the lightest possible DB workload).
     */
    pgconn_t* coord_conn = NULL;
    if (!dryrun) {
        coord_conn = pgpool_acquire(pool, 10000);
        if (!coord_conn) {
            fprintf(stderr, ">>> Failed to acquire coordinator DB connection\n");
            return false;
        }
    }
    defer {
        if (coord_conn) pgpool_release(pool, coord_conn);
    };

    PathSnapshot snapshot = {0};
    if (!dryrun) {
        /*
         * Load via the raw PGconn underlying the pgpool slot so we can
         * reuse the existing connection without opening an extra one.
         */
        load_path_snapshot(&snapshot, pgpool_get_raw_connection(coord_conn));
    }
    defer {
        path_snapshot_destroy(&snapshot);
    };

    /* Build the connection string once; writer threads use it to open their
     * own independent PGconns so COPY streams never share a socket. */
    RunState state = {
        .success = true,
        .snapshot = &snapshot,
        .dryrun = dryrun,
        .min_pages = min_pages,
        .conn_info = conn_info,
        .active_extractors = (int)n_extractors,
    };
    ring_init(&state.ring);
    defer {
        ring_destroy(&state.ring);
    };

    /* ── Start writer threads ── */
    pthread_t* writers = NULL;
    if (n_writers > 0) {
        writers = malloc(sizeof(pthread_t) * (size_t)n_writers);
        if (!writers) {
            fprintf(stderr, ">>> OOM allocating writer thread array\n");
            return false;
        }
        for (int i = 0; i < n_writers; i++) {
            WriterArg* wa = malloc(sizeof(*wa));
            if (!wa) {
                fprintf(stderr, ">>> OOM allocating writer arg %d\n", i);
                atomic_store(&state.success, false);
                n_writers = i;
                break;
            }
            wa->state = &state;
            if (pthread_create(&writers[i], NULL, writer_thread, wa) != 0) {
                fprintf(stderr, ">>> Failed to start writer thread %d\n", i);
                free(wa);
                atomic_store(&state.success, false);
                n_writers = i;
                break;
            }
        }
    }
    defer {
        if (writers) free(writers);
    };

    /* ── Start extractor pool ── */
    Threadpool* thpool = threadpool_create((size_t)n_extractors);
    if (!thpool) {
        fprintf(stderr, ">>> Failed to create extractor threadpool\n");
        ring_close(&state.ring);
        for (int i = 0; i < n_writers; i++)
            pthread_join(writers[i], NULL);
        return false;
    }
    defer {
        threadpool_destroy(thpool, 120000);
    };

    WalkState ws = {
        .thpool = thpool,
        .state = &state,
        .coord_conn = coord_conn,
    };

    /* ── Walk the file tree ── */
    if (vfs != NULL) {
        if (dryrun) printf("Performing VFS index dry run\n");
        vfs_list(vfs, NULL, vfs_list_callback, &ws);
    } else {
        if (dryrun) printf("Performing host index dry run on %s\n", root_dir);
        int rc = dir_walk(root_dir, walk_dir_callback, &ws);
        if (rc != 0) atomic_store(&state.success, false);
    }

    /*
     * If aborted early (e.g. by Ctrl+C or a writer thread connection failure),
     * close the ring immediately. This prevents extractor tasks from blocking
     * indefinitely in ring_push if the ring buffer becomes full.
     */
    if (g_cancel_requested || !atomic_load(&state.success)) { ring_close(&state.ring); }

    /*
     * Wait for all extractor tasks to finish, then close the ring so
     * writer threads know no more batches are coming.
     */
    threadpool_wait(thpool);
    ring_close(&state.ring);

    /* ── Join writer threads ── */
    for (int i = 0; i < n_writers; i++) {
        pthread_join(writers[i], NULL);
    }

    return atomic_load(&state.success) && !g_cancel_requested;
}
