#include "cli.h"
#include <poppler/glib/poppler.h>
#include <pulsar/macros.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>

/** User data passed to the directory walking callback. */
typedef struct {
    pgconn_t* conn;     /** Database connection for the main thread. */
    Threadpool* thpool; /** Threadpool for processing PDF pages. */
    int min_pages;      /** Minimum number of pages in PDF to be indexed. */
    bool* success;      /** Shared success flag for the operation. */
    bool dryrun;        /** Perform dry running with db transactions. */
} WalkDirUserData;

/** Parameters passed to each PDF page processing task. */
typedef struct {
    PopplerDocument* doc;   /** Shared PDF document instance. */
    int page;               /** Zero-based page number to process. */
    const char* name;       /** File name being processed. */
    int64_t file_id;        /** Database file ID. */
    pgconn_t* conn;         /** Database connection for this thread. */
    _Atomic int* ref_count; /** Reference counter for document cleanup. */
} PageParams;

/** SQL queries used throughout the application. */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) VALUES($1, $2, $3) ON CONFLICT(path) DO UPDATE SET name = EXCLUDED.name "
    "RETURNING id";

static const char* page_insert_query =
    "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3) ON CONFLICT (file_id, page_num) DO NOTHING";

/**
 * Threadpool task function that extracts text from a single PDF page and stores it in the database.
 * Uses savepoints to handle conflicts without aborting the parent transaction.
 *
 * @param arg Pointer to PageParams structure containing task parameters.
 */
void extract_pdf_pages(void* arg) {
    PageParams* pp = arg;

    // Ensure cleanup of resources in all exit paths
    defer({
        // Atomically decrement and check if we're the last task
        if (atomic_fetch_sub(pp->ref_count, 1) == 1) {
            // We were the last task - clean up shared resources
            g_object_unref(pp->doc);
            free(pp->ref_count);
        }
        free(pp);
    });

    // Get the page
    PopplerPage* page = poppler_document_get_page(pp->doc, pp->page);
    if (!page) {
        return;
    }
    defer({ g_object_unref(page); });

    // Extract text from the page
    char* text = poppler_page_get_text(page);
    if (!text || text[0] == '\0') {
        // No text on the page.
        return;
    }
    defer({ g_free(text); });

    // Clean the PDF text to remove invalid unicode characters, artifacts, control characters
    size_t len = strlen(text);

    // If text exceeds 2047 characters, it can not be tokenized by the
    // postgres tokenizer. Truncate it.
    if (len >= 2047) {
        text[2046] = '\0';
    }
    pdf_text_clean(text, len, false);

    // Convert parameters for prepared statement
    char file_id_str[32];
    char page_num_str[16];
    snprintf(file_id_str, sizeof(file_id_str), "%ld", pp->file_id);
    snprintf(page_num_str, sizeof(page_num_str), "%d", pp->page + 1);

    const char* values[] = {file_id_str, page_num_str, text};

    // Create a unique savepoint name for this page insert
    char savepoint_name[128];
    snprintf(savepoint_name, sizeof(savepoint_name), "sp_page_%ld_%d_%p", pp->file_id, pp->page, (void*)pp);

    // Establish savepoint before the insert
    char savepoint_cmd[256];
    snprintf(savepoint_cmd, sizeof(savepoint_cmd), "SAVEPOINT %s", savepoint_name);

    PGresult* sp_res = pgconn_query_safe(pp->conn, savepoint_cmd, NULL);
    if (!sp_res) {
        fprintf(stderr, ">>> Failed to create savepoint for page %d of file %ld\n", pp->page + 1, pp->file_id);
        return;
    }
    PQclear(sp_res);

    // Attempt the insert within the savepoint
    PGresult* res = pgconn_query_params_safe(pp->conn, page_insert_query, 3, values, NULL);

    if (!res) {
        // Insert failed - rollback to savepoint to avoid aborting parent transaction
        fprintf(stderr, ">>> Failed to insert page %d of %s: %s. Rolling back savepoint.\n", pp->page + 1, pp->name,
                pgconn_error_message_safe(pp->conn));

        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "ROLLBACK TO SAVEPOINT %s", savepoint_name);
        PGresult* rb_res = pgconn_query_safe(pp->conn, savepoint_cmd, NULL);
        if (rb_res) {
            PQclear(rb_res);
        }

        // Release the savepoint after rollback
        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "RELEASE SAVEPOINT %s", savepoint_name);
        PGresult* rel_res = pgconn_query_safe(pp->conn, savepoint_cmd, NULL);
        if (rel_res) {
            PQclear(rel_res);
        }
        return;
    }

    PQclear(res);

    // Insert succeeded - release the savepoint
    snprintf(savepoint_cmd, sizeof(savepoint_cmd), "RELEASE SAVEPOINT %s", savepoint_name);
    PGresult* rel_res = pgconn_query_safe(pp->conn, savepoint_cmd, NULL);
    if (rel_res) {
        PQclear(rel_res);
    }
}

