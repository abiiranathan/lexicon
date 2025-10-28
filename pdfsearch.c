#include <pgconn/pgconn.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/flag.h>
#include <solidc/macros.h>

#include "cli.h"

#define INFO(msg) printf(">>> %s\n", msg);

typedef struct {
    char* bind_addr;  // Address to bind to. Default is NULL (0.0.0.0)
    char* pgconn;     // postgres connection string.
    char* root_dir;   // Root Dir of PDFs to index.
    int port;         // Server port.
} server_config_t;

extern void pdf_search(PulsarCtx* ctx);
extern void list_files(PulsarCtx* ctx);
extern void get_file_by_id(PulsarCtx* ctx);
extern void get_page_by_file_and_page(PulsarCtx* ctx);
extern void render_pdf_page_as_png(PulsarCtx* ctx);

// Static pool of postgres connections.
// Each connections is used for a single thread run by pulsar server.
pgconn_t* connections[NUM_WORKERS] = {};

#define NUM_SCHEMA 4
static server_config_t config = {.port = 8080};

static const char* schemas[NUM_SCHEMA] = {
    // Files schema
    "CREATE TABLE IF NOT EXISTS files ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    name TEXT NOT NULL, "
    "    num_pages INT NOT NULL, "
    "    path TEXT NOT NULL UNIQUE"
    ")",

    // Pages schema
    "CREATE TABLE IF NOT EXISTS pages ("
    "    id BIGSERIAL PRIMARY KEY, "
    "    file_id BIGINT NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
    "    page_num INTEGER NOT NULL, "
    "    text TEXT NOT NULL, "
    "    text_vector tsvector GENERATED ALWAYS AS (to_tsvector('english', text)) STORED, "
    "    UNIQUE(file_id, page_num)"
    ")",

    // Index for efficient text search
    "CREATE INDEX IF NOT EXISTS idx_pages_text_vector ON pages USING GIN(text_vector)",

    // Index for page lookups.
    "CREATE INDEX IF NOT EXISTS idx_pages_file_id ON pages(file_id)",
};

static void initConnections(void) {
    pgconn_config_t cfg = {
        .conninfo               = config.pgconn,
        .auto_reconnect         = true,
        .max_reconnect_attempts = 5,
        .thread_safe            = true,
    };

    int i;
    for (i = 0; i < NUM_WORKERS; i++) {
        connections[i] = pgconn_create(&cfg);
        if (!connections[i]) {
            printf("Connection failed at worker: %d\n", i);
            break;
        }
    }

    // Not all connections succeeded.
    if (i != NUM_WORKERS) {
        for (int j = 0; j < i - 1; j++) {
            pgconn_destroy(connections[j]);
        }

        printf("Failed to create %d database connections\n", NUM_WORKERS);
        exit(1);
    }
    printf("Successfully created %d database connections\n", NUM_WORKERS);
}

void closeConnections(void) {
    for (int i = 0; i < NUM_WORKERS; i++) {
        pgconn_destroy(connections[i]);
    }
}

static void create_schema(pgconn_t* conn) {
    for (size_t i = 0; i < NUM_SCHEMA; ++i) {
        if (!pgconn_execute(conn, schemas[i], NULL)) {
            fprintf(stderr, "Unable to create table for schema:\n\n%s\n\n", schemas[i]);
            fprintf(stderr, "%s\n", pgconn_error_message(conn));
            exit(1);
        }
    }
    INFO("Tables created successfully");
}

// Connect to database before invoking handler.
static void pre_exec_func(void*) {
    initConnections();
    create_schema(connections[0]);
}

static void build_index(Command* cmd) {
    char* root_dir  = FlagValue_STRING(cmd, "root", NULL);
    pgconn_t* first = connections[0];
    process_pdfs(root_dir, first);
    exit(EXIT_SUCCESS);
}

// Provided by logger.c
extern void logger(PulsarCtx* ctx, uint64_t total_ns);

void cors(PulsarCtx* ctx) {
    /* Add CORS headers for cross-origin clients */
    const char* cors_hdr =
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
        "OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(ctx->conn, cors_hdr, strlen(cors_hdr));
}

int main(int argc, char* argv[]) {
    AddFlag_INT("port", 'p', "The server port", &config.port, false);
    AddFlag_STRING("addr", 'a', "Bind address", &config.bind_addr, false);
    AddFlag_STRING("pgconn", 'c', "Postgres connection URI", &config.pgconn, true);

    Command* idxcmd = AddCommand("index", "Build PDF index into the database", build_index);
    AddFlagCmd_STRING(idxcmd, "root", 'r', "Root directory of pdfs", &config.root_dir, true);

    FlagParse(argc, argv, pre_exec_func, NULL);

    // Register all routes

    route_register("/api/search", HTTP_GET, pdf_search);
    route_register("/api/list-files", HTTP_GET, list_files);
    route_register("/api/list-files/{file_id}", HTTP_GET, get_file_by_id);

    /* Page detail endpoint */
    route_register("/api/file/{file_id}/page/{page_num}", HTTP_GET, get_page_by_file_and_page);
    route_register("/api/file/{file_id}/render-page/{page_num}", HTTP_GET, render_pdf_page_as_png);

    // Since we are using / for static assets, put at the end to avoid collisions
    route_static("/", "./pdfsearch/dist");

    pulsar_set_callback(logger);

    Middleware mw[] = {cors};
    use_global_middleware(mw, sizeof(mw) / sizeof(mw[0]));

    int code = pulsar_run(config.bind_addr, config.port);

    closeConnections();
    return code;
}
