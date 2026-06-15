/*
 * pdf_indexer.c — multi-process PDF text extractor / PostgreSQL ingester.
 *
 * Architecture
 * ============
 *   1. Walk directory/VFS, collect PDF paths.
 *   2. Register each file in DB, clear old pages, record file_id + page_count.
 *   3. Build a flat FileTask array (one entry per PDF).
 *   4. Fork workers; each worker atomically grabs whole files and processes
 *      all pages in a single document-open / COPY stream.
 *
 * Why file-level work-stealing beats page-chunk work-stealing
 * ===========================================================
 *   With page-chunking, a 600-page PDF is opened/parsed/mmapped N times by
 *   N different workers.  libpdfium's document-open cost is O(pages) and
 *   the mmap fault-in touches every page of the file each time.  Switching
 *   to whole-file tasks means each PDF is opened exactly once, across the
 *   entire run, regardless of its page count.
 */
#include "../include/pdf_indexer.h"
#include <fcntl.h>
#include <lexicon/lexicon_pdf.h>
#include <libpq-fe.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vfs.h>
#include "../include/pdf_preprocess.h"

static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) "
    "VALUES($1, $2, $3) "
    "ON CONFLICT(name, path) DO UPDATE "
    "  SET num_pages = EXCLUDED.num_pages "
    "RETURNING id";

extern vfs_t* vfs;
extern char* conn_info;

static volatile sig_atomic_t g_cancel_requested = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_cancel_requested = 1;
}

#define MAX_PAGE_TEXT_LEN  2047
#define COPY_ROW_BUF       (MAX_PAGE_TEXT_LEN * 2 + 64)
#define MAX_FILES_TO_INDEX 4096

/*
 * Worker count: PDF extraction is I/O + single-threaded-libpdfium bound,
 * not CPU bound.  More than ~4-6 workers just adds mmap pressure and
 * postgres connection overhead.  Cap at half the online CPUs, min 2, max 8.
 */
#define MAX_WORKERS 8

static const char* const skip_dirs[] = {
    "node_modules", ".git",   ".svn",     ".hg",    "__pycache__", ".pytest_cache", ".mypy_cache", ".tox",    "venv",
    ".venv",        "env",    ".env",     "vendor", "build",       "dist",          "target",      ".gradle", ".idea",
    ".vscode",      ".cache", "coverage", ".next",  ".nuxt",       ".turbo",        ".DS_Store",   NULL,
};

/* One task = one whole PDF file. */
typedef struct {
    char path[512];
    int64_t file_id;
    int num_pages;
} FileTask;

typedef struct {
    volatile int processed_pages; /* updated atomically by workers          */
    volatile int processed_files; /* ditto                                  */
    volatile int next_file_index; /* work-stealing cursor                   */
    int total_pages;              /* set by coordinator, read-only for workers */
    int file_count;               /* ditto                                  */
    FileTask tasks[MAX_FILES_TO_INDEX];
} SharedWork;

/* =========================================================================
 * Zero-Copy File Loader
 * ======================================================================= */

typedef struct {
    uint8_t* data;
    size_t size;
    int fd;
    bool is_mmap;
} FileBuffer;

static void file_buffer_release(FileBuffer* buf) {
    if (buf->is_mmap) {
        if (buf->data) munmap(buf->data, buf->size);
        if (buf->fd >= 0) close(buf->fd);
    } else {
        free(buf->data);
    }
    *buf = (FileBuffer){.fd = -1};
}

