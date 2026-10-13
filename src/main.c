#include <lexicon/lexicon_pdf.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/dotenv.h>
#include <solidc/flags.h>
#include <solidc/macros.h>
#include <vfs.h>

#include "../include/database.h"
#include "../include/pdf_indexer.h"
#include "../include/routes.h"
#include "../include/auth.h"
#include "../include/upload.h"
#include "../include/vfs_store.h"

typedef struct {
    char* vfs_path;      // Path to Virtual File System image containing embedded PDFs.
    char* bind_addr;     // Address to bind to. Default is NULL (0.0.0.0)
    char* pgconn;        // postgres connection string.
    char* root_dir;      // Root Dir of PDFs to index.
    char* filename;      // Index one file instead of directory traversal.
    char* frontend_dir;  // Directory with build assets
    int port;            // Server port.
    int min_pages;       // Minimum number of pages in a PDF to be indexable. Default is 4
    bool dryrun;         // Perform dry-run
} AppConfig;

static AppConfig config = {
    .port = 8080,
    .min_pages = 4,
    .frontend_dir = "./ui/dist",
};

FlagParser *root = NULL, *idxcmd = NULL;
vfs_t* vfs = NULL;
char* conn_info = NULL;

// Checks for non-empty postgres connection string.
void ensure_valid_pgconn_string() {
    // Use config connection if passed.
    if (config.pgconn != NULL) return;

    // Fallback to environment variable.
    config.pgconn = getenv("PGCONN");
    if (config.pgconn == NULL) {
        LOG_FATAL("PGCONN environment variable must be set or pass --pgconn flag to the program");
    }
}

// Connect to database before invoking handler.
static void cli_pre_exec_handler(void* user_data) {
    AppConfig* c = user_data;
    if (c->vfs_path) {
        vfs_status_t status = vfs_open(c->vfs_path, true, &vfs);
        if (status != VFS_OK) {
            LOG_FATAL("failed to open VFS: %s -- %s", c->vfs_path, vfs_strerror(status));
        }
    }

    ensure_valid_pgconn_string();

    // Set global variable.
    conn_info = c->pgconn;

    create_connection_pool(c->pgconn);
    create_schema();
    puts("Connected to database and initialized schema");
}

// Upload base directory: UPLOAD_DIR, falling back to the library root.
// NULL when neither is set (e.g. VFS-only mode) — uploads are refused then.
const char* app_upload_dir(void) {
    const char* env = getenv("UPLOAD_DIR");
    if (env && *env) return env;
    return config.root_dir;
}

// Minimum PDF pages for indexing (server configuration).
int app_min_pages(void) {
    return config.min_pages;
}

// VFS image path when serving from an image, else NULL.
const char* app_vfs_path(void) {
    return config.vfs_path;
}

// Effective Postgres connection string. ensure_valid_pgconn_string() runs
// before serve/index paths, so this is never NULL there.
const char* app_pgconn(void) {
    return config.pgconn;
}

// Handler for the index subcommand.
static void cli_build_pdf_index(void* user_data) {
    AppConfig* appcfg = user_data;
    if (!vfs && !appcfg->root_dir) {
        LOG_FATAL("--root directory must be provided when not using a VFS");
    }

    process_pdfs(appcfg->root_dir, appcfg->filename, appcfg->min_pages, appcfg->dryrun);
    puts("PDF indexing completed successfully");
}

// CORS middleware.
static void write_cors_headers(PulsarConn* conn) {
    /* Echo a present Origin so credentialed (cookie) cross-origin requests
     * work during development; fall back to the permissive wildcard
     * otherwise. The public read API stays open either way. Reflected
     * origins are restricted to visible ASCII without whitespace to block
     * header injection. */
    const headers_t* headers = req_headers(conn);
    char origin[256] = {0};
    if (headers) {
        const StrSlice value = headers_get(headers, "Origin");
        if (value.data && value.len > 0 && value.len < sizeof(origin)) {
            bool clean = true;
            for (size_t i = 0; i < value.len; i++) {
                const char c = value.data[i];
                if (c <= ' ' || c >= 0x7f) {
                    clean = false;
                    break;
                }
            }
            if (clean) {
                memcpy(origin, value.data, value.len);
                origin[value.len] = '\0';
            }
        }
    }
    if (origin[0]) {
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "Access-Control-Allow-Origin: %s\r\n"
                 "Vary: Origin\r\n"
                 "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                 "Access-Control-Allow-Credentials: true\r\n",
                 origin);
        conn_writeheader_raw(conn, buf, strlen(buf));
        return;
    }
    static const char cors_headers[] =
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, PUT, PATCH, "
        "DELETE, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\n";
    conn_writeheader_raw(conn, cors_headers, sizeof(cors_headers) - 1);
}

void cors(PulsarCtx* ctx) {
    write_cors_headers(ctx->conn);
}

