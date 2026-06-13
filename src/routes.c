#include <pgpool/pgtypes.h>
#include <pulsar/error.h>
#include <solidc/cache.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/str_to_num.h>
#include <vfs.h>

#include "../include/database.h"
#include "../include/json_response.h"
#include "../include/pdf.h"
#include "../include/routes.h"

// Instance of VFS from main. (must not be NULL)
extern vfs_t* vfs;

/** Maximum length for any cache key string, including the NUL terminator. */
#define CACHE_KEY_MAX_LEN 128

/**
 * Acquire a database connection from the pool and jump to @p label if the
 * pool is exhausted.  The caller is responsible for releasing the connection
 * via pgpool_release() at the @p label site.
 *
 * @param var   Name of the pgconn_t* variable to declare.
 * @param label goto label to jump to on acquisition failure.
 */
#define ACQUIRE_DB(var)                                \
    pgconn_t* var = pgpool_acquire(pool, 1000);        \
    if (!(var)) {                                      \
        send_json_error(conn, "Database unavailable"); \
        return;                                        \
    }                                                  \
    defer {                                            \
        pgpool_release(pool, (var));                   \
    };

/**
 * Look up a value in the response cache and, on a hit, send it as JSON and
 * return from the enclosing function.
 *
 * @param cache_   The cache_t* instance.
 * @param key_     NUL-terminated cache key string.
 * @param key_len_ Length of the key in bytes (excluding NUL).
 * @param conn_    PulsarConn* to write the response to.
 */
#define CACHE_HIT_RETURN(cache_, key_, key_len_, conn_)                              \
    do {                                                                             \
        size_t _cached_len = 0;                                                      \
        const char* _cached = cache_get((cache_), (key_), (key_len_), &_cached_len); \
        if (_cached) {                                                               \
            send_cache_json((conn_), _cached, _cached_len);                          \
            cache_release(_cached);                                                  \
            return;                                                                  \
        }                                                                            \
    } while (0)

/**
 * Store JSON in the response cache and send it to the client.
 * Frees @p json_str_ after the send.
 *
 * @param cache_   The cache_t* instance.
 * @param key_     NUL-terminated cache key.
 * @param key_len_ Length of the key in bytes.
 * @param conn_    PulsarConn* to write the response to.
 * @param json_str_ Arena-backed JSON string.
 * @param json_len_ Length of the JSON payload in bytes.
 * @param ttl_     TTL in seconds (0 = use cache default).
 */
#define CACHE_SET_AND_SEND(cache_, key_, key_len_, conn_, json_str_, json_len_, ttl_)  \
    do {                                                                               \
        uint8_t* _cache_copy = (uint8_t*)malloc(json_len_);                            \
        if (_cache_copy) {                                                             \
            memcpy(_cache_copy, (json_str_), (json_len_));                             \
            cache_set((cache_), (key_), (key_len_), _cache_copy, (json_len_), (ttl_)); \
        }                                                                              \
        conn_send_json((conn_), StatusOK, (json_str_), (json_len_));                   \
    } while (0)

#define HANDLE_RECORD_NOT_FOUND(res, conn, errmsg) \
    if (PQntuples(res) == 0) {                     \
        conn_set_status(conn, StatusNotFound);     \
        send_json_error(conn, errmsg);             \
        return;                                    \
    }

#define HANDLE_QUERY_ERROR(res, conn)                    \
    if (!(res)) {                                        \
        send_json_error(conn, pgpool_error_message(db)); \
        return;                                          \
    }

/* ---------------------------------------------------------------------------
 * Module-level state
 * ---------------------------------------------------------------------------
 */

/** Global response cache shared across all route handlers. */
static cache_t* g_res_cache = NULL;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/**
 * Initialises the global response cache.
 *
 * @param capacity    Maximum number of entries the cache may hold.
 * @param ttl_seconds Default time-to-live for cache entries, in seconds.
 * @return true on success, false if allocation failed.
 */
bool init_response_cache(size_t capacity, uint32_t ttl_seconds) {
    g_res_cache = cache_create(capacity, ttl_seconds);
    return g_res_cache != NULL;
}

/**
 * Destroys the global response cache and sets the pointer to NULL.
 * Safe to call even if init_response_cache() was never called.
 */
void destroy_response_cache(void) {
    if (g_res_cache) cache_destroy(g_res_cache);
    g_res_cache = NULL;
}

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------------
 */

// Opens a document from the VFS if one exists otherwise uses default filesystem.
PopplerDocument* gen_open_document(const char* path, int* num_pages) {
    if (vfs != NULL) { return open_vfs_document(vfs, path, num_pages); }
    return open_document(path, num_pages);
}