static bool file_buffer_load(const char* path, FileBuffer* buf) {
    *buf = (FileBuffer){.fd = -1};

    if (vfs != NULL) {
        buf->data = vfs_read_file(vfs, path, &buf->size);
        return buf->data != NULL;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    buf->size = (size_t)st.st_size;
    buf->data = mmap(NULL, buf->size, PROT_READ, MAP_SHARED, fd, 0);
    if (buf->data == MAP_FAILED) {
        buf->data = NULL;
        close(fd);
        return false;
    }

    buf->fd = fd;
    buf->is_mmap = true;
    return true;
}

/* =========================================================================
 * COPY row escaping
 * ======================================================================= */

static ssize_t copy_escape(const char* src, char* dst, size_t cap) {
    size_t out = 0;
    for (const char* p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (out + 2 >= cap) return -1;
        switch (c) {
            case '\\':
                dst[out++] = '\\';
                dst[out++] = '\\';
                break;
            case '\t':
                dst[out++] = '\\';
                dst[out++] = 't';
                break;
            case '\n':
                dst[out++] = '\\';
                dst[out++] = 'n';
                break;
            case '\r':
                dst[out++] = '\\';
                dst[out++] = 'r';
                break;
            default:
                dst[out++] = (char)c;
                break;
        }
    }
    dst[out] = '\0';
    return (ssize_t)out;
}

/* =========================================================================
 * Core per-file processor (called once per file per worker)
 * ======================================================================= */

static bool process_file(const FileTask* task, PGconn* conn) {
    FileBuffer m = {0};
    if (!file_buffer_load(task->path, &m)) return false;

    pdf_document_t* doc = NULL;
    if (pdf_document_open_mem(m.data, m.size, NULL, &doc) != PDF_OK) {
        file_buffer_release(&m);
        return false;
    }

    PGresult* tx = PQexec(conn, "BEGIN");
    bool ok = (tx && PQresultStatus(tx) == PGRES_COMMAND_OK);
    if (tx) PQclear(tx);
    if (!ok) goto cleanup_doc;

    PGresult* cr = PQexec(conn, "COPY pages(file_id, page_num, text) FROM STDIN");
    ok = (cr && PQresultStatus(cr) == PGRES_COPY_IN);
    if (cr) PQclear(cr);
    if (!ok) {
        PQexec(conn, "ROLLBACK");
        goto cleanup_doc;
    }

    char row[COPY_ROW_BUF];
    char escaped[MAX_PAGE_TEXT_LEN * 2 + 4];

    for (int i = 0; i < task->num_pages && ok; i++) {
        pdf_page_t* page = NULL;
        if (pdf_page_open(doc, i, &page) != PDF_OK) continue;

        pdf_text_t* th = NULL;
        if (pdf_text_open(page, &th) != PDF_OK) {
            pdf_page_close(page);
            continue;
        }

        char* raw = NULL;
        if (pdf_text_utf8(th, &raw) == PDF_OK && raw) {
            size_t len = strlen(raw);
            if (len >= MAX_PAGE_TEXT_LEN) {
                raw[MAX_PAGE_TEXT_LEN - 1] = '\0';
                len = MAX_PAGE_TEXT_LEN - 1;
            }

            char* cleaned = pdf_text_clean(raw, len, true);
            if (copy_escape(cleaned, escaped, sizeof(escaped)) >= 0) {
                int n = snprintf(row, sizeof(row), "%" PRId64 "\t%d\t%s\n", task->file_id, i + 1, escaped);
                if (n > 0 && (size_t)n < sizeof(row)) {
                    if (PQputCopyData(conn, row, n) != 1) ok = false;
                }
            }
            if (cleaned != raw) free(cleaned);
            free(raw);
        }

        pdf_text_close(th);
        pdf_page_close(page);
    }

    if (!ok) {
        PQputCopyEnd(conn, "client error during page extraction");
    } else if (PQputCopyEnd(conn, NULL) != 1) {
        ok = false;
    }

    PGresult* fin;
    while ((fin = PQgetResult(conn)) != NULL) {
        if (ok && PQresultStatus(fin) != PGRES_COMMAND_OK) ok = false;
        PQclear(fin);
    }

    if (ok) {
        PGresult* c = PQexec(conn, "COMMIT");
        if (!c || PQresultStatus(c) != PGRES_COMMAND_OK) ok = false;
        if (c) PQclear(c);
    } else {
        PQexec(conn, "ROLLBACK");
    }

cleanup_doc:
    pdf_document_close(doc);
    file_buffer_release(&m); /* mmap released immediately after close — peak RSS ~= largest single PDF */
    return ok;
}

/* =========================================================================
 * Path Collection
 * ======================================================================= */

typedef struct {
    char* paths[MAX_FILES_TO_INDEX];
    int count;
} PathList;

static bool collect_vfs_file(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    PathList* pl = (PathList*)userdata;
    if (pl->count >= MAX_FILES_TO_INDEX) return false;
    const char* ext = strrchr(path, '.');
    if (!ext || ext == path || strcasecmp(ext + 1, "pdf") != 0) return true;
    pl->paths[pl->count++] = strdup(path);
    return true;
}

static WalkDirOption collect_host_file(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;
    PathList* pl = (PathList*)userdata;
    if (pl->count >= MAX_FILES_TO_INDEX) return DirStop;
    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) return DirSkip;
    }
    const char* ext = strrchr(name, '.');
    if (!ext || ext == name || strcasecmp(ext + 1, "pdf") != 0) return DirContinue;
    pl->paths[pl->count++] = strdup(path);
    return DirContinue;
}

/* =========================================================================
 * Public Entry Point
 * ======================================================================= */

