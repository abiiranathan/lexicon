#include <pgpool.h>
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

static server_config_t config = {.port = 8080};
pgpool_t* pool                = NULL;  // Connection pool
const int TIMEOUT_MS          = 5000;  // Default database timeout is 5 seconds

#define NUM_SCHEMA 4
static const char* schemas[NUM_SCHEMA] = {
    // Files schema
    "CREATE TABLE IF NOT EXISTS files ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    name TEXT NOT NULL, "
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

static void create_schema() {
    pgconn_t* dbconn = pgpool_acquire(pool, TIMEOUT_MS);
    ASSERT_NOT_NULL(dbconn && "Unable to acquire connection");
    defer({ pgpool_release(pool, dbconn); });

    for (size_t i = 0; i < NUM_SCHEMA; ++i) {
        if (!pgpool_execute(dbconn, schemas[i], TIMEOUT_MS)) {
            fprintf(stderr, "Unable to create table for schema:\n\n%s\n\n", schemas[i]);
            exit(EXIT_FAILURE);
        }
    }
    INFO("Tables created successfully");
}

static void connect_database() {
    pgpool_config_t c = {
        .auto_reconnect  = true,
        .conninfo        = config.pgconn,
        .max_connections = 100,
        .min_connections = 10,
        .connect_timeout = 5,
    };

    pool = pgpool_create(&c);
    if (pool == NULL) {
        fprintf(stderr, "Failed to connect to postgres database\n");
        exit(EXIT_FAILURE);
    }
    INFO("Connected to postgres database");
    create_schema();
}

extern void home_route(connection_t* conn);
extern void pdf_search(connection_t* conn);
extern void list_files(connection_t* conn);
extern void get_file_by_id(connection_t* conn);

// Connect to database before invoking handler.
static void pre_exec(void*) {
    connect_database();
}

int main(int argc, char* argv[]) {
    AddFlag_INT("port", 'p', "The server port", &config.port, false);
    AddFlag_STRING("addr", 'a', "Bind address", &config.bind_addr, false);
    AddFlag_STRING("pgconn", 'c', "Postgres connection URI", &config.pgconn, true);

    Command* idxcmd = AddCommand("index", "Build PDF index into the database", build_pdf_index);
    AddFlagCmd_STRING(idxcmd, "root", 'r', "Root directory of pdfs", &config.root_dir, true);

    FlagParse(argc, argv, pre_exec, NULL);

    connect_database();
    defer({ pgpool_destroy(pool); });

    // Register all routes
    route_register("/", HTTP_GET, home_route);
    route_register("/api/search", HTTP_GET, pdf_search);
    route_register("/api/list-files", HTTP_GET, list_files);
    route_register("/api/list-files/{file_id}", HTTP_GET, get_file_by_id);

    return pulsar_run(config.bind_addr, config.port);
}
