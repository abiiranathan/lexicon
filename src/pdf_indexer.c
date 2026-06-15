/*
 * pdf_indexer.c — single-threaded synchronous PDF text extractor / PostgreSQL ingester.
 *
 */
#include "../include/pdf_indexer.h"
#include <lexicon/lexicon_pdf.h>
#include <libpq-fe.h> /* PGconn, PQexec, PQexecParams, PQputCopyData, etc. */
#include <vfs.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

/* =========================================================================
 * SQL & Globals
 * ======================================================================= */

static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) "
    "VALUES($1, $2, $3) "
    "ON CONFLICT(name, path) DO UPDATE "
    "  SET num_pages = EXCLUDED.num_pages "
    "RETURNING id";

extern vfs_t* vfs;
extern char* conn_info;
extern pgpool_t* pool;

/** Global cancel flag written by sigint_handler for graceful abort. */
static volatile sig_atomic_t g_cancel_requested = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_cancel_requested = 1;
}

/* =========================================================================
 * Constants & Types
 * ======================================================================= */

/** Maximum UTF-8 text extracted from a single page (bytes, excluding NUL). */
#define MAX_PAGE_TEXT_LEN 2047

/** Buffer size for one COPY row: file_id tab page_num tab escaped_text newline. */
#define COPY_ROW_BUF (MAX_PAGE_TEXT_LEN * 2 + 64)

static const char* const skip_dirs[] = {
    "node_modules", ".git",   ".svn",     ".hg",    "__pycache__", ".pytest_cache", ".mypy_cache", ".tox",    "venv",
    ".venv",        "env",    ".env",     "vendor", "build",       "dist",          "target",      ".gradle", ".idea",
    ".vscode",      ".cache", "coverage", ".next",  ".nuxt",       ".turbo",        ".DS_Store",   NULL,
};

/** Processing outcome for a single PDF. */
typedef enum {
    OUTCOME_SUCCESS = 0,
    OUTCOME_DRYRUN = 1,
    OUTCOME_SKIPPED = 2,
    OUTCOME_LOAD_ERROR = 3,
    OUTCOME_OPEN_ERROR = 4,
    OUTCOME_DB_ERROR = 5,
    OUTCOME_COUNT = 6, /**< Sentinel — keep last. */
} OutcomeCode;

static const char* outcome_to_str(OutcomeCode code) {
    switch (code) {
        case OUTCOME_SUCCESS:
            return "success";
        case OUTCOME_DRYRUN:
            return "dry-run";
        case OUTCOME_SKIPPED:
            return "skipped";
        case OUTCOME_LOAD_ERROR:
            return "load-error";
        case OUTCOME_OPEN_ERROR:
            return "open-error";
        case OUTCOME_DB_ERROR:
            return "database-error";
        default:
            return "unknown";
    }
}

/* =========================================================================
 * Reusable File Buffer
 * ======================================================================= */

typedef struct {
    uint8_t* data;
    size_t size;
    size_t capacity;
} ReusableBuffer;

static void reusable_buffer_free(ReusableBuffer* buf) {
    if (buf) {
        free(buf->data);
        buf->data = NULL;
        buf->size = 0;
        buf->capacity = 0;
    }
}

/**
 * Loads the file at @p path into @p buf, reallocating the internal buffer 
 * only when the file size exceeds the current capacity.
 */
static bool reusable_buffer_load(const char* path, ReusableBuffer* buf) {
    if (vfs != NULL) {
        /* VFS manages its own memory allocations */
        free(buf->data);
        buf->data = vfs_read_file(vfs, path, &buf->size);
        buf->capacity = buf->size;
        return buf->data != NULL;
    }

    FILE* f = fopen(path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) goto err_close;
    long sz = ftell(f);
    if (sz < 0) goto err_close;
    if (fseek(f, 0, SEEK_SET) != 0) goto err_close;

    size_t req_size = (size_t)sz;
    if (req_size > buf->capacity) {
        size_t new_cap = req_size < 4096 ? 4096 : req_size * 2;
        uint8_t* new_ptr = realloc(buf->data, new_cap);
        if (!new_ptr) goto err_close;
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }

    buf->size = req_size;
    if (buf->size && fread(buf->data, 1, buf->size, f) != buf->size) { goto err_close; }

    fclose(f);
    return true;

err_close:
    fclose(f);
    return false;
}

/* =========================================================================
 * COPY Row Escaping
 * ======================================================================= */

/**
 * Escapes @p src into the PostgreSQL text-format COPY representation,
 * writing at most @p cap - 1 bytes to @p dst and NUL-terminating.
 */
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
 * Synchronous PDF Processor
 * ======================================================================= */

