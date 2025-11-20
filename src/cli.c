#include "../include/cli.h"
#include "../include/pdf_preprocess.h"

#include <poppler/glib/poppler.h>
#include <pulsar/macros.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>

#define WORKERS 4

/** SQL queries used throughout the application. */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) VALUES($1, $2, $3) ON CONFLICT(name, path) DO UPDATE SET num_pages = "
    "EXCLUDED.num_pages RETURNING id";
static const char* page_insert_query =
    "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3) ON CONFLICT (file_id, page_num) DO NOTHING";
static const char* file_id_query = "SELECT id FROM files WHERE path = $1";

/** Parameters for processing an entire PDF file (all pages). */
typedef struct {
    char* path;              /** Full path to PDF file. Ownership transferred. */
    char* name;              /** Filename. Ownership transferred. */
    int64_t file_id;         /** Database file ID. */
    int npages;              /** Total number of pages in PDF. */
    pgconn_config_t* config; /** Database configuration for creating connection. */
    _Atomic bool* success;   /** Shared success flag. */
} PDFProcessParams;

/**
 * Extracts text from all PDF pages and inserts them into the database.
 * Called within an active transaction - does NOT manage transaction boundaries.
 *
 * @param doc PDF document.
 * @param file_id Database file ID.
 * @param name Filename for error reporting.
 * @param npages Total number of pages to process.
 * @param conn Database connection with active transaction.
 * @return true if all pages processed successfully, false otherwise.
 */
static bool process_all_pages(PopplerDocument* doc, int64_t file_id, const char* name, int npages, pgconn_t* conn) {
    bool all_ok = true;

    for (int page_num = 0; page_num < npages; page_num++) {
        // Get the page
        PopplerPage* page = poppler_document_get_page(doc, page_num);
        if (!page) {
            fprintf(stderr, ">>> Failed to get page %d of %s\n", page_num + 1, name);
            all_ok = false;
            continue;
        }
        defer({ g_object_unref(page); });

        // Extract text from the page
        char* text = poppler_page_get_text(page);
        if (!text || text[0] == '\0') {
            // No text on this page - continue to next
            if (text) {
                g_free(text);
            }
            continue;
        }
        defer({ g_free(text); });

        // Clean the PDF text
        size_t len = strlen(text);

        // Truncate if exceeds tokenizer limit
        if (len >= 2047) {
            text[2046] = '\0';
        }
        pdf_text_clean(text, len, false);

        // Prepare parameters for insert
        char file_id_str[32];
        char page_num_str[16];
        snprintf(file_id_str, sizeof(file_id_str), "%ld", file_id);
        snprintf(page_num_str, sizeof(page_num_str), "%d", page_num + 1);

        const char* values[] = {file_id_str, page_num_str, text};

        // Insert page
        PGresult* res = pgconn_query_params_safe(conn, page_insert_query, 3, values, NULL);
        if (!res) {
            fprintf(stderr, ">>> Failed to insert page %d of %s: %s\n", page_num + 1, name,
                    pgconn_error_message_safe(conn));
            all_ok = false;
            // Continue processing other pages
        } else {
            PQclear(res);
        }
    }

    return all_ok;
}

/**
 * Threadpool task that processes all pages of a PDF file in a single transaction.
 * Creates its own database connection and manages the transaction lifecycle.
 *
 * @param arg Pointer to PDFProcessParams structure.
 */