bool process_pdfs(const char* root_dir, int min_pages, bool dryrun) {
    struct sigaction sa = {.sa_handler = sigint_handler, .sa_flags = 0};
    struct sigaction old_int, old_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
    defer {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
    };

    /* 1. Collect paths */
    PathList pl = {0};
    if (vfs != NULL) {
        vfs_list(vfs, NULL, collect_vfs_file, &pl);
    } else {
        dir_walk(root_dir, collect_host_file, &pl);
    }
    if (pl.count == 0) {
        printf(">>> No PDFs found to index.\n");
        return true;
    }

    /* Shared work queue in anonymous mmap — visible across fork() */
    SharedWork* work = mmap(NULL, sizeof(SharedWork), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (work == MAP_FAILED) {
        fprintf(stderr, ">>> Failed to allocate shared work memory\n");
        return false;
    }
    memset(work, 0, sizeof(SharedWork));

    /* 2. Register files & build task list (coordinator, single connection) */
    pgconn_t* coord_conn = NULL;
    PGconn* raw_conn = NULL;
    if (!dryrun) {
        coord_conn = pgpool_acquire(pool, 10000);
        if (!coord_conn) {
            fprintf(stderr, ">>> Failed to acquire coordinator DB connection\n");
            munmap(work, sizeof(SharedWork));
            return false;
        }
        raw_conn = pgpool_get_raw_connection(coord_conn);
    }

    pdf_library_init();
    printf(">>> Registering %d files...\n", pl.count);
    fflush(stdout);

    for (int i = 0; i < pl.count; i++) {
        FileBuffer m = {0};
        if (!file_buffer_load(pl.paths[i], &m)) continue;

        pdf_document_t* doc = NULL;
        if (pdf_document_open_mem(m.data, m.size, NULL, &doc) != PDF_OK) {
            file_buffer_release(&m);
            continue;
        }

        int npages = pdf_document_page_count(doc);
        pdf_document_close(doc);
        file_buffer_release(&m);

        if (npages < min_pages) continue;

        int64_t fid = -1;
        if (!dryrun) {
            const char* name = strrchr(pl.paths[i], '/');
            name = name ? name + 1 : pl.paths[i];

            char npages_str[16];
            snprintf(npages_str, sizeof(npages_str), "%d", npages);
            const char* params[] = {name, pl.paths[i], npages_str};

            PGresult* res = PQexecParams(raw_conn, file_insert_query, 3, NULL, params, NULL, NULL, 0);
            if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                fid = atoll(PQgetvalue(res, 0, 0));
            }
            if (res) PQclear(res);

            if (fid >= 0) {
                char fid_str[24];
                snprintf(fid_str, sizeof(fid_str), "%" PRId64, fid);
                const char* dp[] = {fid_str};
                PGresult* dr = PQexecParams(raw_conn, "DELETE FROM pages WHERE file_id = $1", 1, NULL, dp, NULL, NULL,
                                            0);
                if (dr) PQclear(dr);
            }
        }

        if (work->file_count < MAX_FILES_TO_INDEX) {
            FileTask* t = &work->tasks[work->file_count++];
            strncpy(t->path, pl.paths[i], sizeof(t->path) - 1);
            t->file_id = fid;
            t->num_pages = npages;
            work->total_pages += npages;
        }
    }

    if (coord_conn) pgpool_release(pool, coord_conn);
    pdf_library_destroy();

    if (work->file_count == 0) {
        printf(">>> No qualifying PDFs after filtering.\n");
        munmap(work, sizeof(SharedWork));
        for (int i = 0; i < pl.count; i++)
            free(pl.paths[i]);
        return true;
    }

    /*
     * 3. Determine worker count.
     *
     * PDF extraction is I/O + libpdfium-parse bound, not CPU bound.
     * Empirically, 4–6 workers saturates a local SSD without excessive
     * memory pressure.  Cap at min(MAX_WORKERS, half online CPUs, file_count).
     */
    int num_workers = 4;
#if defined(_SC_NPROCESSORS_ONLN)
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 1) num_workers = (int)(nprocs / 2);
#endif
    if (num_workers > MAX_WORKERS) num_workers = MAX_WORKERS;
    if (num_workers > work->file_count) num_workers = work->file_count;
    if (num_workers < 1) num_workers = 1;

    printf(">>> Processing %d files (%d pages) with %d workers...\n", work->file_count, work->total_pages, num_workers);
    fflush(stdout);

    /* 4. Fork */
    for (int w = 0; w < num_workers; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, ">>> fork failed for worker %d\n", w);
            continue;
        }

        if (pid == 0) { /* ---- child ---- */
            pdf_library_init();

            PGconn* conn = NULL;
            if (!dryrun) {
                conn = PQconnectdb(conn_info);
                if (PQstatus(conn) != CONNECTION_OK) {
                    pdf_library_destroy();
                    _exit(1);
                }
                PGresult* s = PQexec(conn, "SET synchronous_commit = off");
                if (s) PQclear(s);
            }

            int idx;
            while (!g_cancel_requested && (idx = __sync_fetch_and_add(&work->next_file_index, 1)) < work->file_count) {
                const FileTask* task = &work->tasks[idx];
                bool ok = process_file(task, conn);

                int pages_done = __sync_fetch_and_add(&work->processed_pages, task->num_pages) + task->num_pages;
                int files_done = __sync_fetch_and_add(&work->processed_files, 1) + 1;

                const char* base = strrchr(task->path, '/');
                printf(">>> [%d/%d files | %d/%d pages] %s — %s\n", files_done, work->file_count, pages_done,
                       work->total_pages, base ? base + 1 : task->path, ok ? "ok" : "FAILED");
                fflush(stdout);
            }

            if (conn) PQfinish(conn);
            pdf_library_destroy();
            _exit(0);
        }
    }

    /* 5. Wait */
    int status;
    while (wait(&status) > 0)
        ;

    bool cancelled = g_cancel_requested;
    printf(">>> Indexing complete: %d/%d pages processed%s\n", work->processed_pages, work->total_pages,
           cancelled ? " (interrupted)" : "");

    for (int i = 0; i < pl.count; i++)
        free(pl.paths[i]);
    munmap(work, sizeof(SharedWork));

    return !cancelled;
}
