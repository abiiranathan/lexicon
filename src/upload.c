// Chunked admin PDF upload with background indexing.
//
// Pulsar's prebuilt multipart parser caps files at 25 KiB and bodies at
// 10 MiB, so uploads use a tiny raw-body protocol instead:
//   POST /api/admin/upload/init            {filename, size_bytes} -> {job_id}
//   POST /api/admin/upload/chunk?job=&offset=   (raw bytes, sequential)
//   POST /api/admin/upload/complete?job=   (validates, queues indexing)
//   GET  /api/admin/upload/jobs/{job_id}   (poll status)
//
// Indexing runs in a fork+exec child (the index subcommand): process_pdfs()
// calls LOG_FATAL (exit) on single-file errors, which must never take down
// the server.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <pulsar/error.h>
#include <solidc/defer.h>
#include <vfs.h>
#include <yyjson.h>

#include "../include/auth.h"
#include "../include/database.h"
#include "../include/error_macros.h"
#include "../include/json_allocator.h"
#include "../include/json_response.h"
#include "../include/upload.h"
#include "../include/vfs_store.h"

#define DEFAULT_UPLOAD_MAX_MB 200
#define MAX_UPLOAD_MAX_MB 4096
#define DEFAULT_INDEX_TIMEOUT_SECS 1800
#define MIN_INDEX_TIMEOUT_SECS 60
#define MAX_FILENAME_LEN 200
// Single chunks ride as raw bodies; the server caps bodies at 10 MiB, so
// stay under it (the frontend uses 4 MiB slices).
#define MAX_CHUNK_HINT (10u * 1024u * 1024u)

static void upload_enqueue(int64_t job_id);

static long upload_max_bytes(void) {
    const char* raw = getenv("UPLOAD_MAX_MB");
    long mb = DEFAULT_UPLOAD_MAX_MB;
    if (raw && *raw) {
        char* end = NULL;
        const long parsed = strtol(raw, &end, 10);
        if (end != raw && parsed >= 1 && parsed <= MAX_UPLOAD_MAX_MB) mb = parsed;
    }
    return mb * 1024L * 1024L;
}

static long index_timeout_secs(void) {
    const char* raw = getenv("UPLOAD_INDEX_TIMEOUT_S");
    long secs = DEFAULT_INDEX_TIMEOUT_SECS;
    if (raw && *raw) {
        char* end = NULL;
        const long parsed = strtol(raw, &end, 10);
        if (end != raw && parsed >= MIN_INDEX_TIMEOUT_SECS) secs = parsed;
    }
    return secs;
}

// Sanitized original filename written into library_path. Empty on rejection.
static void sanitize_filename(const char* name, char* out, size_t out_cap) {
    size_t o = 0;
    for (const char* p = name; *p && o + 1 < out_cap && o < 150; p++) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) out[o++] = c;
    }
    out[o] = '\0';
    // Must look like name.pdf (case-insensitive) and not be dot-led.
    const size_t len = o;
    if (len < 5 || out[0] == '.' ||
        strcasecmp(out + len - 4, ".pdf") != 0) {
        out[0] = '\0';
    }
}

static bool mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool random_hex(char* out, size_t hex_len) {    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < hex_len; i++) {
        uint8_t byte = 0;
        size_t done = 0;
        while (done < 1) {
            const ssize_t n = getrandom(&byte + done, 1 - done, 0);
            if (n <= 0) return false;
            done += (size_t)n;
        }
        out[i] = digits[(byte >> (i % 2 ? 0 : 4)) & 15];
    }
    out[hex_len] = '\0';
    return true;
}

// Staging area for in-flight uploads. UPLOAD_DIR wins; in VFS mode without
// it, plain host temp is used (only staging lives here — the file itself
// goes into the image).
static const char* staging_base(void) {
    const char* env = getenv("UPLOAD_DIR");
    if (env && *env) return env;
    if (app_vfs_path()) return "./uploads";
    return app_upload_dir();
}