static void process_pdf_task(void* arg) {
    PDFProcessParams* params = arg;

    defer({
        free(params->path);
        free(params->name);
        free(params);
    });

    // Create dedicated connection for this task
    pgconn_t* conn = pgconn_create(params->config);
    ASSERT(conn != nullptr);
    defer({ pgconn_destroy(conn); });

    // Open the PDF document
    char* uri = g_filename_to_uri(params->path, NULL, NULL);
    ASSERT(uri != nullptr);
    defer({ g_free(uri); });

    GError* error        = NULL;
    PopplerDocument* doc = poppler_document_new_from_file(uri, NULL, &error);
    if (!doc) {
        fprintf(stderr, ">>> Worker failed to open PDF %s: %s\n", params->path, error->message);
        g_error_free(error);
        atomic_store(params->success, false);
        return;
    }
    defer({ g_object_unref(doc); });

    // Verify page count matches
    int actual_pages = poppler_document_get_n_pages(doc);
    if (actual_pages != params->npages) {
        fprintf(stderr, ">>> Page count mismatch for %s: expected %d, got %d\n", params->name, params->npages,
                actual_pages);
        atomic_store(params->success, false);
        return;
    }

    // Begin single transaction for entire PDF
    PGresult* begin_res = pgconn_query_safe(conn, "BEGIN", NULL);
    ASSERT(begin_res != nullptr);
    PQclear(begin_res);

    bool pages_ok = false;

    // Ensure transaction is closed on all exit paths
    defer({
        const char* end_cmd = pages_ok ? "COMMIT" : "ROLLBACK";
        PGresult* end_res   = pgconn_query_safe(conn, end_cmd, NULL);
        if (end_res) {
            PQclear(end_res);
        } else {
            // If COMMIT/ROLLBACK fails, try ROLLBACK as fallback
            fprintf(stderr, ">>> Failed to %s transaction for %s, attempting ROLLBACK\n", end_cmd, params->name);
            PGresult* fallback = pgconn_query_safe(conn, "ROLLBACK", NULL);
            if (fallback) {
                PQclear(fallback);
            }
        }
    });

    // Process all pages within the transaction
    pages_ok = process_all_pages(doc, params->file_id, params->name, params->npages, conn);

    if (!pages_ok) {
        atomic_store(params->success, false);
        fprintf(stderr, ">>> Transaction for %s will be rolled back due to errors\n", params->path);
    } else {
        printf(">>> Successfully processed %s (%d pages)\n", params->path, params->npages);
    }
}

/** User data passed to the directory walking callback. */
typedef struct {
    pgconn_t* main_conn;     /** Database connection for main thread (file inserts only). */
    Threadpool* thpool;      /** Threadpool for processing PDFs. */
    pgconn_config_t* config; /** Database configuration for workers. */
    int min_pages;           /** Minimum number of pages required. */
    _Atomic bool* success;   /** Shared success flag. */
    bool dryrun;             /** Perform dry run without database writes. */
} WalkDirUserData;

/**
 * Directory walking callback that identifies PDF files and submits them for processing.
 * File metadata is inserted on the main thread, page processing is delegated to workers.
 *
 * @param path Full path to the current file/directory.
 * @param name Filename of the current entry.
 * @param userdata Pointer to WalkDirUserData structure.
 * @return DirContinue to continue, DirSkip to skip directory, DirStop to halt.
 */