/**
 * Sends a JSON-encoded error body with HTTP 400 Bad Request.
 *
 * @param conn PulsarConn to write the response to.
 * @param msg  Human-readable error message; must be NUL-terminated.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    size_t outlen;
    char* err_str = json_create_error(&alc, msg, &outlen);
    if (err_str) {
        conn_send_json(conn, StatusBadRequest, err_str, outlen);
    } else {
        conn_send_json(conn, StatusInternalServerError, "{\"error\":\"Internal error\"}", 26);
    }
}

/**
 * Sends pre-serialised, non-NUL-terminated JSON data with HTTP 200 OK.
 * The caller is responsible for the lifetime of @p data.
 *
 * @param conn PulsarConn to write the response to.
 * @param data Pointer to the JSON payload bytes.
 * @param len  Length of the payload in bytes.
 */
static inline void send_cache_json(PulsarConn* conn, const char* data, size_t len) {
    conn_set_content_type(conn, SS_LIT("application/json"));
    conn_send(conn, StatusOK, data, len);
}

/**
 * Formats a cache key for a file or a specific page within a file.
 *
 * If @p page_num is negative the key covers the whole file; otherwise it
 * covers the individual page.
 *
 * @param buffer      Output buffer to write the NUL-terminated key into.
 * @param buffer_size Capacity of @p buffer in bytes.
 * @param file_id     Database file identifier.
 * @param page_num    Page number (1-based), or a negative value for file-level
 * keys.
 * @return Number of bytes written, excluding the NUL terminator (à la
 * snprintf).
 */
static inline int cache_make_key(char* buffer, size_t buffer_size, int64_t file_id, int page_num) {
    if (page_num >= 0) { return snprintf(buffer, buffer_size, "file:%lld:page:%d", (long long)file_id, page_num); }
    return snprintf(buffer, buffer_size, "file:%lld", (long long)file_id);
}

/* ---------------------------------------------------------------------------
 * Route handlers
 * ---------------------------------------------------------------------------
 */

/**
 * GET /files/:file_id/pages/:page_num
 *
 * Returns the extracted text of a single PDF page as JSON.  Results are
 * cached; subsequent identical requests are served from the cache without
 * hitting the database.
 *
 * Path parameters:
 *   file_id  - Integer database identifier of the file.
 *   page_num - 1-based page number.
 */
void get_page_by_file_and_page(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    const char* file_id_str = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

    int64_t file_id = 0;
    int64_t page_num = 0;
    str_to_i64(file_id_str, &file_id);
    str_to_i64(page_num_str, &page_num);

    char cache_key[CACHE_KEY_MAX_LEN] = {0};
    int key_len = cache_make_key(cache_key, sizeof(cache_key), file_id, (int)page_num);
    CACHE_HIT_RETURN(g_res_cache, cache_key, (size_t)key_len, conn);

    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[] = {file_id_str, page_num_str};

    ACQUIRE_DB(db);
    PGresult* res = pgpool_query_params(db, query, 2, params, -1);
    defer_pqclear(res);
    HANDLE_QUERY_ERROR(res, conn);
    HANDLE_RECORD_NOT_FOUND(res, conn, "No page found for the requested file and page number");

    const char* text = PQgetvalue(res, 0, 0);
    StrSlice response = json_create_page_response(&alc, file_id, (int)page_num, text);
    if (!ss_is_valid(response)) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    uint8_t* cache_copy = malloc(response.len);
    if (cache_copy) {
        memcpy(cache_copy, response.data, response.len);
        cache_set(g_res_cache, cache_key, (size_t)key_len, cache_copy, response.len, 60 * 60);
    }
    conn_send_json(conn, StatusOK, response.data, response.len);
}

/** Function pointer type for a PDF page rendering backend. */
typedef bool (*PageRenderFunc)(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer);

/**
 * GET /files/:file_id/pages/:page_num/render
 *
 * Renders a single PDF page as either a JPEG image.
 * The rendered bytes are cached for one hour.
 *
 * Path parameters:
 *   file_id  - Integer database identifier of the file.
 *   page_num - 1-based page number.
 *
 */