/**
 * Directory walking callback that processes PDF files.
 * For each PDF found, it inserts the file record and submits page processing tasks to the
 * threadpool. Uses savepoints to handle file insert conflicts.
 *
 * @param path Full path to the current file/directory.
 * @param name Filename of the current entry.
 * @param userdata Pointer to WalkDirUserData structure.
 * @return DirContinue to continue walking, DirStop to halt.
 */
WalkDirOption walk_dir_callback(const char* path, const char* name, void* userdata) {
    if (name == nullptr || strlen(name) == 0 || name[0] == '.') {
        return DirContinue;
    }

    // Skip these folders
    static const char* skip_dirs[] = {
        "node_modules",   // Node.js dependencies
        ".git",           // Git repository metadata
        ".svn",           // Subversion metadata
        ".hg",            // Mercurial metadata
        "__pycache__",    // Python bytecode cache
        ".pytest_cache",  // Pytest cache
        ".mypy_cache",    // Mypy type checker cache
        ".tox",           // Tox testing environments
        "venv",           // Python virtual environment
        ".venv",          // Python virtual environment (hidden)
        "env",            // Generic environment directory
        ".env",           // Generic environment directory (hidden)
        "vendor",         // Go/Ruby vendored dependencies
        "build",          // Build folder dependencies
        "dist",           // Dist folder dependencies
        "target",         // Rust/Java build output
        ".gradle",        // Gradle build cache
        ".idea",          // IntelliJ IDEA project files
        ".vscode",        // VS Code settings
        ".cache",         // Generic cache directory
        "coverage",       // Test coverage reports
        ".next",          // Next.js build cache
        ".nuxt",          // Nuxt.js build cache
        ".turbo",         // Turborepo cache
        NULL              // Sentinel
    };

    for (size_t i = 0; skip_dirs[i] != NULL; i++) {
        if (strcmp(name, skip_dirs[i]) == 0) {
            return DirSkip;
        }
    }

    // Find the last dot in the filename
    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) {
        return DirContinue;  // No extension or filename starts with dot
    }

    // Move past the dot to get the actual extension
    ext++;

    // Check if extension is pdf (case-insensitive)
    if (strcasecmp(ext, "pdf") != 0) {
        return DirContinue;  // Not a PDF file
    }

    WalkDirUserData* data = userdata;
    bool* success         = data->success;

    // =============== Open PDF to count number of pages =================

    // Convert file path to URI for poppler
    char* uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri) {
        *data->success = false;
        printf("Error converting path to URI\n");
        return DirStop;
    }
    defer({ g_free(uri); });

    // Open PDF with poppler - the document will be freed by the last worker
    GError* error        = NULL;
    PopplerDocument* doc = poppler_document_new_from_file(uri, NULL, &error);
    if (!doc) {
        *data->success = true;
        fprintf(stderr, ">>> Error opening PDF: %s. Skipping\n", error->message);
        g_error_free(error);
        return DirContinue;
    }

    int npages = poppler_document_get_n_pages(doc);

    // Skip empty or short PDFs.
    if (npages == 0 || npages < data->min_pages) {
        *data->success = true;
        g_object_unref(doc);
        return DirContinue;
    }
    printf(">>> %s (%dpages)\n", path, npages);

    if (data->dryrun) {
        *success = true;
        g_object_unref(doc);
        return DirContinue;
    }

    // Convert int to char*
    char num_pages[8];
    snprintf(num_pages, sizeof(num_pages), "%d", npages);

    // Create a unique savepoint name for this file insert
    char savepoint_name[256];
    snprintf(savepoint_name, sizeof(savepoint_name), "sp_file_%p", (void*)path);

    // Establish savepoint before file insert
    char savepoint_cmd[512];
    snprintf(savepoint_cmd, sizeof(savepoint_cmd), "SAVEPOINT %s", savepoint_name);

    PGresult* sp_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
    if (!sp_res) {
        *success = false;
        fprintf(stderr, ">>> Failed to create savepoint for file %s\n", path);
        g_object_unref(doc);
        return DirStop;
    }
    PQclear(sp_res);

    // Insert file into database
    const char* values[] = {name, path, num_pages};
    PGresult* res        = pgconn_query_params_safe(data->conn, file_insert_query, 3, values, NULL);
    if (!res) {
        *success = false;
        fprintf(stderr, ">>> Failed to insert file %s: %s. Rolling back savepoint.\n", path,
                pgconn_error_message_safe(data->conn));

        // Rollback to savepoint
        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "ROLLBACK TO SAVEPOINT %s", savepoint_name);
        PGresult* rb_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
        if (rb_res) {
            PQclear(rb_res);
        }

        // Release savepoint
        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "RELEASE SAVEPOINT %s", savepoint_name);
        PGresult* rel_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
        if (rel_res) {
            PQclear(rel_res);
        }

        g_object_unref(doc);
        return DirStop;
    }

    // Extract the returned file ID
    int64_t file_id = 0;
    if (PQntuples(res) > 0) {
        file_id = atoll(PQgetvalue(res, 0, 0));
    } else {
        *success = false;
        fprintf(stderr, ">>> Failed to retrieve file ID from insert for %s\n", path);
        PQclear(res);

        // Rollback to savepoint on failure
        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "ROLLBACK TO SAVEPOINT %s", savepoint_name);
        PGresult* rb_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
        if (rb_res) {
            PQclear(rb_res);
        }

        // Release savepoint
        snprintf(savepoint_cmd, sizeof(savepoint_cmd), "RELEASE SAVEPOINT %s", savepoint_name);
        PGresult* rel_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
        if (rel_res) {
            PQclear(rel_res);
        }

        g_object_unref(doc);
        return DirStop;
    }

    PQclear(res);

    // Release the savepoint after successful insert
    snprintf(savepoint_cmd, sizeof(savepoint_cmd), "RELEASE SAVEPOINT %s", savepoint_name);
    PGresult* rel_res = pgconn_query_safe(data->conn, savepoint_cmd, NULL);
    if (rel_res) {
        PQclear(rel_res);
    }

    *success = true;

    // Allocate reference counter for this document
    _Atomic int* ref_count = malloc(sizeof(_Atomic int));
    if (!ref_count) {
        *data->success = false;
        g_object_unref(doc);
        return DirStop;
    }

    // Initialize reference count to number of pages
    atomic_store(ref_count, npages);

    // Submit each page as a separate task to the threadpool
    printf(">>> Extracting text from %d pages\n", npages);
    for (int page = 0; page < npages; page++) {
        PageParams* pp = malloc(sizeof(PageParams));
        if (!pp) {
            perror("malloc");
            *data->success = false;

            // Clean up if we can't allocate task parameters
            if (atomic_fetch_sub(ref_count, npages - page) == npages - page) {
                g_object_unref(doc);
                free(ref_count);
            }
            return DirStop;
        }

        // Initialize task parameters - ownership transfers to threadpool task
        pp->page      = page;
        pp->name      = name;
        pp->doc       = doc;
        pp->file_id   = file_id;
        pp->conn      = data->conn;
        pp->ref_count = ref_count;
        threadpool_submit(data->thpool, extract_pdf_pages, pp);
    }
    return DirContinue;
}