static void send_job_json(PulsarConn* conn, int64_t id, const char* status, const char* filename,
                          int64_t total, int64_t received, int64_t file_id, bool has_file,
                          const char* error, const char* vfs_path) {    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
    if (!doc) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "job_id", id);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_str(doc, root, "filename", filename);
    yyjson_mut_obj_add_int(doc, root, "total_bytes", total);
    yyjson_mut_obj_add_int(doc, root, "received_bytes", received);
    if (has_file) yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    if (error) yyjson_mut_obj_add_str(doc, root, "error", error);
    if (vfs_path && *vfs_path) yyjson_mut_obj_add_str(doc, root, "vfs_path", vfs_path);
    size_t out_len = 0;
    char* json = yyjson_mut_write_opts(doc, 0, &alc, &out_len, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    conn_send_json(conn, StatusOK, json, out_len);
}

void upload_init(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    int64_t user_id = 0;
    if (!auth_require_admin(conn, &user_id)) return;

    Request req = conn_get_request_metadata(conn);
    if (!req.body.data || !req.body.len) {
        send_json_error_status(conn, StatusBadRequest, "missing JSON body");
        return;
    }
    yyjson_doc* doc = yyjson_read(req.body.data, req.body.len, 0);
    if (!doc) {
        send_json_error_status(conn, StatusBadRequest, "invalid JSON body");
        return;
    }
    defer { yyjson_doc_free(doc); };
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* name_val = yyjson_obj_get(root, "filename");
    yyjson_val* size_val = yyjson_obj_get(root, "size_bytes");
    int64_t size = -1;
    if (yyjson_is_uint(size_val)) {
        const uint64_t u = yyjson_get_uint(size_val);
        size = u <= (uint64_t)INT64_MAX ? (int64_t)u : -1;
    } else if (yyjson_is_sint(size_val)) {
        size = (int64_t)yyjson_get_sint(size_val);
    }
    char filename[MAX_FILENAME_LEN + 1] = {0};
    if (yyjson_is_str(name_val)) sanitize_filename(yyjson_get_str(name_val), filename, sizeof(filename));
    if (!filename[0] || size < 1 || size > upload_max_bytes()) {
        send_json_error_status(conn, StatusBadRequest,
                               "filename must end in .pdf (letters, digits, . _ -) and size must be within the configured limit");
        return;
    }

    const char* base = staging_base();
    if (!base || !*base) {
        send_json_error_status(conn, StatusBadRequest,
                               "uploads are not configured: set UPLOAD_DIR or serve with --root (or --vfs)");
        return;
    }
    char staging_dir[PATH_MAX];
    snprintf(staging_dir, sizeof(staging_dir), "%s/.staging", base);
    if (!mkdir_p(staging_dir)) {
        send_json_error_status(conn, StatusInternalServerError,
                               "failed to prepare upload directory");
        return;
    }
    char rand[17];
    if (!random_hex(rand, 16)) {
        send_json_error_status(conn, StatusInternalServerError, "failed to start upload");
        return;
    }
    char staged[PATH_MAX];
    snprintf(staged, sizeof(staged), "%s/%s.part", staging_dir, rand);
    FILE* f = fopen(staged, "wb");
    if (!f) {
        send_json_error_status(conn, StatusInternalServerError, "failed to create staging file");
        return;
    }
    fclose(f);

    acquire_db(db);
    static const char* insert_job =
        "INSERT INTO upload_jobs(filename, staged_path, total_bytes, status, created_by) "
        "VALUES ($1, $2, $3, 'uploading', $4) RETURNING id";
    char size_str[32], user_str[32];
    snprintf(size_str, sizeof(size_str), "%lld", (long long)size);
    snprintf(user_str, sizeof(user_str), "%lld", (long long)user_id);
    const char* params[] = {filename, staged, size_str, user_str};
    run_query(res, db, insert_job, 4, params);
    if (PQntuples(res) != 1) {
        unlink(staged);
        send_json_error_status(conn, StatusInternalServerError, "failed to start upload");
        return;
    }
    send_job_json(conn, atoll(PQgetvalue(res, 0, 0)), "uploading", filename, size, 0, 0, false,
                  NULL, NULL);
}