static WalkDirOption walk_dir_callback(const char* path, const char* name, void* userdata) {
    if (name == nullptr || strlen(name) == 0 || name[0] == '.') {
        return DirContinue;
    }

    // Skip common build/dependency directories
    static const char* skip_dirs[] = {
        "node_modules", ".git",   ".svn",    ".hg",       "__pycache__", ".pytest_cache", ".mypy_cache",
        ".tox",         "venv",   ".venv",   "env",       ".env",        "vendor",        "build",
        "dist",         "target", ".gradle", ".idea",     ".vscode",     ".cache",        "coverage",
        ".next",        ".nuxt",  ".turbo",  ".DS_Store", NULL,
    };

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) {
            return DirSkip;
        }
    }

    // Check for .pdf extension (case-insensitive)
    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) {
        return DirContinue;
    }
    ext++;  // Move past the dot

    if (strcasecmp(ext, "pdf") != 0) {
        return DirContinue;
    }

    WalkDirUserData* data = userdata;

    // Convert file path to URI for poppler to check page count
    char* uri = g_filename_to_uri(path, NULL, NULL);
    ASSERT(uri != nullptr);
    defer({ g_free(uri); });

    // Open PDF to count pages
    GError* error        = NULL;
    PopplerDocument* doc = poppler_document_new_from_file(uri, NULL, &error);
    if (!doc) {
        fprintf(stderr, ">>> Error opening PDF %s: %s. Skipping.\n", path, error->message);
        g_error_free(error);
        return DirContinue;
    }
    defer({ g_object_unref(doc); });

    int npages = poppler_document_get_n_pages(doc);

    // Skip empty or short PDFs
    if (npages == 0 || npages < data->min_pages) {
        return DirContinue;
    }

    if (data->dryrun) {
        printf(">>> Found %s (%d pages)\n", path, npages);
        return DirContinue;
    }

    // Insert file record on main thread and get file ID
    char num_pages_str[16];
    snprintf(num_pages_str, sizeof(num_pages_str), "%d", npages);

    const char* file_values[] = {name, path, num_pages_str};
    PGresult* res             = pgconn_query_params_safe(data->main_conn, file_insert_query, 3, file_values, NULL);

    if (!res) {
        fprintf(stderr, ">>> Failed to insert file %s: %s\n", path, pgconn_error_message_safe(data->main_conn));
        atomic_store(data->success, false);
        return DirStop;
    }

    // Get file ID from RETURNING clause or from separate query
    int64_t file_id;
    if (PQntuples(res) > 0) {
        file_id = atoll(PQgetvalue(res, 0, 0));
        PQclear(res);
    } else {
        PQclear(res);
        // File existed (conflict), query for ID
        const char* path_param[] = {path};
        PGresult* id_res         = pgconn_query_params_safe(data->main_conn, file_id_query, 1, path_param, NULL);
        ASSERT(id_res != nullptr && PQntuples(id_res) > 0);
        file_id = atoll(PQgetvalue(id_res, 0, 0));
        PQclear(id_res);
    }

    // Allocate task parameters - ownership transfers to worker
    PDFProcessParams* params = malloc(sizeof(*params));
    ASSERT(params != nullptr);

    params->path    = strdup(path);
    params->name    = strdup(name);
    params->file_id = file_id;
    params->npages  = npages;
    params->config  = data->config;
    params->success = data->success;

    ASSERT(params->path != nullptr);
    ASSERT(params->name != nullptr);

    // Submit entire PDF processing task to threadpool
    threadpool_submit(data->thpool, process_pdf_task, params);

    return DirContinue;
}

/**
 * Processes all PDF files in the given directory tree.
 * Main thread handles file metadata insertion, workers process pages in single transactions.
 *
 * @param config Database configuration.
 * @param root_dir Root directory to scan for PDF files.
 * @param min_pages Minimum number of pages required for indexing.
 * @param dryrun If true, performs dry run without database writes.
 * @return true if all operations completed successfully, false otherwise.
 */
bool process_pdfs(pgconn_config_t* config, const char* root_dir, int min_pages, bool dryrun) {
    _Atomic bool success = true;

    // Create threadpool for PDF processing
    Threadpool* threadpool = threadpool_create(WORKERS);
    ASSERT(threadpool != nullptr);
    defer({ threadpool_destroy(threadpool, -1); });

    // Main thread connection for file inserts only
    pgconn_t* main_conn = pgconn_create(config);
    ASSERT(main_conn != nullptr);
    defer({ pgconn_destroy(main_conn); });

    // Begin transaction on main connection for file inserts
    if (!dryrun) {
        PGresult* begin_res = pgconn_query_safe(main_conn, "BEGIN", NULL);
        ASSERT(begin_res != nullptr);
        PQclear(begin_res);

        defer({
            const char* end_cmd = atomic_load(&success) ? "COMMIT" : "ROLLBACK";
            PGresult* end_res   = pgconn_query_safe(main_conn, end_cmd, NULL);
            ASSERT(end_res != nullptr);
            PQclear(end_res);
        });
    }

    // Walk directory tree
    WalkDirUserData data = {
        .main_conn = main_conn,
        .thpool    = threadpool,
        .config    = config,
        .min_pages = min_pages,
        .success   = &success,
        .dryrun    = dryrun,
    };

    if (dryrun) {
        printf("Performing index dry run on %s\n", root_dir);
    }

    int walk_result = dir_walk(root_dir, walk_dir_callback, &data);
    if (walk_result != 0) {
        atomic_store(&success, false);
    }

    return atomic_load(&success);
}
