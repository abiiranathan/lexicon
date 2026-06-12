#include "../include/pdf_indexer.h"
#include "../include/pdf.h"
#include "../include/pdf_preprocess.h"

#include <fcntl.h>     // for open, O_RDONLY
#include <inttypes.h>  // for PRId64, PRIu64
#include <poppler/glib/poppler.h>
#include <pthread.h>   // for pthread_mutex_t, pthread_cond_t
#include <solidc/arena.h>
#include <solidc/defer.h>
#include <solidc/map.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>
#include <stdio.h>     // for fprintf, printf, snprintf
#include <stdlib.h>    // for malloc, free, strdup, atoll
#include <string.h>    // for strcmp, strcasecmp, strrchr, strlen, memset
#include <sys/mman.h>  // for mmap, munmap, madvise, MAP_FAILED
#include <sys/stat.h>  // for fstat, struct stat
#include <unistd.h>    // for close, read, sysconf
#include <vfs.h>

/* =========================================================================
 * SQL queries
 * ======================================================================= */

static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) "
    "VALUES($1, $2, $3) "
    "ON CONFLICT(name, path) DO UPDATE "
    "  SET num_pages     = EXCLUDED.num_pages "
    "RETURNING id";

static const char* all_paths_query = "SELECT path FROM files";
static const char* file_id_query = "SELECT id FROM files WHERE path = $1";

// Virtual file system from main.c (NULL -> use host filesystem).
extern vfs_t* vfs;

#define MAX_PAGE_TEXT_LEN 2047
#define PAGE_BATCH_SIZE   1000

typedef struct {
    pgconn_t* conn;
    bool in_use;
} WorkerSlot;

static WorkerSlot g_worker_slots[WORKER_COUNT];

// POSIX Mutex and Condition Variable for CPU-friendly connection pooling
static pthread_mutex_t g_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    int page_num;
    const char* text;
} ExtractedPage;

typedef struct {
    HashMap* map;
    Arena* arena;
    bool populated;
} PathSnapshot;

typedef struct {
    char* path;
    char* name;
    _Atomic bool* success;
    bool dryrun;
    int min_pages;
    const PathSnapshot* snapshot;
} PDFTaskParams;

typedef struct {
    Threadpool* thpool;
    _Atomic bool* success;
    bool dryrun;
    int min_pages;
    const PathSnapshot* snapshot;
} WalkDirUserData;

/**
 * Custom mapping container designed to be managed by GBytes
 */
typedef struct {
    void* data;
    size_t size;
    bool is_mmap;
} GBytesFileMap;

/* =========================================================================
 * Connection leasing (Kernel-level sleep, 0% CPU consumption while waiting)
 * ======================================================================= */

static pgconn_t* db_conn_acquire(int* out_slot_idx) {
    pthread_mutex_lock(&g_pool_mutex);
    while (true) {
        for (int i = 0; i < WORKER_COUNT; i++) {
            if (!g_worker_slots[i].in_use) {
                g_worker_slots[i].in_use = true;
                *out_slot_idx = i;
                pgconn_t* conn = g_worker_slots[i].conn;
                pthread_mutex_unlock(&g_pool_mutex);
                return conn;
            }
        }
        // No connections are free. Suspend this thread completely.
        pthread_cond_wait(&g_pool_cond, &g_pool_mutex);
    }
}

static void db_conn_release(int slot_idx) {
    if (slot_idx >= 0 && slot_idx < WORKER_COUNT) {
        pthread_mutex_lock(&g_pool_mutex);
        g_worker_slots[slot_idx].in_use = false;
        // Wake up exactly one thread waiting for a connection
        pthread_cond_signal(&g_pool_cond);
        pthread_mutex_unlock(&g_pool_mutex);
    }
}

/* =========================================================================
 * File Loader (using GBytes lifecycle)
 * ======================================================================= */

static void* vfs_read_file(vfs_t* fs, const char* path, size_t* size) {
    vfs_fd_t fd = vfs_fopen(fs, path, VFS_O_RDONLY);
    if (fd < 0) return NULL;
    defer {
        vfs_fclose(fs, fd);
    };

    vfs_stat_t st;
    if (vfs_stat(fs, path, &st) != VFS_OK) return NULL;

    if (st.size == 0) {
        *size = 0;
        return malloc(1);
    }

    void* data = malloc(st.size);
    if (!data) return NULL;

    size_t bytes_read = 0;
    if (vfs_fread(fs, fd, data, st.size, &bytes_read) != VFS_OK) {
        free(data);
        return NULL;
    }

    *size = bytes_read;
    return data;
}