void render_pdfpage_as_image(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param(file_id_str, conn, "file_id");
    require_path_param(page_num_str, conn, "page_num");

    int64_t file_id = 0;
    int page = 0;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_int(page_num_str, &page) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    static const char headers[] = "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
    size_t length = sizeof(headers) - 1;
    conn_writeheader_raw(conn, headers, length);

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    ACQUIRE_DB(db);
    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    defer_pqclear(res);

    HANDLE_QUERY_ERROR(res, conn);
    HANDLE_RECORD_NOT_FOUND(res, conn, "No file found for the requested file");

    const char* path = PQgetvalue(res, 0, 0);

    // Generic open.
    int num_pages;
    PopplerDocument* doc = gen_open_document(path, &num_pages);
    if (!doc) {
        send_json_error(conn, "Failed to open document");
        return;
    }

    if (page < 1 || page > num_pages) {
        send_json_error(conn, "Page out of range");
        return;
    }

    PopplerPage* ppage = poppler_document_get_page(doc, page - 1);
    pdf_buffer_t buf = {0};
    double width, height;
    poppler_page_get_size(ppage, &width, &height);
    render_page_to_png_buffer(ppage, (int)width, (int)height, &buf);

    conn_write(conn, buf.data, buf.size);
}

/**
 * GET /search?q=<query>[&file_id=<id>]
 *
 * Full-text search across all pages, optionally scoped to a single file.
 * Results are ranked by relevance using PostgreSQL ts_rank_cd, with a bonus
 * for phrase matches.  The JSON response is cached keyed on (query, file_id).
 *
 * Query parameters:
 *   q       - Search query string (required).
 *   file_id - Restrict results to this file ID (optional).
 */
void pdf_search(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    const char* file_id = query_get(conn, "file_id");
    const char* query_str = query_get(conn, "q");

    /* Build the cache key from the NUL-terminated copies. */
    char cache_key[CACHE_KEY_MAX_LEN] = {0};
    int key_len = snprintf(cache_key, sizeof(cache_key), "search:%s:%s", query_str, file_id ? file_id : "all");
    if (key_len < 0 || key_len >= (int)sizeof(cache_key)) { abort_400(conn, "query too long"); }

    CACHE_HIT_RETURN(g_res_cache, cache_key, (size_t)key_len, conn);

    /* Build the parameterised SQL query. */
    const char* params[2];
    int param_count = 1;
    params[0] = query_str;

    if (file_id) {
        params[1] = file_id;
        param_count = 2;
    }

    /*
   * Common table expression shared by both query variants.  The caller
   * appends either a file-scoped or global suffix depending on whether
   * file_id was supplied.
   */
    static const char common_cte
        [] = "WITH input_queries AS ("
             "  SELECT"
             "    websearch_to_tsquery('english', $1) AS broad_query,"
             "    phraseto_tsquery('english', $1)     AS phrase_query"
             "),"
             "RankedPages AS ("
             "  SELECT"
             "    p.file_id, p.page_num,"
             "    ("
             "      ts_rank_cd(p.text_vector, inputs.broad_query)"
             "      + (CASE WHEN p.text_vector @@ inputs.phrase_query THEN 10.0 ELSE "
             "0.0 END)"
             "    ) AS rank"
             "  FROM pages p"
             "  CROSS JOIN input_queries inputs"
             "  WHERE p.text_vector @@ inputs.broad_query";

    static const char query_suffix
        [] = "  ORDER BY rank DESC LIMIT 100"
             "),"
             "UniquePages AS ("
             "  SELECT DISTINCT ON (file_id, page_num) file_id, page_num, rank"
             "  FROM RankedPages ORDER BY file_id, page_num, rank DESC"
             ")"
             "SELECT"
             "  u.file_id, f.name, f.num_pages, u.page_num,"
             "  ts_headline('english', p.text, inputs.broad_query,"
             "    'StartSel=<b>, StopSel=</b>, MaxWords=200, MinWords=20') AS snippet,"
             "  LEFT(p.text, 2000) AS extended_snippet,"
             "  u.rank"
             " FROM UniquePages u"
             " CROSS JOIN input_queries inputs"
             " JOIN files f ON u.file_id = f.id"
             " JOIN pages p ON u.file_id = p.file_id AND u.page_num = p.page_num"
             " ORDER BY u.rank DESC, f.name, u.page_num LIMIT 100";

    char full_query[4096];
    if (file_id) {
        snprintf(full_query, sizeof(full_query), "%s AND p.file_id = $2 %s", common_cte, query_suffix);
    } else {
        snprintf(full_query, sizeof(full_query), "%s %s", common_cte, query_suffix);
    }

    ACQUIRE_DB(db);

    PGresult* res = NULL;
    TIME_BLOCK("pdfsearch", { res = pgpool_query_params(db, full_query, param_count, params, -1); });
    defer_pqclear(res);

    HANDLE_QUERY_ERROR(res, conn);

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    size_t jsonlen = 0;
    char* json_str = json_create_search_results(&alc, res, query_str, &jsonlen);
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}