void upload_complete(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    if (!auth_require_admin(conn, NULL)) return;

    const char* job_str = query_get(conn, "job");
    char* end = NULL;
    const int64_t job_id = job_str ? strtoll(job_str, &end, 10) : -1;
    if (!job_str || !*job_str || (end && *end) || job_id < 1) {
        send_json_error_status(conn, StatusBadRequest, "invalid job id");
        return;
    }

    acquire_db(db);
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)job_id);
    static const char* load =
        "SELECT filename, staged_path, total_bytes, received_bytes, status "
        "FROM upload_jobs WHERE id = $1";
    const char* load_params[] = {id_str};
    run_query(res, db, load, 1, load_params);
    if (PQntuples(res) != 1) {
        send_json_error_status(conn, StatusNotFound, "unknown upload job");
        return;
    }
    char filename[MAX_FILENAME_LEN + 1];
    char staged[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s", PQgetvalue(res, 0, 0));
    snprintf(staged, sizeof(staged), "%s", PQgetvalue(res, 0, 1));
    const int64_t total = atoll(PQgetvalue(res, 0, 2));
    const int64_t received = atoll(PQgetvalue(res, 0, 3));
    const char* status = PQgetvalue(res, 0, 4);
    if (strcmp(status, "uploading") != 0 || received != total || total < 1) {
        send_json_error_status(conn, StatusConflict, "upload is not complete yet");
        return;
    }

    FILE* f = fopen(staged, "rb");
    if (!f) {
        send_json_error_status(conn, StatusInternalServerError, "staged upload is missing");
        return;
    }
    char magic[5] = {0};
    const size_t magic_len = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (magic_len != sizeof(magic) || memcmp(magic, "%PDF-", 5) != 0) {
        static const char* fail =
            "UPDATE upload_jobs SET status = 'failed', error = 'not a PDF file', "
            "updated_at = now() WHERE id = $1";
        run_query(fail_res, db, fail, 1, load_params);
        (void)fail_res;
        unlink(staged);
        send_json_error_status(conn, StatusUnprocessableEntity, "uploaded file is not a PDF");
        return;
    }

    char safe[MAX_FILENAME_LEN + 1];
    sanitize_filename(filename, safe, sizeof(safe));
    if (!safe[0]) {
        send_json_error_status(conn, StatusBadRequest, "invalid filename");
        return;
    }
    char rand[13];
    if (!random_hex(rand, 12)) {
        send_json_error_status(conn, StatusInternalServerError, "failed to finalize upload");
        return;
    }
    char unique[256];
    if (snprintf(unique, sizeof(unique), "%lld-%s-%s", (long long)time(NULL), rand, safe) >=
        (int)sizeof(unique)) {
        send_json_error_status(conn, StatusInternalServerError, "upload path too long");
        return;
    }

    // VFS mode: the destination lives inside the image at the root level
    // (no parent directories to create); staging stays until the worker
    // imports it. Host mode: move staging to its final library path.
    const char* image = app_vfs_path();
    char final_path[PATH_MAX] = {0};
    char vfs_path[PATH_MAX] = {0};
    if (image && *image) {
        if (snprintf(vfs_path, sizeof(vfs_path), "/%s", unique) >= (int)sizeof(vfs_path)) {
            send_json_error_status(conn, StatusInternalServerError, "upload path too long");
            return;
        }
    } else {
        const char* base = app_upload_dir();
        if (!base || !*base) {
            send_json_error_status(conn, StatusBadRequest,
                                   "uploads are not configured: set UPLOAD_DIR or serve with --root");
            return;
        }
        if (snprintf(final_path, sizeof(final_path), "%s/%s", base, unique) >=
            (int)sizeof(final_path)) {
            send_json_error_status(conn, StatusInternalServerError, "upload path too long");
            return;
        }
        if (rename(staged, final_path) != 0) {
            send_json_error_status(conn, StatusInternalServerError, "failed to finalize upload");
            return;
        }
    }

    static const char* queue =
        "UPDATE upload_jobs SET status = 'queued', final_path = $2, vfs_path = $3, "
        "updated_at = now() "
        "WHERE id = $1 AND status = 'uploading'";
    const char* queue_params[] = {id_str, final_path, vfs_path};
    run_query(queue_res, db, queue, 3, queue_params);
    if (strcmp(PQcmdTuples(queue_res), "1") != 0) {
        if (vfs_path[0]) {
            unlink(staged);
        } else {
            unlink(final_path);
        }
        send_json_error_status(conn, StatusConflict, "upload job changed state concurrently");
        return;
    }
    (void)queue_res;

    upload_enqueue(job_id);

    static const char* fetch =
        "SELECT filename, total_bytes, received_bytes, status, vfs_path FROM upload_jobs WHERE id = $1";
    run_query(cur, db, fetch, 1, load_params);
    send_job_json(conn, job_id, PQgetvalue(cur, 0, 3), PQgetvalue(cur, 0, 0),
                  atoll(PQgetvalue(cur, 0, 1)), atoll(PQgetvalue(cur, 0, 2)), 0, false, NULL,
                  PQgetvalue(cur, 0, 4));
}

