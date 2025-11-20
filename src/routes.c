#include <pgconn/pgtypes.h>
#include <pulsar/pulsar.h>
#include <solidc/cache.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <solidc/str_to_num.h>
#include <yyjson.h>

#include "../include/ai.h"
#include "../include/database.h"
#include "../include/pdf.h"
#include "../include/routes.h"

// Global response cache
static cache_t* g_response_cache = NULL;

/**
 * Initializes the global response cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_response_cache(size_t capacity, uint32_t ttl_seconds) {
    g_response_cache = cache_create(capacity, ttl_seconds);
    return g_response_cache != NULL;
}

/**
 * Destroys the global response cache.
 * Call this at application shutdown.
 */
void destroy_response_cache(void) {
    cache_destroy(g_response_cache);
    g_response_cache = NULL;
}

/**
 * Sends a JSON error response to the client.
 * @param conn The connection to send the error on.
 * @param msg The error message. If NULL, uses a default message.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    char json_str[128];
    snprintf(json_str, sizeof(json_str) - 1, "{\"error\": \"%s\"}", msg ? msg : "Something wrong happened");
    conn_send_json(conn, StatusBadRequest, json_str);
}

// Send JSON from cache. Sends non-null terminated data.
// builtin conn_send_json assumes null-terminated data and is not fit for the purpose.
static inline void send_cache_json(PulsarConn* conn, const char* data, size_t len) {
    conn_set_content_type(conn, "application/json");
    conn_send(conn, StatusOK, data, len);
}

/**
 * Generates a cache key for file/page lookups.
 * @param buffer Buffer to store the key.
 * @param buffer_size Size of the buffer.
 * @param file_id File ID.
 * @param page_num Page number (use -1 for file-only keys).
 * @return Number of characters written (excluding null terminator).
 */
static inline int cache_make_key(char* buffer, size_t buffer_size, int64_t file_id, int page_num) {
    if (!buffer || buffer_size == 0) {
        return 0;
    }
    if (page_num >= 0) {
        return snprintf(buffer, buffer_size, "file:%lld:page:%d", (long long)file_id, page_num);
    } else {
        return snprintf(buffer, buffer_size, "file:%lld", (long long)file_id);
    }
}

/**
 * Returns full text for a given file_id and page_num.
 * Path: /api/file/{file_id}/page/{page_num}
 */
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

    // Try cache first
    char cache_key[CACHE_KEY_MAX_LEN];
    cache_make_key(cache_key, sizeof(cache_key), file_id, (int)page_num_ll);

    char* cached_json = nullptr;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    pgconn_t* db             = get_pgconn(conn);
    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[]     = {file_id_str, page_num_str};

    PGresult* res = pgconn_query_params(db, query, 2, params, nullptr);
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

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    yyjson_mut_obj_add_int(doc, root, "page_num", (int)page_num_ll);
    yyjson_mut_obj_add_str(doc, root, "text", text ? text : "");

    char* json_str = yyjson_mut_write(doc, 0, nullptr);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    // Cache the response
    cache_set(g_response_cache, cache_key, (unsigned char*)json_str, strlen(json_str), 0);

    conn_send_json(conn, StatusOK, json_str);
}

