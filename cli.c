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
    bool* success;      /** Shared success flag for the operation. */
    Threadpool* thpool; /** Threadpool for processing PDF pages. */
} WalkDirUserData;

/** Parameters passed to each PDF page processing task. */
typedef struct {
    PopplerDocument* doc;   /** Shared PDF document instance. */
    int page;               /** Zero-based page number to process. */
    int64_t file_id;        /** Database file ID. */
    pgconn_t* conn;         /** Database connection for this thread. */
    _Atomic int* ref_count; /** Reference counter for document cleanup. */
} PageParams;

/** SQL queries used throughout the application. */
static const char* file_insert_query =
    "INSERT INTO files(name, path, num_pages) VALUES($1, $2, $3) ON CONFLICT(path) DO NOTHING";
static const char* page_insert_query = "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3)";
static const char* file_id_query     = "SELECT id FROM files WHERE path = $1";

/**
 * Threadpool task function that extracts text from a single PDF page and stores it in the database.
 * This function handles its own cleanup and manages the shared document reference counting.
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
    if (!text) {
        text = g_strdup("");  // Empty string if no text
        ASSERT(text);
    }
    defer({ g_free(text); });

    // Convert parameters for prepared statement
    char file_id_str[32];
    char page_num_str[16];
    snprintf(file_id_str, sizeof(file_id_str), "%ld", pp->file_id);
    snprintf(page_num_str, sizeof(page_num_str), "%d", pp->page + 1);

    // Insert page into database using prepared statement
    const char* values[] = {file_id_str, page_num_str, text};
    PGresult* res;

    res = pgconn_query_params_safe(pp->conn, page_insert_query, 3, values, NULL);
    if (!res) {
        fprintf(stderr, "Failed to insert page %d: %s\n", pp->page + 1, pgconn_error_message_safe(pp->conn));
        return;
    }
}

/**
 * Directory walking callback that processes PDF files.
 * For each PDF found, it inserts the file record and submits page processing tasks to the
 * threadpool.
 *
 * @param path Full path to the current file/directory.
 * @param name Filename of the current entry.
 * @param userdata Pointer to WalkDirUserData structure.
 * @return DirContinue to continue walking, DirStop to halt.
 */
WalkDirOption walk_dir_callback(const char* path, const char* name, void* userdata) {
    if (name == nullptr || strlen(name) == 0) {
        return DirContinue;
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
        fprintf(stderr, "Error opening PDF: %s. Skipping\n", error->message);
        g_error_free(error);
        return DirContinue;
    }

    int npages = poppler_document_get_n_pages(doc);
    if (npages == 0) {
        *data->success = true;
        g_object_unref(doc);
        return DirContinue;
    }

    // Convert int to char*
    char num_pages[8];
    snprintf(num_pages, sizeof(num_pages), "%d", npages);

    // Insert file into database
    const char* values[] = {name, path, num_pages};
    PGresult* res        = pgconn_query_params_safe(data->conn, file_insert_query, 3, values, NULL);
    if (!res) {
        *success = false;
        fprintf(stderr, "Failed to insert file %s: %s\n", path, pgconn_error_message_safe(data->conn));
        return DirStop;
    }
    defer({ PQclear(res); });

    printf(">>> Inserted file: %s\n", path);
    *success = true;

    // Get the file ID for the inserted/existing file
    const char* file_path_param[] = {path};
    PGresult* id_res              = pgconn_query_params_safe(data->conn, file_id_query, 1, file_path_param, NULL);
    if (!id_res || PQntuples(id_res) == 0) {
        *success = false;
        fprintf(stderr, "Failed to retrieve file ID for %s\n", path);
        if (id_res) {
            PQclear(id_res);
        }
        return DirStop;
    }
    defer({ PQclear(id_res); });

    int64_t file_id = atoll(PQgetvalue(id_res, 0, 0));

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
    printf("[%s]: Extracting text from %d pages\n", name, npages);
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
 *
 * @param root_dir Root directory to start scanning for PDF files.
 * @return true if all operations completed successfully, false otherwise.
 */
bool process_pdfs(const char* root_dir, pgconn_t* conn) {
    bool success = false;
    // Create threadpool with automatic cleanup
    Threadpool* threadpool = threadpool_create(4);
    if (!threadpool) {
        puts("Failed to create threadpool");
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
        .conn    = conn,
        .thpool  = threadpool,
        .success = &success,
    };

    if (dir_walk(root_dir, walk_dir_callback, &data) != 0) {
        return false;
    }
    return success;
}