void upload_job_status(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    if (!auth_require_admin(conn, NULL)) return;

    require_path_param(job_id_str, conn, "job_id");
    char* end = NULL;
    const int64_t job_id = strtoll(job_id_str, &end, 10);
    if ((end && *end) || job_id < 1) {
        send_json_error_status(conn, StatusBadRequest, "invalid job id");
        return;
    }

    acquire_db(db);
    static const char* query =
        "SELECT filename, total_bytes, received_bytes, status, file_id, error, vfs_path "
        "FROM upload_jobs WHERE id = $1";
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)job_id);
    const char* params[] = {id_str};
    run_query(res, db, query, 1, params);
    if (PQntuples(res) != 1) {
        send_json_error_status(conn, StatusNotFound, "unknown upload job");
        return;
    }
    const char* file_id_str = PQgetvalue(res, 0, 4);
    send_job_json(conn, job_id, PQgetvalue(res, 0, 3), PQgetvalue(res, 0, 0),
                  atoll(PQgetvalue(res, 0, 1)), atoll(PQgetvalue(res, 0, 2)),
                  file_id_str && *file_id_str ? atoll(file_id_str) : 0,
                  file_id_str && *file_id_str, PQgetvalue(res, 0, 5),
                  PQgetvalue(res, 0, 6));
}

/* ------------------------------------------------------------------ */
/* Background indexing worker (fork+exec, never in-process: the indexer */
/* calls LOG_FATAL (exit) on single-file errors)                       */
/* ------------------------------------------------------------------ */

typedef struct job_node {
    int64_t id;
    struct job_node* next;
} job_node_t;

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;
static job_node_t* queue_head = NULL;
static job_node_t* queue_tail = NULL;
static bool worker_stop = false;
static bool worker_started = false;
static pthread_t worker_thread;

static void worker_push(int64_t id) {
    job_node_t* node = malloc(sizeof(*node));
    if (!node) return;
    node->id = id;
    node->next = NULL;
    pthread_mutex_lock(&worker_lock);
    if (queue_tail) {
        queue_tail->next = node;
    } else {
        queue_head = node;
    }
    queue_tail = node;
    pthread_cond_signal(&worker_cond);
    pthread_mutex_unlock(&worker_lock);
}