/**
 * Loads, extracts, and ingests a single PDF into the database.
 * Uses stack-allocated buffers for text operations to minimize heap allocations.
 *
 * @param path      Absolute path to the PDF file.
 * @param min_pages Skip files with fewer than this many pages.
 * @param dryrun    If true, open the PDF to count pages but do not write to DB.
 * @param conn      libpq connection (may be NULL when dryrun is true).
 * @param m         Pointer to the persistent file buffer.
 * @return Processing outcome code.
 */
static OutcomeCode process_one_pdf(const char* path, int min_pages, bool dryrun, PGconn* conn, ReusableBuffer* m) {
    OutcomeCode outcome = OUTCOME_LOAD_ERROR;

    if (!reusable_buffer_load(path, m)) return OUTCOME_LOAD_ERROR;

    pdf_document_t doc = {0};
    if (pdf_document_open_mem(&doc, m->data, m->size, NULL) != PDF_OK) { return OUTCOME_OPEN_ERROR; }

    int npages = pdf_document_page_count(&doc);
    if (npages <= 0 || npages < min_pages || dryrun) {
        outcome = dryrun ? OUTCOME_DRYRUN : OUTCOME_SKIPPED;
        goto out_close;
    }

    PGresult* tx_res = PQexec(conn, "BEGIN");
    if (!tx_res || PQresultStatus(tx_res) != PGRES_COMMAND_OK) {
        fprintf(stderr, ">>> Failed to start transaction: %s\n", PQerrorMessage(conn));
        if (tx_res) PQclear(tx_res);
        outcome = OUTCOME_DB_ERROR;
        goto out_close;
    }
    PQclear(tx_res);

    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char npages_str[16];
    snprintf(npages_str, sizeof(npages_str), "%d", npages);
    const char* params[] = {name, path, npages_str};

    int64_t fid = -1;
    PGresult* res = PQexecParams(conn, file_insert_query, 3, NULL, params, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        fid = atoll(PQgetvalue(res, 0, 0));
    } else {
        fprintf(stderr, ">>> file insert failed for '%s': %s\n", name, PQerrorMessage(conn));
    }
    PQclear(res);

    if (fid < 0) {
        PGresult* roll_res = PQexec(conn, "ROLLBACK");
        PQclear(roll_res);
        outcome = OUTCOME_DB_ERROR;
        goto out_close;
    }

    /* Initiate COPY FROM STDIN */
    PGresult* copy_res = PQexec(conn, "COPY pages(file_id, page_num, text) FROM STDIN (ON_ERROR ignore);");
    if (!copy_res || PQresultStatus(copy_res) != PGRES_COPY_IN) {
        fprintf(stderr, ">>> COPY start failed for '%s': %s\n", name, PQerrorMessage(conn));
        if (copy_res) PQclear(copy_res);
        PGresult* roll_res = PQexec(conn, "ROLLBACK");
        PQclear(roll_res);
        outcome = OUTCOME_DB_ERROR;
        goto out_close;
    }
    PQclear(copy_res);

    char row[COPY_ROW_BUF] = {0};
    char escaped[MAX_PAGE_TEXT_LEN * 2 + 4] = {0};
    char raw_buf[MAX_PAGE_TEXT_LEN + 1] = {0};
    bool copy_ok = true;

    for (int i = 0; i < npages && copy_ok; i++) {
        pdf_page_t page = {0};
        if (pdf_page_open(&doc, i, &page) != PDF_OK) continue;

        pdf_text_t* th = NULL;
        if (pdf_text_open(&page, &th) != PDF_OK) {
            pdf_page_close(&page);
            continue;
        }

        size_t written = 0;
        /* Read directly into stack buffer instead of allocating */
        if (pdf_text_utf8_buf(th, raw_buf, sizeof(raw_buf) - 1, &written) == PDF_OK && written > 0) {
            raw_buf[written] = '\0';

            if (copy_escape(raw_buf, escaped, sizeof(escaped)) >= 0) {
                int n = snprintf(row, sizeof(row), "%" PRId64 "\t%d\t%s\n", fid, i + 1, escaped);
                if (n > 0 && (size_t)n < sizeof(row) && PQputCopyData(conn, row, n) != 1) {
                    fprintf(stderr, ">>> PQputCopyData failed: %s\n", PQerrorMessage(conn));
                    copy_ok = false;
                }
            }
        }

        pdf_text_close(th);
        pdf_page_close(&page);
    }

    if (!copy_ok) {
        PQputCopyEnd(conn, "client error during page extraction");
    } else if (PQputCopyEnd(conn, NULL) != 1) {
        fprintf(stderr, ">>> PQputCopyEnd failed: %s\n", PQerrorMessage(conn));
        copy_ok = false;
    }

    PGresult* fin;
    while ((fin = PQgetResult(conn)) != NULL) {
        if (copy_ok && PQresultStatus(fin) != PGRES_COMMAND_OK) {
            fprintf(stderr, ">>> COPY finalise failed for '%s': %s\n", name, PQresultErrorMessage(fin));
            copy_ok = false;
        }
        PQclear(fin);
    }

    if (copy_ok) {
        PGresult* commit_res = PQexec(conn, "COMMIT");
        if (!commit_res || PQresultStatus(commit_res) != PGRES_COMMAND_OK) {
            fprintf(stderr, ">>> COMMIT failed: %s\n", PQerrorMessage(conn));
            copy_ok = false;
        }
        if (commit_res) { PQclear(commit_res); }
    } else {
        PGresult* roll_res = PQexec(conn, "ROLLBACK");
        PQclear(roll_res);
    }

    outcome = copy_ok ? OUTCOME_SUCCESS : OUTCOME_DB_ERROR;

out_close:
    pdf_document_close(&doc);
    return outcome;
}