static void gbytes_file_map_free(gpointer data) {
    GBytesFileMap* m = data;
    if (m) {
        if (m->data) {
            if (m->is_mmap) {
                munmap(m->data, m->size);
            } else {
                free(m->data);
            }
        }
        free(m);
    }
}

static GBytesFileMap* load_file_map(const char* path) {
    GBytesFileMap* m = malloc(sizeof(*m));
    if (!m) return NULL;

    m->data = NULL;
    m->size = 0;
    m->is_mmap = false;

    if (vfs != NULL) {
        size_t sz = 0;
        void* d = vfs_read_file(vfs, path, &sz);
        if (!d) {
            free(m);
            return NULL;
        }
        m->data = d;
        m->size = sz;
        m->is_mmap = false;
    } else {
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
            m->is_mmap = false;
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
            size_t total_read = 0;
            while (total_read < m->size) {
                ssize_t n = read(fd, (char*)buf + total_read, m->size - total_read);
                if (n <= 0) break;
                total_read += (size_t)n;
            }
            close(fd);
            m->data = buf;
            m->is_mmap = false;
        }
    }

    return m;
}

/* =========================================================================
 * Extraction and Insertion processing
 * ======================================================================= */

static int extract_pdf_pages(PopplerDocument* doc, int npages, const char* name, ExtractedPage* out_pages,
                             Arena* arena) {
    int valid_pages = 0;
    for (int i = 0; i < npages; i++) {
        PopplerPage* page = poppler_document_get_page(doc, i);
        if (!page) {
            fprintf(stderr, ">>> Failed to get page %d of %s\n", i + 1, name);
            continue;
        }

        char* raw = poppler_page_get_text(page);
        g_object_unref(page);

        if (!raw) continue;
        if (raw[0] == '\0') {
            free(raw);
            continue;
        }

        size_t len = strlen(raw);
        if (len >= (size_t)MAX_PAGE_TEXT_LEN) {
            raw[MAX_PAGE_TEXT_LEN - 1] = '\0';
            len = (size_t)MAX_PAGE_TEXT_LEN - 1;
        }
        pdf_text_clean(raw, len, false);

        char* text = arena_strdup(arena, raw);
        free(raw);
        if (!text) continue;

        out_pages[valid_pages].page_num = i + 1;
        out_pages[valid_pages].text = text;
        valid_pages++;
    }
    return valid_pages;
}

static bool process_all_pages_from_memory(int64_t file_id, const char* name, const ExtractedPage* pages,
                                          int valid_pages, pgconn_t* conn, Arena* scratch) {
    if (valid_pages <= 0) return true;

    for (int start = 0; start < valid_pages; start += PAGE_BATCH_SIZE) {
        int batch_size = valid_pages - start;
        if (batch_size > PAGE_BATCH_SIZE) { batch_size = PAGE_BATCH_SIZE; }

        int nparams = 1 + batch_size * 2;
        const char** param_values = arena_alloc(scratch, sizeof(char*) * (size_t)nparams);
        if (!param_values) return false;

        char* file_id_str = arena_alloc(scratch, 32);
        if (!file_id_str) return false;
        snprintf(file_id_str, 32, "%" PRId64, file_id);
        param_values[0] = file_id_str;

        size_t query_cap = 128 + (size_t)batch_size * 24;
        char* query = arena_alloc(scratch, query_cap);
        if (!query) return false;

        size_t pos = 0;
        pos += (size_t)snprintf(query + pos, query_cap - pos, "INSERT INTO pages(file_id,page_num,text) VALUES ");

        for (int k = 0; k < batch_size; k++) {
            int idx = start + k;
            int pnum_slot = 2 + k * 2;
            int text_slot = 2 + k * 2 + 1;

            pos += (size_t)snprintf(query + pos, query_cap - pos, "($1,$%d,$%d)%s", pnum_slot, text_slot,
                                    (k < batch_size - 1) ? "," : "");

            char* pnum_str = arena_alloc(scratch, 16);
            if (!pnum_str) return false;
            snprintf(pnum_str, 16, "%d", pages[idx].page_num);

            param_values[pnum_slot - 1] = pnum_str;
            param_values[text_slot - 1] = pages[idx].text;
        }

        pos += (size_t)snprintf(query + pos, query_cap - pos, " ON CONFLICT (file_id,page_num) DO NOTHING");

        if (pos >= query_cap) {
            fprintf(stderr, ">>> Query buffer overflow during page insert batch\n");
            return false;
        }

        bool ok = pgpool_execute_params(conn, query, nparams, param_values, -1);
        if (!ok) {
            fprintf(stderr, ">>> Batch page INSERT failed for %s: %s\n", name, pgpool_error_message(conn));
            return false;
        }
    }

    return true;
}

/* =========================================================================
 * PDF worker task
 * ======================================================================= */