static bool worker_pop(int64_t* out_id) {
    pthread_mutex_lock(&worker_lock);
    while (!queue_head && !worker_stop) pthread_cond_wait(&worker_cond, &worker_lock);
    job_node_t* node = queue_head;
    if (node) {
        queue_head = node->next;
        if (!queue_head) queue_tail = NULL;
    }
    pthread_mutex_unlock(&worker_lock);
    if (!node) return false;
    *out_id = node->id;
    free(node);
    return true;
}

static void upload_enqueue(int64_t job_id) {
    worker_push(job_id);
}

// Runs the index subcommand in a child process so indexer LOG_FATAL exits
// can only fail the job, never the server. Returns true with file_id set.
// Runs the index subcommand in a forked child (LOG_FATAL-safe). When
// vfs_image is non-NULL the child indexes inside the image (--vfs) instead
// of the host filesystem.
static bool index_in_child(const char* dir, const char* path, const char* vfs_image,
                           char* err_out, size_t err_cap, int64_t* out_file_id) {
    *out_file_id = 0;
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "/tmp/lexicon-index-%d.log", (int)getpid());

    char self[PATH_MAX];
    const ssize_t self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_len <= 0) {
        snprintf(err_out, err_cap, "cannot locate server executable");
        return false;
    }
    self[self_len] = '\0';

    char min_pages_str[16];
    snprintf(min_pages_str, sizeof(min_pages_str), "%d", app_min_pages());

    const pid_t child = fork();
    if (child < 0) {
        snprintf(err_out, err_cap, "failed to spawn indexer");
        return false;
    }
    // The child must get --pgconn explicitly: process env PGCONN may have
    // been overridden by .env loading, while config carries the effective
    // connection string.
    const char* pgconn = app_pgconn();
    if (!pgconn) {
        snprintf(err_out, err_cap, "server has no database connection string");
        return false;
    }
    if (child == 0) {
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDERR_FILENO);
            if (log_fd != STDERR_FILENO) close(log_fd);
        }
        if (vfs_image) {
            execl(self, self, "index", "--pgconn", pgconn, "--vfs", vfs_image, "--filename", path,
                  "--min_pages", min_pages_str, (char*)NULL);
        } else {
            execl(self, self, "index", "--pgconn", pgconn, "--root", dir, "--filename", path,
                  "--min_pages", min_pages_str, (char*)NULL);
        }
        _exit(127);
    }

    const long timeout = index_timeout_secs();
    long waited_ms = 0;
    bool timed_out = false;
    int status = 0;
    pid_t done = 0;
    while ((done = waitpid(child, &status, WNOHANG)) == 0) {
        if (waited_ms >= timeout * 1000L) {
            timed_out = true;
            kill(child, SIGKILL);
            continue;
        }
        struct timespec quarter = {.tv_sec = 0, .tv_nsec = 250 * 1000 * 1000};
        nanosleep(&quarter, NULL);
        waited_ms += 250;
    }
    char tail[601] = {0};
    FILE* log = fopen(log_path, "rb");
    if (log) {
        fseek(log, 0, SEEK_END);
        const long size = ftell(log);
        const long start = size > 600 ? size - 600 : 0;
        fseek(log, start, SEEK_SET);
        const size_t n = fread(tail, 1, sizeof(tail) - 1, log);
        tail[n] = '\0';
        fclose(log);
    }
    unlink(log_path);

    if (done < 0 || timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (timed_out) {
            snprintf(err_out, err_cap, "indexing timed out after %ld seconds", timeout);
        } else if (done >= 0 && WIFSIGNALED(status)) {
            snprintf(err_out, err_cap, "indexer killed by signal %d", WTERMSIG(status));
        } else {
            const int code = done >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            snprintf(err_out, err_cap, "indexing failed (exit %d)%s%s", code, tail[0] ? ": " : "",
                     tail[0] ? tail : "");
        }
        return false;
    }

    pgconn_t* db = pgpool_acquire(pool, 10000);
    if (!db) {
        snprintf(err_out, err_cap, "no database connection available");
        return false;
    }
    defer { pgpool_release(pool, db); };
    static const char* find_file = "SELECT id FROM files WHERE path = $1 LIMIT 1";
    const char* params[] = {path};
    PGresult* res = pgpool_query_params(db, find_file, 1, params, -1);
    if (!res) {
        snprintf(err_out, err_cap, "failed to look up indexed file");
        return false;
    }
    defer { PQclear(res); };
    if (PQntuples(res) != 1) {
        snprintf(err_out, err_cap,
                 "indexing produced no file record (below the minimum page count? see server log)");
        return false;
    }
    *out_file_id = atoll(PQgetvalue(res, 0, 0));
    return true;
}