/**
 * Processes all PDF files in the given directory tree.
 * Creates a transaction, walks the directory structure, and submits PDF page processing tasks.
 * Uses savepoints to handle conflicts gracefully without aborting the main transaction.
 *
 * @param conn Database connection.
 * @param root_dir Root directory to start scanning for PDF files.
 * @param min_pages Minimum number of pages required for a PDF to be indexed.
 * @param dryrun If true, performs a dry run without database writes.
 * @return true if all operations completed successfully, false otherwise.
 */
bool process_pdfs(pgconn_t* conn, const char* root_dir, int min_pages, bool dryrun) {
    bool success = false;

    // Create threadpool with automatic cleanup
    Threadpool* threadpool = threadpool_create(4);
    if (!threadpool) {
        puts(">>> Failed to create threadpool");
        return false;
    }

    // Begin transaction with automatic commit/rollback
    ASSERT(pgconn_begin(conn));

    defer({
        if (success) {
            pgconn_commit(conn);
        } else {
            pgconn_rollback(conn);
        }
    });

    // Wait for threads to complete before commit/rollback.
    defer({ threadpool_destroy(threadpool, -1); });

    // Start walking the root directory
    WalkDirUserData data = {
        .conn      = conn,
        .thpool    = threadpool,
        .success   = &success,
        .min_pages = min_pages,
        .dryrun    = dryrun,
    };

    if (dryrun) {
        printf("Performing index dry run on %s\n", root_dir);
    }

    if (dir_walk(root_dir, walk_dir_callback, &data) != 0) {
        return false;
    }
    return success;
}
