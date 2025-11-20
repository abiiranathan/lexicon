#include "../include/database.h"

#include <pulsar/pulsar.h>

// Schema array with partitioning.
static const char* schemas[] = {
    // Turn off notices
    "SET client_min_messages = ERROR",

    // Use rum extension for faster tsvector searches instead
    // of GIN index.
    "CREATE EXTENSION IF NOT EXISTS rum",

    // Files schema
    "CREATE TABLE IF NOT EXISTS files ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    name TEXT NOT NULL, "
    "    num_pages INT NOT NULL, "
    "    path TEXT NOT NULL,"
    "    UNIQUE(name, path)"
    ")",

    // Create Pages as partitioned table (hash on file_id)
    "CREATE TABLE IF NOT EXISTS pages ("
    "    id BIGSERIAL, "
    "    file_id BIGINT NOT NULL, "
    "    page_num INTEGER NOT NULL, "
    "    text TEXT NOT NULL, "
    "    text_vector tsvector GENERATED ALWAYS AS (to_tsvector('english', substring(text, 1, 100000))) STORED, "
    "    UNIQUE(file_id, page_num)"
    ") PARTITION BY HASH(file_id)",

    // Create 10 partitions
    "CREATE TABLE IF NOT EXISTS pages_p0 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 0)",
    "CREATE TABLE IF NOT EXISTS pages_p1 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 1)",
    "CREATE TABLE IF NOT EXISTS pages_p2 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 2)",
    "CREATE TABLE IF NOT EXISTS pages_p3 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 3)",
    "CREATE TABLE IF NOT EXISTS pages_p4 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 4)",
    "CREATE TABLE IF NOT EXISTS pages_p5 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 5)",
    "CREATE TABLE IF NOT EXISTS pages_p6 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 6)",
    "CREATE TABLE IF NOT EXISTS pages_p7 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 7)",
    "CREATE TABLE IF NOT EXISTS pages_p8 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 8)",
    "CREATE TABLE IF NOT EXISTS pages_p9 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER 9)",

    "ALTER TABLE pages DROP CONSTRAINT IF EXISTS fk_pages_file_id",

    // Foreign key on partitioned table (PostgreSQL 11+)
    "ALTER TABLE pages ADD CONSTRAINT fk_pages_file_id FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE",

    // RUM Index on each partition (must be created per partition)
    "CREATE INDEX IF NOT EXISTS idx_pages_p0_text_vector ON pages_p0 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p1_text_vector ON pages_p1 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p2_text_vector ON pages_p2 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p3_text_vector ON pages_p3 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p4_text_vector ON pages_p4 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p5_text_vector ON pages_p5 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p6_text_vector ON pages_p6 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p7_text_vector ON pages_p7 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p8_text_vector ON pages_p8 USING rum(text_vector rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p9_text_vector ON pages_p9 USING rum(text_vector rum_tsvector_ops)",

    // Other indexes (can be on parent table, automatically applied to partitions)
    "CREATE INDEX IF NOT EXISTS idx_pages_file_id ON pages(file_id)",
    "CREATE INDEX IF NOT EXISTS idx_pages_page_num ON pages(page_num)",
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