#include "../include/database.h"

#include <pulsar/pulsar.h>

// Schema array.
static const char* schemas[] = {
    // Turn off notices
    "SET client_min_messages = ERROR",

    // Files schema
    "CREATE TABLE IF NOT EXISTS files ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    name TEXT NOT NULL, "
    "    num_pages INT NOT NULL, "
    "    path TEXT NOT NULL,"
    "    UNIQUE(name, path)"
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
    "CREATE INDEX IF NOT EXISTS idx_pages_page_num ON pages(page_num)",

    // -- Composite index for filtering
    "CREATE INDEX IF NOT EXISTS idx_pages_lookup ON pages(file_id, page_num)",
};

// Array size of the schemas.
#define NUM_SCHEMA (sizeof(schemas) / sizeof(schemas[0]))

// Static pool of postgres connections.
// Each connections is used by a single server thread, eliminating need
// for locking.
static pgconn_t* connections[NUM_WORKERS] = {};

void init_connections(const char* conn_string) {
    pgconn_config_t cfg = {
        .conninfo               = conn_string,  // Connection string
        .auto_reconnect         = true,         // Auto reconnect of failure
        .max_reconnect_attempts = 5,            // Max attempts
        .thread_safe            = false,        // each worker has its connection
    };

    size_t i;
    for (i = 0; i < NUM_WORKERS; i++) {
        connections[i] = pgconn_create(&cfg);
        if (!connections[i]) {
            printf("Connection failed at worker: %d\n", i);
            break;
        }
    }

    // Not all connections succeeded.
    if (i != NUM_WORKERS) {
        for (size_t j = 0; j < i - 1; j++) {
            pgconn_destroy(connections[j]);
        }
        exit(EXIT_FAILURE);
    }
}

void close_connections(void) {
    for (size_t i = 0; i < NUM_WORKERS; i++) {
        pgconn_destroy(connections[i]);
    }
}

void create_schema() {
    ASSERT(connections[0]);

    pgconn_t* conn = connections[0];
    for (size_t i = 0; i < NUM_SCHEMA; ++i) {
        if (!pgconn_execute(conn, schemas[i], NULL)) {
            fprintf(stderr, "%s\n", pgconn_error_message(conn));
            exit(EXIT_FAILURE);
        }
    }
}

pgconn_t* get_pgconn(PulsarConn* conn) {
    const int worker = conn_worker_id(conn);
    ASSERT(worker >= 0 && worker < NUM_WORKERS);
    return connections[worker];
}