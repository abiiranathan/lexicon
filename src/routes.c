#include <pgconn/pgtypes.h>
#include <solidc/cache.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/str_to_num.h>

#include "../include/ai.h"
#include "../include/database.h"
#include "../include/json_response.h"
#include "../include/pdf.h"
#include "../include/routes.h"

#define CACHE_KEY_MAX_LEN 128

// Global response cache
static cache_t* g_response_cache = NULL;

bool init_response_cache(size_t capacity, uint32_t ttl_seconds) {
    g_response_cache = cache_create(capacity, ttl_seconds);
    return g_response_cache != NULL;
}

void destroy_response_cache(void) {
    cache_destroy(g_response_cache);
    g_response_cache = NULL;
}

/**
 * Sends a JSON error response to the client.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    char* json_str = json_create_error(msg);
    if (json_str) {
        conn_send_json(conn, StatusBadRequest, json_str);
        free(json_str);
    } else {
        conn_send(conn, StatusInternalServerError, "{\"error\": \"Internal error\"}", 27);
    }
}

// Send JSON from cache. Sends non-null terminated data.
static inline void send_cache_json(PulsarConn* conn, const char* data, size_t len) {
    conn_set_content_type(conn, "application/json");
    conn_send(conn, StatusOK, data, len);
}

static inline int cache_make_key(char* buffer, size_t buffer_size, int64_t file_id, int page_num) {
    if (!buffer || buffer_size == 0) return 0;
    if (page_num >= 0) {
        return snprintf(buffer, buffer_size, "file:%lld:page:%d", (long long)file_id, page_num);
    } else {
        return snprintf(buffer, buffer_size, "file:%lld", (long long)file_id);
    }
}

void get_page_by_file_and_page(PulsarCtx* ctx) {
    PulsarConn* conn         = ctx->conn;
    const char* file_id_str  = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

    ASSERT(g_response_cache);

    int64_t file_id;
    int64_t page_num_ll;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_i64(page_num_str, &page_num_ll) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    char cache_key[CACHE_KEY_MAX_LEN] = {};
    cache_make_key(cache_key, sizeof(cache_key), file_id, (int)page_num_ll);
    size_t cached_len       = 0;
    size_t key_len          = strlen(cache_key);
    const char* cached_json = cache_get(g_response_cache, cache_key, key_len, &cached_len);
    if (cached_json) {
        send_cache_json(conn, cached_json, cached_len);
        cache_release(cached_json);  // release zero-copy reference
        return;
    }

    pgconn_t* db             = get_pgconn(conn);
    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[]     = {file_id_str, page_num_str};

    PGresult* res = pgconn_query_params(db, query, 2, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No page found for the requested file and page number");
        return;
    }

    const char* text = PQgetvalue(res, 0, 0);

    // Use new JSON generator
    char* json_str = json_create_page_response(file_id, (int)page_num_ll, text);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    cache_set(g_response_cache, cache_key, key_len, (unsigned char*)json_str, strlen(json_str), 0);
    conn_send_json(conn, StatusOK, json_str);
    free(json_str);
}

void render_pdf_page_as_png(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;

    const char* file_id_str  = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");
    const char* type         = query_get(conn, "type");

    if (type == NULL) {
        type = "png";
    }

    // Must be png or PDF.
    if (!(strcmp(type, "png") == 0 || strcmp(type, "pdf") == 0)) {
        send_json_error(conn, "type query parameter must be either 'png' or 'pdf'");
        return;
    }

    int64_t file_id;
    int page_num;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_int(page_num_str, &page_num) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    char cache_key[CACHE_KEY_MAX_LEN] = {};
    snprintf(cache_key, sizeof(cache_key), "render-page:file:%lld:page:%d:type:%s", (long long)file_id, page_num, type);

    static const char png_headers[] = "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
    static const char pdf_headers[] = "Content-Type: application/pdf\r\nCache-Control: public, max-age=3600\r\n";
    size_t length                   = 0;
    size_t key_len                  = strlen(cache_key);

    const char* bytes = cache_get(g_response_cache, cache_key, key_len, &length);
    if (bytes) {
        if (strcmp(type, "png") == 0) {
            conn_writeheader_raw(conn, png_headers, sizeof(png_headers) - 1);
        } else {
            conn_writeheader_raw(conn, pdf_headers, sizeof(pdf_headers) - 1);
        }
        conn_write(conn, bytes, length);
        cache_release(bytes);
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = get_pgconn(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No file found for the requested file");
        return;
    }

    const char* path = PQgetvalue(res, 0, 0);

    if (page_num < 1) {
        send_json_error(conn, "Page out of range");
        return;
    }

    pdf_buffer_t byte_buf                                                            = {0};
    bool (*renderFunc)(const char* pdf_path, int page_num, pdf_buffer_t* out_buffer) = NULL;

    if (strcmp(type, "png") == 0) {
        conn_writeheader_raw(conn, png_headers, sizeof(png_headers) - 1);
        renderFunc = render_page_from_document_to_buffer;
    } else {
        conn_writeheader_raw(conn, pdf_headers, sizeof(pdf_headers) - 1);
        renderFunc = render_page_from_document_to_pdf_buffer;
    }

    if (renderFunc(path, page_num - 1, &byte_buf)) {
        conn_write(conn, byte_buf.data, byte_buf.size);
        cache_set(g_response_cache, cache_key, key_len, byte_buf.data, byte_buf.size, 60);
    } else {
        conn_set_status(conn, StatusInternalServerError);
        send_json_error(conn, "Error writing data");
        return;
    }
}

void pdf_search(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    ASSERT(g_response_cache);

    const char* fileId     = query_get(conn, "file_id");
    const char* query      = query_get(conn, "q");
    const char* ai_enabled = query_get(conn, "ai_enabled");
    const bool use_ai      = !(ai_enabled && strcmp(ai_enabled, "false") == 0);

    if (!query || query[0] == '\0') {
        send_json_error(conn, "Missing search query");
        return;
    }

    // Cache key generation remains the same
    char cache_key[CACHE_KEY_MAX_LEN] = {};
    snprintf(cache_key, sizeof(cache_key), "search:%s:%s", query, fileId ? fileId : "all");
    size_t key_len = strlen(cache_key);

    size_t cached_len       = 0;
    const char* cached_json = cache_get(g_response_cache, cache_key, key_len, &cached_len);
    if (cached_json) {
        send_cache_json(conn, cached_json, cached_len);
        cache_release(cached_json);
        return;
    }

    pgconn_t* db = get_pgconn(conn);

    const char* params[2];
    int param_count = 1;
    params[0]       = query;

    const char* common_cte =
        "WITH input_queries AS ("
        "  SELECT "
        "    websearch_to_tsquery('english', $1) as broad_query, "
        "    phraseto_tsquery('english', $1) as phrase_query "
        "), "
        "RankedPages AS ("
        "  SELECT "
        "    p.file_id, p.page_num, "
        "    ( "
        "      ts_rank_cd(p.text_vector, inputs.broad_query) "
        "      + "
        "      (CASE WHEN p.text_vector @@ inputs.phrase_query THEN 10.0 ELSE 0.0 END) "
        "    ) as rank "
        "  FROM pages p "
        "  CROSS JOIN input_queries inputs "
        "  WHERE p.text_vector @@ inputs.broad_query ";

    // Combining strings for specific file vs all files
    char full_query[4096];

    if (fileId) {
        params[1]   = fileId;
        param_count = 2;
        snprintf(full_query, sizeof(full_query),
                 "%s AND p.file_id = $2 "
                 "  ORDER BY rank DESC LIMIT 100 "
                 "), "
                 "UniquePages AS ("
                 "  SELECT DISTINCT ON (file_id, page_num) file_id, page_num, rank "
                 "  FROM RankedPages ORDER BY file_id, page_num, rank DESC "
                 ") "
                 "SELECT "
                 "  u.file_id, f.name, f.num_pages, u.page_num, "
                 "  ts_headline('english', p.text, inputs.broad_query, "
                 "    'StartSel=<b>, StopSel=</b>, MaxWords=200, MinWords=20') as snippet, "
                 "  LEFT(p.text, 2000) as extended_snippet, "
                 "  u.rank "
                 "FROM UniquePages u "
                 "CROSS JOIN input_queries inputs "  // Need inputs again for ts_headline
                 "JOIN files f ON u.file_id = f.id "
                 "JOIN pages p ON u.file_id = p.file_id AND u.page_num = p.page_num "
                 "ORDER BY u.rank DESC, f.name, u.page_num LIMIT 100",
                 common_cte);
    } else {
        snprintf(full_query, sizeof(full_query),
                 "%s "
                 "  ORDER BY rank DESC LIMIT 100 "
                 "), "
                 "UniquePages AS ("
                 "  SELECT DISTINCT ON (file_id, page_num) file_id, page_num, rank "
                 "  FROM RankedPages ORDER BY file_id, page_num, rank DESC "
                 ") "
                 "SELECT "
                 "  u.file_id, f.name, f.num_pages, u.page_num, "
                 "  ts_headline('english', p.text, inputs.broad_query, "
                 "    'StartSel=<b>, StopSel=</b>, MaxWords=200, MinWords=20') as snippet, "
                 "  LEFT(p.text, 2000) as extended_snippet, "
                 "  u.rank "
                 "FROM UniquePages u "
                 "CROSS JOIN input_queries inputs "
                 "JOIN files f ON u.file_id = f.id "
                 "JOIN pages p ON u.file_id = p.file_id AND u.page_num = p.page_num "
                 "ORDER BY u.rank DESC, f.name, u.page_num LIMIT 100",
                 common_cte);
    }

    PGresult* res = NULL;
    TIME_BLOCK("pdfsearch", { res = pgconn_query_params(db, full_query, param_count, params, NULL); });

    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    // --- Build Context for AI (keep logic here as it interacts with AI service) ---
    const size_t MAX_CONTEXT_SIZE = 30 * 1024;
    size_t context_capacity       = 32 * 1024;
    char* context                 = (char*)malloc(context_capacity);
    if (!context) {
        send_json_error(conn, "Failed to allocate memory for context");
        return;
    }
    defer({ free(context); });

    context[0]         = '\0';
    size_t context_len = 0;
    int ntuples        = PQntuples(res);
    int context_limit  = (ntuples < 15) ? ntuples : 15;

    for (int i = 0; i < context_limit; i++) {
        bool valid;
        int num_pages = pg_get_int(res, i, 2, &valid);
        int page_num  = pg_get_int(res, i, 3, &valid);
        if (!valid) continue;

        const char* file_name        = PQgetvalue(res, i, 1);
        const char* extended_snippet = PQgetvalue(res, i, 5);

        int header_len =
            snprintf(NULL, 0, "\n=== EXCERPT %d: [%s, Page %d of %d] ===\n", i + 1, file_name, page_num, num_pages);
        if (header_len < 0) continue;

        size_t snippet_len = strlen(extended_snippet);
        size_t needed      = context_len + (size_t)header_len + snippet_len + 3;

        if (needed > MAX_CONTEXT_SIZE) break;

        if (needed > context_capacity) {
            size_t new_cap = context_capacity * 2;
            while (new_cap < needed)
                new_cap *= 2;
            char* new_ctx = realloc(context, new_cap);
            if (!new_ctx) break;

            context          = new_ctx;
            context_capacity = new_cap;
        }

        int written = snprintf(context + context_len, context_capacity - context_len,
                               "\n=== EXCERPT %d: [%s, Page %d of %d] ===\n", i + 1, file_name, page_num, num_pages);

        if (written > 0) {
            context_len += (size_t)written;
            memcpy(context + context_len, extended_snippet, snippet_len);
            context_len += snippet_len;
            context[context_len++] = '\n';
            context[context_len++] = '\n';
            context[context_len]   = '\0';
        }
    }

    char* ai_summary       = NULL;
    bool ai_summary_cached = false;

    if (use_ai) {
        const char* api_key = getenv("GEMINI_API_KEY");
        if (api_key && context_len > 0 && fileId == NULL) {
            TIME_BLOCK("GEMINI API CALL",
                       { ai_summary = get_ai_summary(query, context, api_key, &ai_summary_cached); });
        }
    }

    // --- Use JSON Generator ---
    size_t jsonlen = 0;
    char* json_str = json_create_search_results(res, query, ai_summary, &jsonlen);
    if (ai_summary) {
        if (ai_summary_cached) {
            // Decrease reference count for this pointer.
            deref_ai_response(ai_summary);
        } else {
            free(ai_summary);
        }
    };

    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    cache_set(g_response_cache, cache_key, key_len, (unsigned char*)json_str, jsonlen, 60);
    conn_send_json(conn, StatusOK, json_str);
    free(json_str);
}

void list_files(PulsarCtx* ctx) {
    PulsarConn* conn          = ctx->conn;
    const char* page_str      = query_get(conn, "page");
    const char* page_size_str = query_get(conn, "limit");
    const char* name          = query_get(conn, "name");

    ASSERT(g_response_cache);

    int page = 1, page_size = 10;
    if (page_str) str_to_int(page_str, &page);
    if (page_size_str) str_to_int(page_size_str, &page_size);

    if (page < 1) page = 1;
    if (page_size < 1) page_size = 25;
    if (page_size > 100) page_size = 100;

    char cache_key[CACHE_KEY_MAX_LEN] = {};
    if (name && name[0] != '\0') {
        snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d:n%s", page, page_size, name);
    } else {
        snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d", page, page_size);
    }

    size_t cached_len       = 0;
    size_t key_len          = strlen(cache_key);
    const char* cached_json = cache_get(g_response_cache, cache_key, key_len, &cached_len);
    if (cached_json) {
        send_cache_json(conn, cached_json, cached_len);
        cache_release(cached_json);
        return;
    }

    pgconn_t* db = get_pgconn(conn);

    const char* count_query = "SELECT COUNT(*) FROM files";
    PGresult* count_res     = pgconn_query(db, count_query, NULL);
    if (!count_res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(count_res); });

    int64_t total_count = 0;
    if (PQntuples(count_res) > 0) {
        const char* count_str = PQgetvalue(count_res, 0, 0);
        if (count_str) total_count = strtoll(count_str, NULL, 10);
    }

    int offset = (page - 1) * page_size;
    const char* query;
    int param_count;
    const char* params[3];
    char limit_str[32], offset_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", page_size);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    if (name && name[0] != '\0') {
        char nameFilter[128];
        snprintf(nameFilter, sizeof(nameFilter), "%%%s%%", name);
        query     = "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT $2 OFFSET $3";
        params[0] = nameFilter;
        params[1] = limit_str;
        params[2] = offset_str;
        param_count = 3;
    } else {
        query       = "SELECT id, name, path, num_pages FROM files ORDER BY name LIMIT $1 OFFSET $2";
        params[0]   = limit_str;
        params[1]   = offset_str;
        param_count = 2;
    }

    PGresult* res = pgconn_query_params(db, query, param_count, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    // Use new JSON generator
    size_t jsonlen = 0;
    char* json_str = json_create_file_list(res, page, page_size, total_count, &jsonlen);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }

    cache_set(g_response_cache, cache_key, key_len, (unsigned char*)json_str, jsonlen, 0);
    conn_send_json(conn, StatusOK, json_str);
    free(json_str);
}

void get_file_by_id(PulsarCtx* ctx) {
    ASSERT(g_response_cache);

    PulsarConn* conn        = ctx->conn;
    const char* file_id_str = get_path_param(conn, "file_id");
    int64_t file_id;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    char cache_key[CACHE_KEY_MAX_LEN] = {};
    cache_make_key(cache_key, sizeof(cache_key), file_id, -1);

    size_t cached_len       = 0;
    size_t key_len          = strlen(cache_key);
    const char* cached_json = cache_get(g_response_cache, cache_key, key_len, &cached_len);
    if (cached_json) {
        send_cache_json(conn, cached_json, cached_len);
        cache_release(cached_json);
        return;
    }

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = get_pgconn(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No book matches the requested ID");
        return;
    }

    const char* file_name     = PQgetvalue(res, 0, 0);
    const char* file_path     = PQgetvalue(res, 0, 1);
    const char* num_pages_str = PQgetvalue(res, 0, 2);
    int64_t num_pages         = strtoll(num_pages_str, NULL, 10);

    // Use new JSON generator
    size_t length  = 0;
    char* json_str = json_create_file_response(file_id, file_name, file_path, num_pages, &length);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    cache_set(g_response_cache, cache_key, key_len, (unsigned char*)json_str, length, 0);
    conn_send_json(conn, StatusOK, json_str);
    free(json_str);
}
