#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/dotenv.h>
#include <solidc/flags.h>
#include <solidc/macros.h>
#include <vfs.h>

#include "../include/database.h"
#include "../include/pdf_indexer.h"
#include "../include/routes.h"

typedef struct {
    char* vfs_path;      // Path to Virtual File System image containing embedded PDFs.
    char* bind_addr;     // Address to bind to. Default is NULL (0.0.0.0)
    char* pgconn;        // postgres connection string.
    char* root_dir;      // Root Dir of PDFs to index.
    char* frontend_dir;  // Directory with build assets
    int port;            // Server port.
    int min_pages;       // Minimum number of pages in a PDF to be indexable. Default is 4
    bool dryrun;         // Perform dry-run
} AppConfig;

static AppConfig config = {.port = 8080, .min_pages = 4, .frontend_dir = "./ui/dist"};
FlagParser *root = NULL, *indexer = NULL;

// Virtual File System image. (exported to other files)
vfs_t* vfs = NULL;
char* conn_info = NULL;

// Checks for non-empty postgres connection string.
void ensure_valid_pgconn_string() {
    // Use config connection if passed.
    if (config.pgconn != NULL) return;

    // Fallback to environment variable PGCONN and fail if not dound.
    config.pgconn = getenv("PGCONN");
    if (config.pgconn == NULL) {
        puts("PGCONN environment variable must be set or pass --pgconn flag to the program");
        exit(EXIT_FAILURE);
    }
}

// Connect to database before invoking handler.
static void cli_pre_exec_handler(void* user_data) {
    AppConfig* c = user_data;
    if (c->vfs_path) {
        vfs_status_t status = vfs_open(c->vfs_path, true, &vfs);
        if (status != VFS_OK) {
            LOG_ERROR("failed to open VFS: %s -- %s", c->vfs_path, vfs_strerror(status));
            exit(EXIT_FAILURE);
        }
    }

    ensure_valid_pgconn_string();

    // Set global variable.
    conn_info = c->pgconn;

    create_connection_pool(c->pgconn);
    create_schema();
    puts("Connected to database and initialized schema");
}

// Handler for the index subcommand.
static void cli_build_pdf_index(void* user_data) {
    AppConfig* appcfg = user_data;
    if (!vfs && !appcfg->root_dir) {
        fprintf(stderr, "--root directory must be provided when not using a VFS\n");
        exit(EXIT_FAILURE);
    }
    process_pdfs(appcfg->root_dir, appcfg->min_pages, appcfg->dryrun);
    exit(EXIT_SUCCESS);
}

// CORS middleware.
void cors(PulsarCtx* ctx) {
    /* Add CORS headers for font-end during dev */
    static const char cors_headers
        [] = "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
             "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(ctx->conn, cors_headers, sizeof(cors_headers) - 1);
}

int main(int argc, char* argv[]) {
    load_dotenv(".env");
    defer_call(close_connections);

    ensure_valid_pgconn_string();
    root = flag_parser_new("lexicon", "Fast PDF indexer and server");
    defer_call1(flag_parser_free, root);

    // Global flags
    flag_int(root, "port", 'p', "The server port", &config.port);
    flag_string(root, "addr", 'a', "Bind address", &config.bind_addr);
    flag_string(root, "pgconn", 'c', "Postgres connection URI", &config.pgconn);
    flag_string(root, "build-dir", 'd', "Frontend build directory", &config.frontend_dir);
    flag_string(root, "vfs", 'v', "Path to Virtual File System image (.vfs)", &config.vfs_path);

    // Index subcommand and its flags.
    indexer = flag_add_subcommand(root, "index", "Build PDF index into the db", cli_build_pdf_index);
    flag_string(indexer, "root", 'r', "Root directory of pdfs", &config.root_dir);
    flag_int(indexer, "min_pages", 'p', "Minimum number of pages in PDF", &config.min_pages);
    flag_bool(indexer, "dryrun", 'r', "Perform dry-run without commiting changes", &config.dryrun);

    // Before the matching subcommand runs, we need to connect to the database.
    flag_set_pre_invoke(root, cli_pre_exec_handler);

    // Set the server config as user-data for all handlers.
    FlagStatus status = flag_parse_and_invoke(root, argc, argv, &config);
    if (status != FLAG_OK) {
        fprintf(stderr, "ERROR: %s\n", flag_status_str(status));
        flag_print_usage(root);
        exit(EXIT_FAILURE);
    }

    if (!is_dir(config.frontend_dir)) {
        fprintf(stderr, "ERROR: --build-dir must specify a valid directory.\n");
        flag_print_usage(root);
        exit(EXIT_FAILURE);
    }

    // Register all routes
    route_get("/api/search", pdf_search);
    route_get("/api/list-files", list_files);
    route_get("/api/list-files/{file_id}", get_file_by_id);

    /* Page detail endpoint */
    route_get("/api/file/{file_id}/page/{page_num}", get_page_by_file_and_page);
    route_get("/api/file/{file_id}/render-page/{page_num}", render_pdfpage_as_image);

    // Since we are using / for static assets, put at the end to avoid collisions
    printf("Serving SPA at: %s\n", config.frontend_dir);
    route_static("/", config.frontend_dir);
    pulsar_set_callback(pulsar_logger, STDOUT_FILENO);

    // Attach CORS middleware globally.
    Middleware mw[1] = {cors};
    use_global_middleware(mw, ARRAY_SIZE(mw));

    return pulsar_run(config.bind_addr, config.port);
}
