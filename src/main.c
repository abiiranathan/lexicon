#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/dotenv.h>
#include <solidc/flags.h>
#include <solidc/macros.h>

#include "../include/database.h"
#include "../include/pdf_indexer.h"
#include "../include/routes.h"

typedef struct {
    char* bind_addr;  // Address to bind to. Default is NULL (0.0.0.0)
    char* pgconn;     // postgres connection string.
    char* root_dir;   // Root Dir of PDFs to index.
    int port;         // Server port.
    int min_pages;    // Minimum number of pages in a PDF to be indexable. Default is 4
    bool dryrun;      // Perform dry-run
} AppConfig;

static AppConfig config = {.port = 8080, .min_pages = 4};
FlagParser *root = NULL, *indexer = NULL;

#define INFO(msg) printf(">>> %s\n", msg);

// Checks for non-empty postgres connection string.
void ensure_valid_pgconn_string() {
    // Use config connection if passed.
    if (config.pgconn != NULL) {
        return;
    }

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
    ensure_valid_pgconn_string();
    create_connection_pool(c->pgconn);
    create_schema();
    puts("Connected to database and initialized schema");
}

// Handler for the index subcommand.
static void cli_build_pdf_index(void* user_data) {
    AppConfig* appcfg = user_data;
    process_pdfs(appcfg->root_dir, appcfg->min_pages, appcfg->dryrun);
    exit(EXIT_SUCCESS);
}

// CORS middleware.
void cors(PulsarCtx* ctx) {
    /* Add CORS headers for font-end during dev */
    static const char cors_headers[] =
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
        "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(ctx->conn, cors_headers, sizeof(cors_headers) - 1);
}

void init() {
    // Initialize a fast in-memory cache with 1024 default entries.
    // Default TTL is 2 hours
    if (!init_response_cache(1024, 2 * 60)) {
        LOG_ERROR("failed to initialize in-memory cache");
        exit(EXIT_FAILURE);
    }
}

void cleanup() {
    close_connections();
    destroy_response_cache();
}

int main(int argc, char* argv[]) {
    // Load .env if exists
    load_dotenv(".env");
    init();
    defer {
        cleanup();
    };
    ensure_valid_pgconn_string();
    root = flag_parser_new("lexicon", "Fast PDF indexer and server");
    defer {
        flag_parser_free(root);
    };

    // Global flags
    flag_int(root, "port", 'p', "The server port", &config.port);
    flag_string(root, "addr", 'a', "Bind address", &config.bind_addr);
    flag_string(root, "pgconn", 'c', "Postgres connection URI", &config.pgconn);

    // Index subcommand and its flags.
    indexer = flag_add_subcommand(root, "index", "Build PDF index into the db", cli_build_pdf_index);
    flag_req_string(indexer, "root", 'r', "Root directory of pdfs", &config.root_dir);
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

    // Register all routes
    route_register("/api/search", HTTP_GET, pdf_search);
    route_register("/api/list-files", HTTP_GET, list_files);
    route_register("/api/list-files/{file_id}", HTTP_GET, get_file_by_id);

    /* Page detail endpoint */
    route_register("/api/file/{file_id}/page/{page_num}", HTTP_GET, get_page_by_file_and_page);
    route_register("/api/file/{file_id}/render-page/{page_num}", HTTP_GET, render_pdf_page_as_png);

    // Since we are using / for static assets, put at the end to avoid collisions
    route_static("/", "./ui/dist");
    pulsar_set_callback(pulsar_logger, STDOUT_FILENO);

    Middleware mw[] = {cors};
    use_global_middleware(mw, sizeof(mw) / sizeof(mw[0]));

    int code = pulsar_run(config.bind_addr, config.port);
    printf("Server exited with status code: %d\n", code);
    return code;
}