/**
 * GET /files?page=<n>&limit=<n>[&name=<filter>]
 *
 * Returns a paginated JSON list of all files, optionally filtered by name
 * (case-insensitive ILIKE match).  The response is cached per unique
 * combination of page, limit, and name filter.
 *
 * Query parameters:
 *   page  - 1-based page index (default: 1).
 *   limit - Number of results per page, clamped to [1, 100] (default: 10).
 *   name  - Optional substring filter applied case-insensitively.
 */
void list_files(PulsarCtx* ctx) {
    ASSERT(g_res_cache);

    PulsarConn* conn = ctx->conn;

    const char* page_str = query_get(conn, "page");
    const char* page_size_str = query_get(conn, "limit");
    const char* name = query_get(conn, "name");

    int page = 1, page_size = 10;
    if (page_str) str_to_int(page_str, &page);
    if (page_size_str) str_to_int(page_size_str, &page_size);

    if (page < 1) page = 1;
    if (page_size < 1) page_size = 25;
    if (page_size > 100) page_size = 100;

    /* Build cache key from pagination params and optional name filter. */
    char cache_key[CACHE_KEY_MAX_LEN] = {0};
    size_t key_len;
    if (!name) {
        key_len = (size_t)snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d:n%s", page, page_size, name);
    } else {
        key_len = (size_t)snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d", page, page_size);
    }

    CACHE_HIT_RETURN(g_res_cache, cache_key, key_len, conn);

    ACQUIRE_DB(db);

    /* Fetch the total row count for pagination metadata. */
    static const char* count_query = "SELECT COUNT(*) FROM files";
    PGresult* count_res = pgpool_query(db, count_query, 5000);
    if (!count_res) {
        send_json_error(conn, pgpool_error_message(db));
        return;
    }
    defer_pqclear(count_res);

    int64_t total_count = 0;
    if (PQntuples(count_res) > 0) {
        const char* count_str = PQgetvalue(count_res, 0, 0);
        if (count_str) { total_count = strtoll(count_str, NULL, 10); }
    }

    int offset = (page - 1) * page_size;
    const char* query = NULL;
    int param_count = 0;
    const char* params[3];
    char limit_str[32];
    char offset_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", page_size);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    char name_filter[128] = {0};
    if (name) {
        snprintf(name_filter, sizeof(name_filter), "%%%s%%", name);
        query = "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT $2 OFFSET $3";
        params[0] = name_filter;
        params[1] = limit_str;
        params[2] = offset_str;
        param_count = 3;
    } else {
        query = "SELECT id, name, path, num_pages FROM files ORDER BY name LIMIT $1 OFFSET $2";
        params[0] = limit_str;
        params[1] = offset_str;
        param_count = 2;
    }

    PGresult* res = pgpool_query_params(db, query, param_count, params, -1);
    HANDLE_QUERY_ERROR(res, conn);
    defer_pqclear(res);

    size_t jsonlen = 0;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    char* json_str = json_create_file_list(&alc, res, page, page_size, total_count, &jsonlen);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    CACHE_SET_AND_SEND(g_res_cache, cache_key, key_len, conn, json_str, jsonlen, 0);
}

/**
 * GET /files/:file_id
 *
 * Returns metadata for a single file (name, path, page count) as JSON.
 * Results are cached keyed on the file ID.
 *
 * Path parameters:
 *   file_id - Integer database identifier of the file.
 */
void get_file_by_id(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    const char* file_id_str = get_path_param(conn, "file_id");
    int64_t file_id = strtoll(file_id_str, NULL, 10);

    char cache_key[CACHE_KEY_MAX_LEN] = {0};
    size_t key_len = (size_t)cache_make_key(cache_key, sizeof(cache_key), file_id, -1);

    CACHE_HIT_RETURN(g_res_cache, cache_key, key_len, conn);
    ACQUIRE_DB(db);

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    HANDLE_QUERY_ERROR(res, conn);
    defer_pqclear(res);

    HANDLE_RECORD_NOT_FOUND(res, conn, "No book matches the requested ID");

    const char* file_name = PQgetvalue(res, 0, 0);
    const char* file_path = PQgetvalue(res, 0, 1);
    const char* num_pages_str = PQgetvalue(res, 0, 2);
    int64_t num_pages = strtoll(num_pages_str, NULL, 10);

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    size_t jsonlen = 0;
    char* json_str = json_create_file_response(&alc, file_id, file_name, file_path, num_pages, &jsonlen);
    abort_if_nullptr(json_str, conn, "Failed to serialize JSON");

    CACHE_SET_AND_SEND(g_res_cache, cache_key, key_len, conn, json_str, jsonlen, 0);
}
