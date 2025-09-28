#include "cli.h"
#include <poppler/glib/poppler.h>
#include <pulsar/macros.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/threadpool.h>
#include <stdatomic.h>

extern pgpool_t* pool; /** Global connection pool. */
extern int TIMEOUT_MS; /** Database default timeout in milliseconds. */

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
    "INSERT INTO files(name, path) VALUES($1, $2) ON CONFLICT(path) DO NOTHING";
static const char* stmt_insert = "insert_file";

static const char* page_insert_query =
    "INSERT INTO pages(file_id, page_num, text) VALUES($1, $2, $3)";
static const char* stmt_page_insert = "insert_page";

static const char* file_id_query = "SELECT id FROM files WHERE path = $1";
static const char* file_id_stmt  = "select_file_id";

/**
 * Ensures that prepared statements exist on the given connection.
 * This function is idempotent - it's safe to call multiple times on the same connection.
 *
 * @param conn Database connection to prepare statements on.
 * @return true on success, false on failure.
 */
static bool ensure_prepared_statements(pgconn_t* conn) {
    // Check if statements are already prepared on this connection
    PGresult* check_res = PQexec(pgpool_get_raw_connection(conn),
                                 "SELECT name FROM pg_prepared_statements WHERE name IN "
                                 "('insert_file', 'insert_page', 'select_file_id')");

    int existing_count = 0;
    if (check_res && PQresultStatus(check_res) == PGRES_TUPLES_OK) {
        existing_count = PQntuples(check_res);
    }

    if (check_res) {
        PQclear(check_res);
    }

    // If all 3 statements exist, we're done
    if (existing_count == 3) {
        return true;
    }

    // Prepare missing statements
    if (!pgpool_prepare(conn, stmt_insert, file_insert_query, 2, NULL, TIMEOUT_MS)) {
        fprintf(stderr, "Failed to prepare file insert statement\n");
        return false;
    }

    if (!pgpool_prepare(conn, stmt_page_insert, page_insert_query, 3, NULL, TIMEOUT_MS)) {
        fprintf(stderr, "Failed to prepare page insert statement\n");
        return false;
    }

    if (!pgpool_prepare(conn, file_id_stmt, file_id_query, 1, NULL, TIMEOUT_MS)) {
        fprintf(stderr, "Failed to prepare file ID select statement\n");
        return false;
    }

    return true;
}