/* =========================================================================
 * Walker State and Callbacks
 * ======================================================================= */

typedef struct {
    int processed_count;
    int min_pages;
    bool dryrun;
    PGconn* conn;
    int outcome_counts[OUTCOME_COUNT];
    ReusableBuffer file_buf;
} IndexerState;

static void record_outcome(IndexerState* is, const char* path, OutcomeCode outcome) {
    is->processed_count++;
    is->outcome_counts[(int)outcome]++;

    const char* base = strrchr(path, '/');
    printf(">>> [%d] %s — %s\n", is->processed_count, base ? base + 1 : path, outcome_to_str(outcome));
    fflush(stdout);
}

static bool process_vfs_file(const char* path, const vfs_stat_t* st, void* userdata) {
    (void)st;
    IndexerState* is = (IndexerState*)userdata;
    if (g_cancel_requested) return false;

    const char* ext = strrchr(path, '.');
    if (!ext || ext == path || strcasecmp(ext + 1, "pdf") != 0) return true;

    OutcomeCode outcome = process_one_pdf(path, is->min_pages, is->dryrun, is->conn, &is->file_buf);
    record_outcome(is, path, outcome);
    return true;
}

static WalkDirOption process_host_file(const FileAttributes* attr, const char* path, const char* name, void* userdata) {
    (void)attr;
    IndexerState* is = (IndexerState*)userdata;
    if (g_cancel_requested) return DirStop;

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) return DirSkip;
    }

    const char* ext = strrchr(name, '.');
    if (!ext || ext == name || strcasecmp(ext + 1, "pdf") != 0) return DirContinue;

    OutcomeCode outcome = process_one_pdf(path, is->min_pages, is->dryrun, is->conn, &is->file_buf);
    record_outcome(is, path, outcome);
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

    PGconn* raw_conn = PQconnectdb(conn_info);
    if (!raw_conn) return false;

    if (PQstatus(raw_conn) != CONNECTION_OK) {
        fprintf(stderr, "pgpool: connection failed: %s", PQerrorMessage(raw_conn));
        PQfinish(raw_conn);
        return false;
    }
    defer {
        PQfinish(raw_conn);
    };

    if (!dryrun) {
        PGresult* sync_res = PQexec(raw_conn, "SET synchronous_commit = off");
        if (sync_res) { PQclear(sync_res); }
    }

    IndexerState is = {.processed_count = 0,
                       .min_pages = min_pages,
                       .dryrun = dryrun,
                       .conn = raw_conn,
                       .outcome_counts = {0},
                       .file_buf = {0}};

    if (vfs != NULL) {
        if (dryrun) printf("Performing VFS index dry run\n");
        vfs_list(vfs, NULL, process_vfs_file, &is);
    } else {
        if (dryrun) printf("Performing host index dry run on %s\n", root_dir);
        dir_walk(root_dir, process_host_file, &is);
    }

    /* Free the persistent file buffer once processing is fully completed */
    reusable_buffer_free(&is.file_buf);

#if defined(__GLIBC__)
    /* Trim once at the end of processing instead of per file to minimize system overhead */
    malloc_trim(0);
#endif

    bool cancelled = g_cancel_requested;
    printf(">>> Indexing complete: %d PDFs processed%s\n", is.processed_count, cancelled ? " (interrupted)" : "");
    printf(
        ">>> Outcomes — success:%-5d dry-run:%-5d "
        "load-err:%-5d open-err:%-5d db-err:%-5d\n",
        is.outcome_counts[OUTCOME_SUCCESS], is.outcome_counts[OUTCOME_DRYRUN], is.outcome_counts[OUTCOME_LOAD_ERROR],
        is.outcome_counts[OUTCOME_OPEN_ERROR], is.outcome_counts[OUTCOME_DB_ERROR]);

    return !cancelled;
}