// Imports the staged file into the VFS image at a root-level destination
// (no parent directories to create), using kernel-side bulk transfer.
static bool vfs_import_staged(const char* image, const char* staged, const char* vfs_path,
                              size_t size, char* err_out, size_t err_cap) {
    vfs_t* writer = NULL;
    if (vfs_open(image, false, &writer) != VFS_OK || !writer) {
        snprintf(err_out, err_cap, "cannot open VFS image for writing");
        return false;
    }
    const int host_fd = open(staged, O_RDONLY);
    if (host_fd < 0) {
        snprintf(err_out, err_cap, "staged upload is missing");
        vfs_close(writer);
        return false;
    }
    bool ok = false;
    const vfs_fd_t fd = vfs_fopen(writer, vfs_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_EXCL);
    if (fd < 0) {
        snprintf(err_out, err_cap, "VFS import failed: %s", vfs_strerror((vfs_status_t)fd));
    } else {
        const vfs_status_t st = vfs_import_fd(writer, fd, host_fd, (uint64_t)size);
        vfs_fclose(writer, fd);
        if (st != VFS_OK) {
            snprintf(err_out, err_cap, "VFS import failed: %s", vfs_strerror(st));
        } else {
            ok = true;
        }
    }
    close(host_fd);
    vfs_close(writer); // flushes batched metadata
    return ok;
}

// Best-effort removal of a path imported by a failed job.
static void vfs_remove_path(const char* image, const char* vfs_path) {
    vfs_t* writer = NULL;
    if (vfs_open(image, false, &writer) != VFS_OK || !writer) return;
    vfs_unlink(writer, vfs_path);
    vfs_close(writer);
}