/** Cleanup prepared statements. */
static void cleanup_prepared_statements(pgconn_t* conn) {
    pgpool_deallocate(conn, stmt_insert, TIMEOUT_MS);
    pgpool_deallocate(conn, stmt_page_insert, TIMEOUT_MS);
    pgpool_deallocate(conn, file_id_stmt, TIMEOUT_MS);
}

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
        // Deallocate prepare statements
        cleanup_prepared_statements(pp->conn);
        printf("Released connection: %p\n", (void*)pp->conn);

        pgpool_release(pool, pp->conn);

        // Atomically decrement and check if we're the last task
        if (atomic_fetch_sub(pp->ref_count, 1) == 1) {
            // We were the last task - clean up shared resources
            g_object_unref(pp->doc);
            free(pp->ref_count);
        }
        free(pp);
    });

    // Ensure prepared statements exist on this connection
    if (!ensure_prepared_statements(pp->conn)) {
        fprintf(stderr, "Failed to prepare statements on worker connection\n");
        return;
    }

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
    }
    defer({ g_free(text); });

    // Convert parameters for prepared statement
    char file_id_str[32];
    char page_num_str[16];
    snprintf(file_id_str, sizeof(file_id_str), "%ld", pp->file_id);
    snprintf(page_num_str, sizeof(page_num_str), "%d", pp->page + 1);

    // Insert page into database using prepared statement
    const char* values[] = {file_id_str, page_num_str, text};
    PGresult* res =
        pgpool_execute_prepared(pp->conn, stmt_page_insert, 3, values, NULL, NULL, 0, TIMEOUT_MS);
    defer({
        if (res) PQclear(res);
    });

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        const char* msg = pgpool_error_message(pp->conn);
        fprintf(stderr, "Failed to insert page %d: %s\n", pp->page + 1, msg);
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
    pgconn_t* conn        = data->conn;
    bool* success         = data->success;

    // Ensure prepared statements exist on this connection
    if (!ensure_prepared_statements(conn)) {
        fprintf(stderr, "Failed to prepare statements on main connection\n");
        *success = false;
        return DirStop;
    }
    defer({ cleanup_prepared_statements(conn); });

    // Insert file into database
    const char* values[] = {name, path};
    PGresult* res =
        pgpool_execute_prepared(conn, stmt_insert, 2, values, NULL, NULL, 0, TIMEOUT_MS);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        *success = false;
        fprintf(stderr, "Failed to insert file %s: %s\n", path, pgpool_error_message(conn));
        if (res) {
            PQclear(res);
        }
        return DirStop;
    }
    defer({ PQclear(res); });

    printf(">>> Inserted PDF: %s\n", path);
    *success = true;

    // Get the file ID for the inserted/existing file
    const char* file_path_param[] = {path};
    PGresult* id_res =
        pgpool_execute_prepared(conn, file_id_stmt, 1, file_path_param, NULL, NULL, 0, TIMEOUT_MS);
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
        *data->success = false;
        printf("Error opening PDF: %s\n", error->message);
        g_error_free(error);
        return DirStop;
    }

    int npages = poppler_document_get_n_pages(doc);
    if (npages == 0) {
        *data->success = true;
        g_object_unref(doc);
        return DirContinue;
    }

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
    for (int page = 0; page < npages; page++) {
        printf("[%s]: Page %d of %d\n", name, page + 1, npages);

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

        // Acquire a database connection for this worker thread
        pgconn_t* thread_conn = pgpool_acquire(pool, 10000);  // Wait 10s for a connection
        if (!thread_conn) {
            printf("Timeout waiting for a connection\n");
            free(pp);

            *data->success = false;
            // Clean up if we can't get a connection
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
        pp->conn      = thread_conn;
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
bool process_pdfs(const char* root_dir) {
    bool success = false;

    // Acquire connection with automatic cleanup
    pgconn_t* conn = pgpool_acquire(pool, 1000);
    ASSERT_NOT_NULL(conn && "Failed to acquire connection from pool");
    defer({ pgpool_release(pool, conn); });

    // Create threadpool with automatic cleanup
    Threadpool* threadpool = threadpool_create(4);
    if (!threadpool) {
        puts("Failed to create threadpool");
        return false;
    }
    defer({ threadpool_destroy(threadpool, -1); });

    // Begin transaction with automatic commit/rollback
    ASSERT_TRUE(pgpool_begin(conn) && "Failed to BEGIN transaction");
    defer({
        if (success) {
            if (!pgpool_commit(conn)) {
                fprintf(stderr, "Failed to commit transaction\n");
            }
        } else {
            if (!pgpool_rollback(conn)) {
                fprintf(stderr, "Failed to rollback transaction\n");
            }
        }
    });

    // Start walking the root directory
    WalkDirUserData data = {.conn = conn, .thpool = threadpool, .success = &success};
    if (dir_walk(root_dir, walk_dir_callback, &data) != 0) {
        return false;
    }

    return success;
}

/**
 * Main entry point for the PDF indexing command.
 * Extracts the root directory from command arguments and starts the PDF processing.
 *
 * @param cmd Command structure containing parsed command-line arguments.
 */
void build_pdf_index(Command* cmd) {
    char* root_dir = FlagValue_STRING(cmd, "root", NULL);
    ASSERT_NOT_NULL(root_dir);

    process_pdfs(root_dir);
    pgpool_destroy(pool);  // clean up all connections since we are quiting
    exit(EXIT_SUCCESS);
}