static void process_pdf_task(void* arg) {
    PDFTaskParams* params = arg;
    PopplerDocument* doc = NULL;
    GBytes* file_bytes = NULL;
    pgconn_t* conn = NULL;
    int conn_slot_idx = -1;
    Arena* scratch = NULL;
    PGresult* res = NULL;
    bool in_txn = false;
    int npages = 0;
    ExtractedPage* extracted_pages = NULL;
    int valid_pages = 0;

    // Step 1: Fast-path path existence check (No file loaded, zero file I/O)
    if (params->snapshot->populated) {
        if (map_get(params->snapshot->map, params->path, strlen(params->path))) {
            goto cleanup;  // Path already exists in DB; skip entirely
        }
    }

    // Step 2: Load file mapping (Only reached if the path is brand new)
    GBytesFileMap* m = load_file_map(params->path);
    if (!m) {
        fprintf(stderr, ">>> Failed to load %s -- skipping\n", params->path);
        atomic_store(params->success, false);
        goto cleanup;
    }

    // Wrap the mapping inside a reference-counted GBytes object.
    file_bytes = g_bytes_new_with_free_func(m->data, m->size, gbytes_file_map_free, m);
    if (!file_bytes) {
        gbytes_file_map_free(m);
        atomic_store(params->success, false);
        goto cleanup;
    }

    // Open Poppler Document safely using GBytes
    GError* error = NULL;
    doc = poppler_document_new_from_bytes(file_bytes, NULL, &error);

    g_bytes_unref(file_bytes);
    file_bytes = NULL;

    if (!doc) {
        fprintf(stderr, ">>> Failed to open PDF: %s (GError: %s)\n", params->path, error ? error->message : "Unknown");
        if (error) g_error_free(error);
        atomic_store(params->success, false);
        goto cleanup;
    }

    npages = poppler_document_get_n_pages(doc);
    if (npages == 0 || npages < params->min_pages) { goto cleanup; }

    if (params->dryrun) {
        printf(">>> Found %s (%d pages)\n", params->path, npages);
        goto cleanup;
    }

    scratch = arena_create(1024 * 1024);
    if (!scratch) {
        fprintf(stderr, ">>> OOM creating scratch arena for %s\n", params->name);
        atomic_store(params->success, false);
        goto cleanup;
    }

    extracted_pages = arena_alloc(scratch, sizeof(ExtractedPage) * (size_t)npages);
    if (!extracted_pages) {
        fprintf(stderr, ">>> OOM allocating page array for %s\n", params->name);
        atomic_store(params->success, false);
        goto cleanup;
    }

    // Perform text extraction in user-space
    valid_pages = extract_pdf_pages(doc, npages, params->name, extracted_pages, scratch);
    if (valid_pages < 0) {
        atomic_store(params->success, false);
        goto cleanup;
    }

    g_object_unref(doc);
    doc = NULL;

    if (valid_pages == 0) { goto cleanup; }

    // Acquire leased connection (blocks safely with 0% CPU consumption if empty)
    conn = db_conn_acquire(&conn_slot_idx);
    if (!conn) {
        fprintf(stderr, ">>> No DB connection available for %s\n", params->name);
        atomic_store(params->success, false);
        goto cleanup;
    }

    if (!pgpool_begin(conn, 5000)) {
        fprintf(stderr, ">>> BEGIN failed for %s: %s\n", params->name, pgpool_error_message(conn));
        atomic_store(params->success, false);
        goto cleanup;
    }
    in_txn = true;

    {
        char npages_str[16];
        snprintf(npages_str, sizeof(npages_str), "%d", npages);
        const char* file_values[] = {params->name, params->path, npages_str};

        res = pgpool_query_params(conn, file_insert_query, 3, file_values, -1);
        if (!res) {
            fprintf(stderr, ">>> File INSERT failed for %s: %s\n", params->path, pgpool_error_message(conn));
            atomic_store(params->success, false);
            goto cleanup;
        }
    }

    int64_t file_id = 0;
    if (PQntuples(res) > 0) {
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
        res = NULL;
    } else {
        PQclear(res);
        res = NULL;

        const char* path_param[] = {params->path};
        res = pgpool_query_params(conn, file_id_query, 1, path_param, -1);
        if (!res || PQntuples(res) == 0) {
            fprintf(stderr, ">>> Could not retrieve file_id for %s\n", params->path);
            atomic_store(params->success, false);
            goto cleanup;
        }
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
        res = NULL;
    }

    if (!process_all_pages_from_memory(file_id, params->name, extracted_pages, valid_pages, conn, scratch)) {
        fprintf(stderr, ">>> Rolling back %s (page errors)\n", params->name);
        atomic_store(params->success, false);
        goto cleanup;
    }

    if (!pgpool_commit(conn, 15000)) {
        fprintf(stderr, ">>> COMMIT failed for %s: %s\n", params->name, pgpool_error_message(conn));
        atomic_store(params->success, false);
        goto cleanup;
    }

    in_txn = false;
    printf(">>> Indexed %s (%d pages)\n", params->path, npages);

cleanup:
    if (res) PQclear(res);
    if (in_txn && conn) pgpool_rollback(conn, 15000);
    db_conn_release(conn_slot_idx);
    if (scratch) arena_destroy(scratch);
    if (doc) g_object_unref(doc);
    if (file_bytes) g_bytes_unref(file_bytes);
    free(params->path);
    free(params->name);
    free(params);
}

