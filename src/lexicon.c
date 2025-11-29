#include <curl/curl.h>
#include <pgconn/pgconn.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/dotenv.h>
#include <solidc/flags.h>
#include <solidc/macros.h>

#include "../include/ai.h"
#include "../include/cli.h"
#include "../include/database.h"
#include "../include/logger.h"
#include "../include/routes.h"

#define INFO(msg) printf(">>> %s\n", msg);

typedef struct {
    char* bind_addr;  // Address to bind to. Default is NULL (0.0.0.0)
    char* pgconn;     // postgres connection string.
    char* root_dir;   // Root Dir of PDFs to index.
    int port;         // Server port.
    int min_pages;    // Minimum number of pages in a PDF to be indexable. Default is 4
    bool dryrun;      // Perform dry-run
} AppConfig;

static AppConfig config = {.port = 8080, .min_pages = 4};

void ensure_valid_pgconn() {
    if (config.pgconn) {
        return;
    }

    config.pgconn = getenv("PGCONN");
    if (config.pgconn == NULL) {
        puts("PGCONN environment variable must be set or pass --pgconn flag to the program");
        exit(EXIT_FAILURE);
    }
}

// Connect to database before invoking handler.
static void pre_exec_func(void* user_data) {
    AppConfig* c = user_data;
    ensure_valid_pgconn();
    init_connections(c->pgconn);
    create_schema();
}

// Handler for the index subcommand.
static void build_index_handler(void* user_data) {
    AppConfig* appcfg      = user_data;
    pgconn_config_t pg_cfg = {
        .conninfo               = config.pgconn,
        .auto_reconnect         = true,
        .max_reconnect_attempts = 5,
        .thread_safe            = true,
    };

    process_pdfs(&pg_cfg, appcfg->root_dir, appcfg->min_pages, appcfg->dryrun);
    exit(EXIT_SUCCESS);
}

void cors(PulsarCtx* ctx) {
    /* Add CORS headers for cross-origin clients */
    static const char cors_hdr[] =
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
        "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(ctx->conn, cors_hdr, sizeof(cors_hdr) - 1);
}

static void init() {
    curl_global_init(0);
    ASSERT(init_response_cache(1024, 300));
    ASSERT(init_ai_cache(500, 24 * 60 * 60));
}

static void cleanup() {
    close_connections();
    destroy_response_cache();
    destroy_ai_cache();
    curl_global_cleanup();
}

int main(int argc, char* argv[]) {
    // Load .env if exists
    load_dotenv(".env");

    init();
    defer({ cleanup(); });

    ensure_valid_pgconn();

    FlagParser* parser = flag_parser_new("lexicon", "Fast PDF indexer and server");
    ASSERT_NOT_NULL(parser);
    defer({ flag_parser_free(parser); });

    // Global flags
    flag_int(parser, "port", 'p', "The server port", &config.port);
    flag_string(parser, "addr", 'a', "Bind address", &config.bind_addr);
    flag_string(parser, "pgconn", 'c', "Postgres connection URI", &config.pgconn);

    // Index subcommand
    FlagParser* index = flag_add_subcommand(parser, "index", "Build PDF index into the database", build_index_handler);
    ASSERT_NOT_NULL(index);
    flag_req_string(index, "root", 'r', "Root directory of pdfs", &config.root_dir);
    flag_int(index, "min_pages", 'p', "Minimum number of pages in PDF to be indexed", &config.min_pages);
    flag_bool(index, "dryrun", 'r', "Perform dry-run without commiting changes", &config.dryrun);

    // pre-invoke command.
    flag_set_pre_invoke(parser, pre_exec_func);

    // Set the server config as user-data for all handlers.
    FlagStatus status = flag_parse_and_invoke(parser, argc, argv, &config);
    if (status != FLAG_OK) {
        fprintf(stderr, "ERROR: %s(code=%d)\n", flag_status_str(status), status);
        flag_print_usage(parser);
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
    pulsar_set_callback(logger);

    Middleware mw[] = {cors};
    use_global_middleware(mw, sizeof(mw) / sizeof(mw[0]));

    int code = pulsar_run(config.bind_addr, config.port);
    printf("Server exited with status code: %d\n", code);
    return code;
}
