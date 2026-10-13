#include "../include/database.h"

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

    "CREATE TABLE IF NOT EXISTS ocr_pages ("
    "    file_id BIGINT NOT NULL REFERENCES files(id) ON DELETE CASCADE, "
    "    page_num INTEGER NOT NULL, "
    "    result JSONB NOT NULL, "
    "    PRIMARY KEY (file_id, page_num)"
    ")",

    // OAuth users. is_admin is derived from the ADMIN_EMAILS allowlist at
    // login time; the allowlist stays the source of truth.
    "CREATE TABLE IF NOT EXISTS users ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    provider TEXT NOT NULL, "
    "    provider_sub TEXT NOT NULL, "
    "    email TEXT NOT NULL, "
    "    is_admin BOOLEAN NOT NULL DEFAULT FALSE, "
    "    created_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
    "    last_login_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
    "    UNIQUE(provider, provider_sub)"
    ")",

    // Opaque session tokens: only SHA-256 hex digests are stored.
    "CREATE TABLE IF NOT EXISTS sessions ("
    "    token_hash TEXT NOT NULL PRIMARY KEY, "
    "    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE, "
    "    expires_at TIMESTAMPTZ NOT NULL"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_sessions_user_id ON sessions(user_id)",

    // Single-use OAuth state/nonce values (CSRF protection).
    "CREATE TABLE IF NOT EXISTS oauth_states ("
    "    state TEXT NOT NULL PRIMARY KEY, "
    "    provider TEXT NOT NULL, "
    "    code_verifier TEXT NOT NULL, "
    "    expires_at TIMESTAMPTZ NOT NULL"
    ")",

    // Chunked PDF upload jobs processed by a background worker.
    "CREATE TABLE IF NOT EXISTS upload_jobs ("
    "    id BIGSERIAL NOT NULL PRIMARY KEY, "
    "    filename TEXT NOT NULL, "
    "    staged_path TEXT NOT NULL, "
    "    final_path TEXT, "
    "    total_bytes BIGINT NOT NULL, "
    "    received_bytes BIGINT NOT NULL DEFAULT 0, "
    "    status TEXT NOT NULL DEFAULT 'uploading', "
    "    error TEXT, "
    "    file_id BIGINT REFERENCES files(id) ON DELETE SET NULL, "
    "    created_by BIGINT REFERENCES users(id) ON DELETE SET NULL, "
    "    created_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
    "    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()"
    ")",

    // VFS destination for uploads into images (NULL for host-filesystem
    // uploads). ADD COLUMN IF NOT EXISTS keeps existing tables working.
    "ALTER TABLE upload_jobs ADD COLUMN IF NOT EXISTS vfs_path TEXT",

    // Create Pages as partitioned table (hash on file_id)
    "CREATE TABLE IF NOT EXISTS pages ("
    "    id BIGSERIAL, "
    "    file_id BIGINT NOT NULL, "
    "    page_num INTEGER NOT NULL, "
    "    text TEXT NOT NULL, "
    "    text_vector tsvector GENERATED ALWAYS AS (to_tsvector('english', substring(text, 1, "
    "100000))) STORED, "
    "    UNIQUE(file_id, page_num)"
    ") PARTITION BY HASH(file_id)",

    // Create 10 partitions
    "CREATE TABLE IF NOT EXISTS pages_p0 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "0)",    "CREATE TABLE IF NOT EXISTS pages_p1 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "1)",
    "CREATE TABLE IF NOT EXISTS pages_p2 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "2)",
    "CREATE TABLE IF NOT EXISTS pages_p3 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "3)",
    "CREATE TABLE IF NOT EXISTS pages_p4 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "4)",
    "CREATE TABLE IF NOT EXISTS pages_p5 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "5)",
    "CREATE TABLE IF NOT EXISTS pages_p6 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "6)",
    "CREATE TABLE IF NOT EXISTS pages_p7 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "7)",
    "CREATE TABLE IF NOT EXISTS pages_p8 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "8)",
    "CREATE TABLE IF NOT EXISTS pages_p9 PARTITION OF pages FOR VALUES WITH (MODULUS 10, REMAINDER "
    "9)",

    "ALTER TABLE pages DROP CONSTRAINT IF EXISTS fk_pages_file_id",

    // Foreign key on partitioned table (PostgreSQL 11+)
    "ALTER TABLE pages ADD CONSTRAINT fk_pages_file_id FOREIGN KEY (file_id) REFERENCES files(id) "
    "ON DELETE CASCADE",

    // RUM Index on each partition (must be created per partition)
    "CREATE INDEX IF NOT EXISTS idx_pages_p0_text_vector ON pages_p0 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p1_text_vector ON pages_p1 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p2_text_vector ON pages_p2 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p3_text_vector ON pages_p3 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p4_text_vector ON pages_p4 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p5_text_vector ON pages_p5 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p6_text_vector ON pages_p6 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p7_text_vector ON pages_p7 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p8_text_vector ON pages_p8 USING rum(text_vector "
    "rum_tsvector_ops)",
    "CREATE INDEX IF NOT EXISTS idx_pages_p9_text_vector ON pages_p9 USING rum(text_vector "
    "rum_tsvector_ops)",

    // Other indexes (can be on parent table, automatically applied to partitions)
    "CREATE INDEX IF NOT EXISTS idx_pages_file_id ON pages(file_id)",
    "CREATE INDEX IF NOT EXISTS idx_pages_page_num ON pages(page_num)",
    "CREATE INDEX IF NOT EXISTS idx_pages_lookup ON pages(file_id, page_num)",
};

// Array size of the schemas.
#define NUM_SCHEMA (sizeof(schemas) / sizeof(schemas[0]))

// Exported so that callers can access it.
pgpool_t* pool = NULL;

// Create and initialialize the connection pool.
void create_connection_pool(const char* conn_string) {
    pgpool_config_t cfg = {
        .conninfo = conn_string,
        .auto_reconnect = true,
        .min_connections = 10,
        .max_connections = 20,
        .connect_timeout = 10,
    };
    pool = pgpool_create(&cfg);
    ASSERT(pool != NULL && "connection pool creation failed");
}

// Destroys a connection pool and releases all resources.
void close_connections(void) {
    if (pool) pgpool_destroy(pool, 1000);
}

// Create all the table schema and partitions, RUM extension and associated INDEXES.
void create_schema() {
    pgconn_t* conn = pgpool_acquire(pool, 1000);
    ASSERT(conn && "failed to acquired connection from pool");

    for (size_t i = 0; i < NUM_SCHEMA; ++i) {
        if (!pgpool_execute(conn, schemas[i], 5000)) {
            fprintf(stderr, "%s\n", pgpool_error_message(conn));
            close_connections();
            exit(EXIT_FAILURE);
        }
    }
}