/* =========================================================================
 * Walk / scan callbacks
 * ======================================================================= */

static bool submit_pdf_task(const char* path, const char* name, WalkDirUserData* data) {
    PDFTaskParams* params = malloc(sizeof(*params));
    if (!params) {
        fprintf(stderr, ">>> OOM allocating task for %s\n", path);
        atomic_store(data->success, false);
        return false;
    }

    *params = (PDFTaskParams){0};
    params->path = strdup(path);
    params->name = strdup(name);
    params->success = data->success;
    params->dryrun = data->dryrun;
    params->min_pages = data->min_pages;
    params->snapshot = data->snapshot;

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

static WalkDirOption walk_dir_callback(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;

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
 * Path snapshot loader
 * ======================================================================= */

static void load_path_snapshot(PathSnapshot* snap, pgconn_t* conn) {
    memset(snap, 0, sizeof(*snap));

    snap->arena = arena_create(0);
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

    PGresult* res = pgpool_query_params(conn, all_paths_query, 0, NULL, -1);
    if (!res) {
        fprintf(stderr, ">>> Failed to fetch existing paths: %s\n", pgpool_error_message(conn));
        return;
    }

    int nrows = PQntuples(res);
    for (int i = 0; i < nrows; i++) {
        const char* path = PQgetvalue(res, i, 0);
        if (!path) continue;

        char* key = arena_strdup(snap->arena, path);
        if (!key) continue;

        // Since we only query for path existence, mapping key to key is sufficient
        map_set(snap->map, key, strlen(key), key);
    }

    PQclear(res);
    snap->populated = true;
    printf(">>> Loaded %d existing file paths\n", nrows);
}

static void path_snapshot_destroy(PathSnapshot* snap) {
    if (snap->map) map_destroy(snap->map);
    if (snap->arena) arena_destroy(snap->arena);
    memset(snap, 0, sizeof(*snap));
}

/* =========================================================================
 * Public entry point
 * ======================================================================= */

bool process_pdfs(const char* root_dir, int min_pages, bool dryrun) {
    _Atomic bool success = true;

    if (!dryrun) {
        for (int i = 0; i < WORKER_COUNT; i++) {
            g_worker_slots[i].in_use = false;
            g_worker_slots[i].conn = pgpool_acquire(pool, 10000);
            if (!g_worker_slots[i].conn) {
                fprintf(stderr, ">>> Failed to acquire DB connection for worker %d\n", i);
                for (int j = 0; j < i; j++) {
                    pgpool_release(pool, g_worker_slots[j].conn);
                    g_worker_slots[j].conn = NULL;
                }
                return false;
            }
        }
    }

    defer {
        if (!dryrun) {
            pthread_mutex_lock(&g_pool_mutex);
            for (int i = 0; i < WORKER_COUNT; i++) {
                if (g_worker_slots[i].conn) {
                    pgpool_release(pool, g_worker_slots[i].conn);
                    g_worker_slots[i].conn = NULL;
                }
                g_worker_slots[i].in_use = false;
            }
            pthread_mutex_unlock(&g_pool_mutex);
        }
    };

    PathSnapshot snapshot = {0};
    if (!dryrun) load_path_snapshot(&snapshot, g_worker_slots[0].conn);
    defer {
        path_snapshot_destroy(&snapshot);
    };

    int num_threads = WORKER_COUNT;
#ifdef _SC_NPROCESSORS_ONLN
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) { num_threads = (int)nprocs; }
#endif
    if (num_threads < WORKER_COUNT) { num_threads = WORKER_COUNT; }

    Threadpool* threadpool = threadpool_create((size_t)num_threads);
    if (!threadpool) {
        fprintf(stderr, ">>> Failed to create threadpool\n");
        return false;
    }
    defer {
        threadpool_destroy(threadpool, 60000);
    };

    WalkDirUserData data = {
        .thpool = threadpool,
        .success = &success,
        .dryrun = dryrun,
        .min_pages = min_pages,
        .snapshot = &snapshot,
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