void render_pdf_page_as_png(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;

    const char* file_id_str  = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

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

    // Try cache first
    char cache_key[CACHE_KEY_MAX_LEN];
    snprintf(cache_key, sizeof(cache_key), "render-page:file:%lld:page:%d", (long long)file_id, page_num);

    char* png_data  = nullptr;
    size_t png_size = 0;
    if (cache_get(g_response_cache, cache_key, &png_data, &png_size)) {
        static const char headers[] = "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
        conn_writeheader_raw(conn, headers, sizeof(headers) - 1);
        conn_write(conn, png_data, png_size);
        free(png_data);
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = get_pgconn(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, nullptr);
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

    // Page numbers are 1-based, so we subtract 1
    if (page_num < 1) {
        send_json_error(conn, "Page out of range");
        return;
    }

    png_buffer_t png_buffer = {0};
    if (render_page_from_document_to_buffer(path, page_num - 1, &png_buffer)) {
        static const char headers[] = "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
        conn_writeheader_raw(conn, headers, sizeof(headers) - 1);
        conn_write(conn, png_buffer.data, png_buffer.size);

        // Cache the page for 60 seconds
        cache_set(g_response_cache, cache_key, png_buffer.data, png_buffer.size, 60);
    } else {
        conn_set_status(conn, StatusInternalServerError);
        send_json_error(conn, "Error writing PNG image");
        return;
    }
}

/**
 * Performs full-text search on PDF pages.
 * Returns matching results with snippets, ranked by relevance.
 * Sends smart snippets to Gemini instead of full page content for better performance.
 */
void pdf_search(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    ASSERT(g_response_cache);

    const char* fileId = query_get(conn, "file_id");  // possibly undefined (NULL)
    const char* query  = query_get(conn, "q");
    if (!query || query[0] == '\0') {
        send_json_error(conn, "Missing search query");
        return;
    }

    // Generate cache key for search query
    char cache_key[CACHE_KEY_MAX_LEN];
    snprintf(cache_key, sizeof(cache_key), "search:%s", query);

    // Try cache first
    char* cached_json = NULL;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    pgconn_t* db = get_pgconn(conn);

    // Build a query suffix
    char query_suffix[128];
    snprintf(query_suffix, sizeof(query_suffix), "%s", query);

    // Assume only query is provided. Update if file_id is also provided.
    const char* params[2];
    int param_count          = 1;
    params[0]                = query_suffix;
    const char* search_query = NULL;

    if (fileId) {
        search_query =
            "WITH query AS ("
            "  SELECT websearch_to_tsquery('english', $1) as tsq"
            "), "
            "RankedChunks AS ("
            "  SELECT "
            "    p.file_id, f.name, f.num_pages, p.page_num, p.text, "
            "    ts_rank(p.text_vector, query.tsq) as rank "
            "  FROM pages p "
            "  CROSS JOIN query "
            "  JOIN files f ON p.file_id = f.id "
            "  WHERE p.text_vector @@ query.tsq "
            "    AND ts_rank(p.text_vector, query.tsq) >= 0.005 "
            "    AND f.id = $2 "
            "), "
            "UniquePages AS ("
            "  SELECT DISTINCT ON (file_id, page_num) "
            "    file_id, name, num_pages, page_num, text, rank "
            "  FROM RankedChunks "
            "  ORDER BY file_id, page_num, rank DESC"
            ") "
            "SELECT "
            "  file_id, name, num_pages, page_num, "
            "  LEFT(text, 500) as snippet, "
            "  LEFT(text, 2000) as extended_snippet "  // Larger snippet for AI context
            "FROM UniquePages "
            "ORDER BY rank DESC, name, page_num "
            "LIMIT 100";
        params[1]   = fileId;
        param_count = 2;
    } else {
        search_query =
            "WITH query AS ("
            "  SELECT websearch_to_tsquery('english', $1) as tsq"
            "), "
            "RankedChunks AS ("
            "  SELECT "
            "    p.file_id, f.name, f.num_pages, p.page_num, p.text, "
            "    ts_rank(p.text_vector, query.tsq) as rank "
            "  FROM pages p "
            "  CROSS JOIN query "
            "  JOIN files f ON p.file_id = f.id "
            "  WHERE p.text_vector @@ query.tsq "
            "    AND ts_rank(p.text_vector, query.tsq) >= 0.005 "
            "), "
            "UniquePages AS ("
            "  SELECT DISTINCT ON (file_id, page_num) "
            "    file_id, name, num_pages, page_num, text, rank "
            "  FROM RankedChunks "
            "  ORDER BY file_id, page_num, rank DESC"
            ") "
            "SELECT "
            "  file_id, name, num_pages, page_num, "
            "  LEFT(text, 500) as snippet, "
            "  LEFT(text, 2000) as extended_snippet "  // Larger snippet for AI context
            "FROM UniquePages "
            "ORDER BY rank DESC, name, page_num "
            "LIMIT 100";
    }

    PGresult* res = NULL;
    TIME_BLOCK("pdfsearch", { res = pgconn_query_params(db, search_query, param_count, params, NULL); });

    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    // Create mutable document for building JSON
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);

    // Build context from extended snippets (much more efficient than full pages)
    // Target: ~30KB total context (about 7,500 tokens)
    const size_t MAX_CONTEXT_SIZE = 30 * 1024;  // 30KB limit
    size_t context_capacity       = 32 * 1024;  // Start with 32KB buffer
    char* context                 = (char*)malloc(context_capacity);
    if (!context) {
        send_json_error(conn, "Failed to allocate memory for context");
        return;
    }
    defer({ free(context); });

    context[0]         = '\0';
    size_t context_len = 0;
    int ntuples        = PQntuples(res);
    bool file_id_valid, page_num_valid, num_pages_valid;

    // Limit to top 15 results for AI context to avoid token limits
    // With 2000 char snippets, 15 results = ~30KB (perfect for Gemini)
    int context_limit = (ntuples < 15) ? ntuples : 15;

    for (int i = 0; i < ntuples; i++) {
        int64_t file_id;
        int page_num, num_pages;

        // Parse all numeric fields - skip row if any fail
        file_id   = pg_get_longlong(res, i, 0, &file_id_valid);
        num_pages = pg_get_int(res, i, 2, &num_pages_valid);
        page_num  = pg_get_int(res, i, 3, &page_num_valid);
        if (!file_id_valid || !page_num_valid || !num_pages_valid) {
            continue;
        }

        const char* file_name        = PQgetvalue(res, i, 1);
        const char* snippet          = PQgetvalue(res, i, 4);
        const char* extended_snippet = PQgetvalue(res, i, 5);  // Get extended snippet

        ASSERT(file_name);         // we must have a filename
        ASSERT(extended_snippet);  // we must have some text

        // Build context from top N results with EXTENDED snippets (not full pages)
        if (i < context_limit && context_len < MAX_CONTEXT_SIZE) {
            // Calculate space needed
            int header_len_int =
                snprintf(NULL, 0, "\n=== EXCERPT %d: [%s, Page %d of %d] ===\n", i + 1, file_name, page_num, num_pages);
            if (header_len_int < 0) {
                continue;  // snprintf error, skip this entry
            }

            size_t snippet_len = strlen(extended_snippet);
            size_t needed      = context_len + (size_t)header_len_int + snippet_len + 3;

            // Check if we'd exceed max context size
            if (needed > MAX_CONTEXT_SIZE) {
                fprintf(stderr, "[PDF Search] Info: Stopping context build at %zu chars (limit %zu)\n", context_len,
                        MAX_CONTEXT_SIZE);
                break;
            }

            // Reallocate if needed
            if (needed > context_capacity) {
                size_t new_capacity = context_capacity * 2;
                while (new_capacity < needed) {
                    new_capacity *= 2;
                }
                char* new_context = (char*)realloc(context, new_capacity);
                if (!new_context) {
                    fprintf(stderr, "[PDF Search] Warning: Failed to expand context buffer\n");
                    break;
                }
                context          = new_context;
                context_capacity = new_capacity;
            }

            // Add header
            int written = snprintf(context + context_len, context_capacity - context_len,
                                   "\n=== EXCERPT %d: [%s, Page %d of %d] ===\n", i + 1,
                                   file_name ? file_name : "Unknown", page_num, num_pages);

            if (written > 0) {
                context_len += (size_t)written;

                // Add extended snippet (2000 chars max per result)
                size_t remaining = context_capacity - context_len - 1;
                if (snippet_len < remaining) {
                    memcpy(context + context_len, extended_snippet, snippet_len);
                    context_len += snippet_len;
                    context[context_len++] = '\n';
                    context[context_len++] = '\n';
                    context[context_len]   = '\0';
                }
            }
        }

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        if (!result_obj) {
            continue;
        }

        yyjson_mut_obj_add_int(doc, result_obj, "file_id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "file_name", file_name ? file_name : "");
        yyjson_mut_obj_add_int(doc, result_obj, "page_num", page_num);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", num_pages);
        yyjson_mut_obj_add_str(doc, result_obj, "snippet", snippet ? snippet : "");
        yyjson_mut_arr_append(results_array, result_obj);
    }

    yyjson_mut_obj_add_val(doc, root, "results", results_array);
    yyjson_mut_obj_add_uint(doc, root, "count", yyjson_mut_arr_size(results_array));
    yyjson_mut_obj_add_str(doc, root, "query", query);

    // Get AI summary with EXTENDED SNIPPETS if we have results and API key
    const char* api_key = getenv("GEMINI_API_KEY");

    // Skip searches within the book itself.
    if (api_key && ntuples > 0 && context_len > 0 && fileId == NULL) {
        char* ai_summary = NULL;
        TIME_BLOCK("GEMINI API CALL", { ai_summary = get_ai_summary(query, context, api_key); });

        fprintf(stderr, "[PDF Search] Info: Sending %zu characters (%d excerpts) to Gemini\n", context_len,
                context_limit);
        if (ai_summary) {
            size_t summary_len          = strlen(ai_summary);
            yyjson_mut_val* summary_val = yyjson_mut_strncpy(doc, ai_summary, summary_len);
            if (summary_val) {
                yyjson_mut_obj_add_val(doc, root, "ai_summary", summary_val);
            } else {
                yyjson_mut_obj_add_null(doc, root, "ai_summary");
            }
            free(ai_summary);
        } else {
            yyjson_mut_obj_add_null(doc, root, "ai_summary");
        }
    } else {
        yyjson_mut_obj_add_null(doc, root, "ai_summary");
    }

    // Serialize to string
    size_t jsonlen = 0;
    char* json_str = yyjson_mut_write(doc, 0, &jsonlen);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    // Cache the search results with shorter TTL (60 seconds)
    cache_set(g_response_cache, cache_key, (unsigned char*)json_str, jsonlen, 60);

    conn_send_json(conn, StatusOK, json_str);
}

/**
 * Lists all PDF files in the database paginated on query params "page" & limit.
 * Returns JSON object with keys: "page", "limit", "results", "has_next", "has_prev","total_count",
 * "total_pages".
 */
void list_files(PulsarCtx* ctx) {
    PulsarConn* conn          = ctx->conn;
    const char* page_str      = query_get(conn, "page");
    const char* page_size_str = query_get(conn, "limit");
    const char* name          = query_get(conn, "name");

    ASSERT(g_response_cache);

    int page = 1, page_size = 10;
    if (page_str) {
        str_to_int(page_str, &page);
    }

    if (page_size_str) {
        str_to_int(page_size_str, &page_size);
    }

    // Ensure page is at least 1
    if (page < 1) {
        page = 1;
    }
    // Ensure page_size is reasonable
    if (page_size < 1) {
        page_size = 25;
    }
    if (page_size > 100) {
        page_size = 100;
    }

    // Generate cache key including pagination and name filter
    char cache_key[CACHE_KEY_MAX_LEN];
    if (name && name[0] != '\0') {
        snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d:n%s", page, page_size, name);
    } else {
        snprintf(cache_key, sizeof(cache_key), "list:p%d:l%d", page, page_size);
    }

    // Try cache first
    char* cached_json = NULL;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    pgconn_t* db = get_pgconn(conn);

    // First, get the total count of files
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
        if (count_str) {
            total_count = strtoll(count_str, NULL, 10);
        }
    }

    // Calculate offset for pagination
    int offset = (page - 1) * page_size;

    // After validating page_size, add name validation
    const char* query;
    int param_count;
    const char* params[3];  // Increased from 2 to accommodate name parameter

    // Prepare limit and offset parameters
    char limit_str[32], offset_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", page_size);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    if (name && name[0] != '\0') {
        // Filter by name using case-insensitive ILIKE
        char nameFilter[128];
        snprintf(nameFilter, sizeof(nameFilter), "%%%s%%", name);
        query =
            "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT "
            "$2 OFFSET $3";
        params[0]   = nameFilter;
        params[1]   = limit_str;
        params[2]   = offset_str;
        param_count = 3;
    } else {
        // No name filter
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

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    // Create root object instead of array
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    if (!root) {
        send_json_error(conn, "Failed to create JSON root object");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);

    // Create results array
    yyjson_mut_val* results_array = yyjson_mut_arr(doc);
    if (!results_array) {
        send_json_error(conn, "Failed to create JSON results array");
        return;
    }
    yyjson_mut_obj_add_val(doc, root, "results", results_array);

    int ntuples = PQntuples(res);
    for (int i = 0; i < ntuples; i++) {
        const char* file_id_str   = PQgetvalue(res, i, 0);
        const char* file_name     = PQgetvalue(res, i, 1);
        const char* file_path     = PQgetvalue(res, i, 2);
        const char* num_pages_str = PQgetvalue(res, i, 3);

        if (!file_id_str || !file_name || !file_path || !num_pages_str) {
            continue;
        }

        // Convert file ID and num_pages to integers
        int64_t file_id   = strtoll(file_id_str, NULL, 10);
        int64_t num_pages = strtoll(num_pages_str, NULL, 10);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        if (!result_obj) {
            continue;
        }

        yyjson_mut_obj_add_int(doc, result_obj, "id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "name", file_name);
        yyjson_mut_obj_add_str(doc, result_obj, "path", file_path);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", (int)num_pages);

        yyjson_mut_arr_append(results_array, result_obj);
    }

    // Add pagination metadata
    yyjson_mut_obj_add_int(doc, root, "page", page);
    yyjson_mut_obj_add_int(doc, root, "limit", page_size);
    yyjson_mut_obj_add_int(doc, root, "total_count", (int)total_count);

    // Calculate has_next and has_prev
    int64_t total_pages = (total_count + page_size - 1) / page_size;
    if (total_pages == 0) {
        total_pages = 1;  // At least one page even if empty
    }
    yyjson_mut_obj_add_bool(doc, root, "has_next", page < total_pages);
    yyjson_mut_obj_add_bool(doc, root, "has_prev", page > 1);
    yyjson_mut_obj_add_int(doc, root, "total_pages", (int)total_pages);

    char* json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }

    // Cache the list response
    cache_set(g_response_cache, cache_key, (unsigned char*)json_str, strlen(json_str), 0);

    conn_send_json(conn, StatusOK, json_str);
}

/** Retrieves a single file by its ID. */
void get_file_by_id(PulsarCtx* ctx) {
    ASSERT(g_response_cache);

    PulsarConn* conn        = ctx->conn;
    const char* file_id_str = get_path_param(conn, "file_id");
    int64_t file_id;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    // Try cache first
    char cache_key[CACHE_KEY_MAX_LEN];
    cache_make_key(cache_key, sizeof(cache_key), file_id, -1);

    char* cached_json = NULL;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
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

    int64_t num_pages = strtoll(num_pages_str, NULL, 10);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "id", file_id);
    yyjson_mut_obj_add_str(doc, root, "name", file_name);
    yyjson_mut_obj_add_str(doc, root, "path", file_path);
    yyjson_mut_obj_add_int(doc, root, "num_pages", (int)num_pages);

    size_t length  = 0;
    char* json_str = yyjson_mut_write(doc, 0, &length);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    // Cache the file metadata
    cache_set(g_response_cache, cache_key, (unsigned char*)json_str, length, 0);
    conn_send_json(conn, StatusOK, json_str);
}