static void run_upload_job(int64_t job_id) {
    pgconn_t* db = pgpool_acquire(pool, 30000);
    if (!db) return;
    defer { pgpool_release(pool, db); };

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)job_id);
    static const char* claim =
        "UPDATE upload_jobs SET status = 'running', updated_at = now() "
        "WHERE id = $1 AND status = 'queued'";
    const char* claim_params[] = {id_str};
    PGresult* claim_res = pgpool_query_params(db, claim, 1, claim_params, -1);
    if (!claim_res) return;
    const bool claimed = strcmp(PQcmdTuples(claim_res), "1") == 0;
    PQclear(claim_res);
    if (!claimed) return;

    static const char* load =
        "SELECT final_path, vfs_path, staged_path, total_bytes FROM upload_jobs WHERE id = $1";
    PGresult* res = pgpool_query_params(db, load, 1, claim_params, -1);
    if (!res || PQntuples(res) != 1) {
        if (res) PQclear(res);
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", PQgetvalue(res, 0, 0));
    char job_vfs[PATH_MAX];
    snprintf(job_vfs, sizeof(job_vfs), "%s", PQgetvalue(res, 0, 1));
    char staged[PATH_MAX];
    snprintf(staged, sizeof(staged), "%s", PQgetvalue(res, 0, 2));
    const int64_t total = atoll(PQgetvalue(res, 0, 3));
    PQclear(res);

    char err[640] = {0};
    int64_t file_id = 0;
    bool ok = false;
    if (job_vfs[0]) {
        // VFS mode: import staging into the image, index it with a child
        // holding its own fresh handle, then swap the server handle so
        // subsequent reads observe the new file.
        const char* image = app_vfs_path();
        if (!image || !*image) {
            snprintf(err, sizeof(err), "VFS image is no longer configured");
        } else if (!vfs_import_staged(image, staged, job_vfs, (size_t)total, err, sizeof(err))) {
            // err set by the importer.
        } else if (!index_in_child(".", job_vfs, image, err, sizeof(err), &file_id)) {
            vfs_remove_path(image, job_vfs);
        } else {
            vfs_t* fresh = NULL;
            if (vfs_open(image, true, &fresh) != VFS_OK || !vfs_store_replace(fresh)) {
                if (fresh) vfs_close(fresh);
                snprintf(err, sizeof(err), "indexed but the text layer cannot see the new file");
                vfs_remove_path(image, job_vfs);
            } else {
                ok = true;
            }
        }
        unlink(staged);
    } else {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", path);
        char* slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            snprintf(dir, sizeof(dir), ".");
        }
        ok = index_in_child(dir, path, NULL, err, sizeof(err), &file_id);
        if (!ok) unlink(path);
    }

    if (ok) {
        static const char* done =
            "UPDATE upload_jobs SET status = 'done', file_id = $2, error = NULL, "
            "updated_at = now() WHERE id = $1";
        char fid_str[32];
        snprintf(fid_str, sizeof(fid_str), "%lld", (long long)file_id);
        const char* done_params[] = {id_str, fid_str};
        PGresult* done_res = pgpool_query_params(db, done, 2, done_params, -1);
        if (done_res) PQclear(done_res);
    } else {
        static const char* failed =
            "UPDATE upload_jobs SET status = 'failed', error = $2, updated_at = now() "
            "WHERE id = $1";
        const char* failed_params[] = {id_str, err[0] ? err : "indexing failed"};
        PGresult* failed_res = pgpool_query_params(db, failed, 2, failed_params, -1);
        if (failed_res) PQclear(failed_res);
    }
}

static void* upload_worker_main(void* arg) {
    (void)arg;
    int64_t id = 0;
    while (worker_pop(&id)) run_upload_job(id);
    return NULL;
}

void upload_system_init(void) {
    if (worker_started) return;
    worker_started = true;

    // Recover from a previous run: drop dead in-progress uploads, requeue.
    pgconn_t* db = pgpool_acquire(pool, 10000);
    if (db) {
        static const char* recover =
            "SELECT id, staged_path FROM upload_jobs WHERE status = 'uploading'";
        PGresult* res = pgpool_query_params(db, recover, 0, NULL, -1);
        if (res) {
            for (int i = 0; i < PQntuples(res); i++) unlink(PQgetvalue(res, i, 1));
            PQclear(res);
        }
        PGresult* abort_res = pgpool_query_params(
            db, "UPDATE upload_jobs SET status = 'aborted', updated_at = now() WHERE status = 'uploading'", 0,
            NULL, -1);
        if (abort_res) PQclear(abort_res);
        PGresult* queued = pgpool_query_params(
            db, "SELECT id FROM upload_jobs WHERE status = 'queued' ORDER BY id", 0, NULL, -1);
        if (queued) {
            for (int i = 0; i < PQntuples(queued); i++)
                worker_push(atoll(PQgetvalue(queued, i, 0)));
            PQclear(queued);
        }
        pgpool_release(pool, db);
    }

    if (pthread_create(&worker_thread, NULL, upload_worker_main, NULL) != 0) {
        fprintf(stderr, "upload: failed to start indexing worker thread\n");
        worker_started = false;
    }
}