// Preflight responder for mutating endpoints. Browsers preflight
// cross-origin non-simple requests (DELETE, JSON/octet-stream POSTs with
// credentials); unmatched paths 404 before middleware, so these explicit
// OPTIONS routes exist to answer with 204. CORS headers are already written
// by the global middleware, which always runs first. No auth here:
// preflights never carry credentials.
void cors_preflight(PulsarCtx* ctx) {
    conn_send(ctx->conn, StatusNoContent, "", 0);
}

void cleanup(void) {
    upload_system_shutdown();
    vfs_store_close();
    close_connections();
    pdf_library_destroy();
}

int main(int argc, char* argv[]) {
    srand(time(NULL));
    pdf_library_init();
    load_dotenv(".env");

    ensure_valid_pgconn_string();
    root = flag_parser_new("lexicon", "Fast PDF indexer and server");
    defer_call1(flag_parser_free, root);

    // Register persistent flags that are available to all subcommands.
    flag_persistent_string(root, "vfs", 'v', "Path to Virtual File System image (.vfs)",
                           &config.vfs_path);
    flag_persistent_string(root, "root", 'r', "Root directory of pdfs", &config.root_dir);

    // Global flags
    flag_int(root, "port", 'p', "The server port", &config.port);
    flag_string(root, "addr", 'a', "Bind address", &config.bind_addr);
    flag_persistent_string(root, "pgconn", 'c', "Postgres connection URI", &config.pgconn);
    flag_string(root, "build-dir", 'd', "Frontend build directory", &config.frontend_dir);

    // Index subcommand and its flags.
    idxcmd = flag_add_subcommand(root, "index", "Build PDF index into the db", cli_build_pdf_index);
    flag_int(idxcmd, "min_pages", 'p', "Minimum number of pages in PDF", &config.min_pages);
    flag_bool(idxcmd, "dryrun", 'r', "Perform dry-run without commiting changes", &config.dryrun);
    flag_string(idxcmd, "filename", 'f', "Index a single file instead of directory traversal",
                &config.filename);

    // Before the matching subcommand runs, we need to connect to the database.
    flag_set_pre_invoke(root, cli_pre_exec_handler);

    // Set the server config as user-data for all handlers.
    FlagStatus status = flag_parse_and_invoke(root, argc, argv, &config);
    if (status != FLAG_OK) {
        LOG_FATAL("Failed to parse command line arguments: %s", flag_status_str(status));
    }

    // If a subcommand was invoked, we are done. Otherwise, start the server.
    if (flag_handler_was_invoked(flag_active_subcommand(root))) {
        exit(EXIT_SUCCESS);
    }

    if (!is_dir(config.frontend_dir)) {
        LOG_FATAL("Frontend build directory does not exist: %s", config.frontend_dir);
    }

    // Register all routes
    route_get("/api/search", pdf_search);
    route_get("/api/list-books", list_books);
    route_get("/api/list-files", list_files);
    route_get("/api/list-files/{file_id}", get_file_by_id);

    /* Page detail endpoint */
    route_get("/api/file/{file_id}/page/{page_num}", get_page_text);
    route_get("/api/file/{file_id}/render-page/{page_num}", render_pdfpage_as_image);
    route_get("/api/file/{file_id}/text-layer/{page_num}", get_pdfpage_text_layer);

    /* Authentication (OAuth) */
    route_get("/api/auth/providers", auth_providers);
    route_get("/api/auth/login/{provider}", auth_login);
    route_get("/api/auth/callback/{provider}", auth_callback);
    route_post("/api/auth/logout", auth_logout);
    route_get("/api/auth/whoami", auth_whoami);

    /* Admin PDF upload (chunked) + indexing jobs */
    route_post("/api/admin/upload/init", upload_init);
    route_post("/api/admin/upload/chunk", upload_chunk);
    route_post("/api/admin/upload/complete", upload_complete);
    route_get("/api/admin/upload/jobs/{job_id}", upload_job_status);
    route_delete("/api/admin/files/{file_id}", delete_file);

    /* CORS preflights for the mutating endpoints above (unmatched paths 404
     * before middleware, so these must exist explicitly). */
    route_options("/api/auth/logout", cors_preflight);
    route_options("/api/admin/upload/init", cors_preflight);
    route_options("/api/admin/upload/chunk", cors_preflight);
    route_options("/api/admin/upload/complete", cors_preflight);
    route_options("/api/admin/files/{file_id}", cors_preflight);

    // Since we are using / for static assets, put at the end to avoid collisions
    printf("Serving SPA at: %s\n", config.frontend_dir);
    route_static("/", config.frontend_dir);
    pulsar_set_callback(pulsar_logger, STDOUT_FILENO);

    // Attach CORS middleware globally.
    Middleware mw[1] = {cors};
    use_global_middleware(mw, ARRAY_SIZE(mw));

    upload_system_init();

    defer { cleanup(); };
    return pulsar_run(config.bind_addr, config.port);
}
