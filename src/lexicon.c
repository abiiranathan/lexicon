#include <curl/curl.h>
#include <pgconn/pgconn.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/dotenv.h>
#include <solidc/flag.h>
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
} server_config_t;

static server_config_t config = {.port = 8080, .min_pages = 4};

void ensure_valid_pgconn() {
    if (config.pgconn == NULL) {
        config.pgconn = getenv("PGCONN");
        if (config.pgconn == NULL) {
            puts("PGCONN environment variable must be set or pass --pgconn flag to the program");
            exit(1);
        }
    }
}

// Connect to database before invoking handler.
static void pre_exec_func(void*) {
    ensure_valid_pgconn();
    init_connections(config.pgconn);
    create_schema();
}

static void build_index(Command* cmd) {
    char* root_dir = FlagValue_STRING(cmd, "root", NULL);
    ASSERT_NOT_NULL(root_dir);

    size_t min_pages    = FlagValue_SIZE_T(cmd, "min_pages", 4);
    bool dryrun         = FlagValue_INT(cmd, "dryrun", false);
    pgconn_config_t cfg = {
        .conninfo               = config.pgconn,
        .auto_reconnect         = true,
        .max_reconnect_attempts = 5,
        .thread_safe            = true,
    };

    process_pdfs(&cfg, root_dir, min_pages, dryrun);
    exit(EXIT_SUCCESS);
}

void cors(PulsarCtx* ctx) {
    /* Add CORS headers for cross-origin clients */
    const char* cors_hdr =
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
        "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(ctx->conn, cors_hdr, strlen(cors_hdr));
}

static void init() {
    curl_global_init(0);
    ASSERT(init_response_cache(1024, 300));  // 1024 entries, 300 sec TTL
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
    ensure_valid_pgconn();

    AddFlag_INT("port", 'p', "The server port", &config.port, false);
    AddFlag_STRING("addr", 'a', "Bind address", &config.bind_addr, false);
    AddFlag_STRING("pgconn", 'c', "Postgres connection URI", &config.pgconn, false);

    Command* idxcmd = AddCommand("index", "Build PDF index into the database", build_index);
    AddFlagCmd_STRING(idxcmd, "root", 'r', "Root directory of pdfs", &config.root_dir, true);
    AddFlagCmd_INT(idxcmd, "min_pages", 'p', "Minimum number of pages in PDF to be indexed", &config.min_pages, false);
    AddFlagCmd_BOOL(idxcmd, "dryrun", 'r', "Perform dry-run without commiting changes", &config.dryrun, false);

    FlagParse(argc, argv, pre_exec_func, NULL);

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

    cleanup();
    return code;
}