void upload_system_shutdown(void) {
    if (!worker_started) return;
    pthread_mutex_lock(&worker_lock);
    worker_stop = true;
    pthread_cond_broadcast(&worker_cond);
    pthread_mutex_unlock(&worker_lock);
    pthread_join(worker_thread, NULL);
    worker_started = false;
}

void upload_chunk(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    if (!auth_require_admin(conn, NULL)) return;

    const char* job_str = query_get(conn, "job");
    const char* off_str = query_get(conn, "offset");
    char* end = NULL;
    const int64_t job_id = job_str ? strtoll(job_str, &end, 10) : -1;
    if (!job_str || !*job_str || (end && *end) || job_id < 1) {
        send_json_error_status(conn, StatusBadRequest, "invalid job id");
        return;
    }
    end = NULL;
    const int64_t offset = off_str ? strtoll(off_str, &end, 10) : -1;
    if (!off_str || !*off_str || (end && *end) || offset < 0) {
        send_json_error_status(conn, StatusBadRequest, "invalid offset");
        return;
    }

    Request req = conn_get_request_metadata(conn);
    const size_t chunk_len = req.body.data ? req.body.len : 0;
    if (chunk_len == 0 || chunk_len > MAX_CHUNK_HINT) {
        send_json_error_status(conn, StatusBadRequest, "empty or oversized chunk");
        return;
    }

    acquire_db(db);
    static const char* load =
        "SELECT staged_path, total_bytes, received_bytes, status FROM upload_jobs WHERE id = $1";
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)job_id);
    const char* load_params[] = {id_str};
    run_query(res, db, load, 1, load_params);
    if (PQntuples(res) != 1) {
        send_json_error_status(conn, StatusNotFound, "unknown upload job");
        return;
    }
    char staged[PATH_MAX];
    snprintf(staged, sizeof(staged), "%s", PQgetvalue(res, 0, 0));
    const int64_t total = atoll(PQgetvalue(res, 0, 1));
    const int64_t received = atoll(PQgetvalue(res, 0, 2));
    const char* status = PQgetvalue(res, 0, 3);
    if (strcmp(status, "uploading") != 0 || received != offset ||
        received + (int64_t)chunk_len > total) {
        send_json_error_status(conn, StatusConflict,
                               "stale job state: refresh the upload and resume at received_bytes");
        return;
    }

    FILE* f = fopen(staged, "ab");
    if (!f) {
        send_json_error_status(conn, StatusInternalServerError, "failed to append chunk");
        return;
    }
    const size_t written = fwrite(req.body.data, 1, chunk_len, f);
    fclose(f);
    if (written != chunk_len) {
        send_json_error_status(conn, StatusInternalServerError, "failed to append chunk");
        return;
    }

    static const char* advance =
        "UPDATE upload_jobs SET received_bytes = received_bytes + $2, updated_at = now() "
        "WHERE id = $1 AND status = 'uploading' AND received_bytes = $3";
    char len_str[32], off_str2[32];
    snprintf(len_str, sizeof(len_str), "%zu", chunk_len);
    snprintf(off_str2, sizeof(off_str2), "%lld", (long long)offset);
    const char* advance_params[] = {id_str, len_str, off_str2};
    run_query(advance_res, db, advance, 3, advance_params);
    if (strcmp(PQcmdTuples(advance_res), "1") != 0) {
        send_json_error_status(conn, StatusConflict,
                               "stale job state: refresh the upload and resume at received_bytes");
        return;
    }
    (void)advance_res;

    static const char* fetch =
        "SELECT filename, total_bytes, received_bytes, status, vfs_path FROM upload_jobs WHERE id = $1";
    run_query(cur, db, fetch, 1, load_params);
    send_job_json(conn, job_id, PQgetvalue(cur, 0, 3), PQgetvalue(cur, 0, 0),
                  atoll(PQgetvalue(cur, 0, 1)), atoll(PQgetvalue(cur, 0, 2)), 0, false, NULL,
                  PQgetvalue(cur, 0, 4));
}
