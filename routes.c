#include <curl/curl.h>
#include <pgconn/pgconn.h>
#include <pgconn/pgtypes.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/str_to_num.h>
#include <string.h>
#include <yyjson.h>
#include "pdf.h"

#include <solidc/cache.h>

// Connections array with each worker getting its own connection.
extern pgconn_t* connections[NUM_WORKERS];

// Global response cache - initialize at startup with cache_create()
static cache_t* g_response_cache = nullptr;

// Get current connection for worker.
#define DB(conn) ((connections[(size_t)conn_worker_id(conn)]))

/**
 * Initializes the global response cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_response_cache(size_t capacity, uint32_t ttl_seconds) {
    g_response_cache = cache_create(capacity, ttl_seconds);
    return g_response_cache != nullptr;
}

/**
 * Destroys the global response cache.
 * Call this at application shutdown.
 */
void destroy_response_cache(void) {
    cache_destroy(g_response_cache);
    g_response_cache = nullptr;
}

/**
 * Sends a JSON error response to the client.
 * @param conn The connection to send the error on.
 * @param msg The error message. If NULL, uses a default message.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    char json_str[128];
    snprintf(json_str, sizeof(json_str) - 1, "{\"error\": \"%s\"}",
             msg ? msg : "Something wrong happened");
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

// Structure to capture curl response
typedef struct {
    char* data;
    size_t size;
} curl_response_t;

// Callback for curl to write response data
static size_t _write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize       = size * nmemb;
    curl_response_t* resp = (curl_response_t*)userp;

    char* ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) {
        return 0;  // Out of memory
    }

    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = 0;

    return realsize;
}

/**
 * Calls Gemini API to generate AI summary for search results.
 * Enhanced with better prompting to allow Gemini to provide comprehensive answers.
 * @param query The search query
 * @param context The context from search results (complete page content)
 * @param api_key The Gemini API key
 * @return Allocated string with AI summary (caller must free), or NULL on error
 */
static char* get_ai_summary(const char* query, const char* context, const char* api_key) {
    if (!query || !context || !api_key) {
        return NULL;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return NULL;
    }

    curl_response_t response = {0};

    // Build the API URL
    char url[512];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/"
             "gemini-2.0-flash:generateContent?key=%s",
             api_key);

    // Build the JSON request body
    yyjson_mut_doc* req_doc = yyjson_mut_doc_new(NULL);
    if (!req_doc) {
        fprintf(stderr, "[AI Summary] Error: Failed to create JSON document\n");
        curl_easy_cleanup(curl);
        return NULL;
    }

    yyjson_mut_val* req_root = yyjson_mut_obj(req_doc);
    yyjson_mut_doc_set_root(req_doc, req_root);

    yyjson_mut_val* contents_arr = yyjson_mut_arr(req_doc);
    yyjson_mut_val* content_obj  = yyjson_mut_obj(req_doc);
    yyjson_mut_val* parts_arr    = yyjson_mut_arr(req_doc);
    yyjson_mut_val* part_obj     = yyjson_mut_obj(req_doc);

    // Enhanced prompt with complete page content and instructions for comprehensive answers
    char* prompt       = NULL;
    size_t context_len = strlen(context);
    size_t prompt_size = context_len + 2048;  // Extra space for prompt template
    prompt             = (char*)malloc(prompt_size);
    if (!prompt) {
        fprintf(stderr, "[AI Summary] Error: Failed to allocate memory for prompt\n");
        yyjson_mut_doc_free(req_doc);
        curl_easy_cleanup(curl);
        return NULL;
    }
    defer({ free(prompt); });

    snprintf(
        prompt, prompt_size,
        "You are an expert AI assistant helping users find information about their query. Queries "
        "are mostly about Medical and Programming. "
        "Use your comprehensive knowledge to provide a thorough, accurate answer. "
        "PDF page excerpts are provided below as additional context to supplement your "
        "response.\n\n"
        "USER QUERY: \"%s\"\n\n"
        "SUPPLEMENTARY PDF CONTEXT:\n"
        "%s\n\n"
        "INSTRUCTIONS:\n"
        "1. Draw primarily on your expert knowledge to answer the user's query comprehensively\n"
        "2. Use the provided PDF content to enhance, verify, or add specific details to your "
        "answer\n"
        "3. If the PDF content contradicts your knowledge or provides unique information, give it "
        "appropriate weight\n"
        "4. If the PDF pages are incomplete or don't fully address the query, fill in gaps with "
        "your knowledge while being clear about what comes from which source\n"
        "5. Synthesize information from multiple pages coherently when relevant\n"
        "6. For MEDICAL MANAGEMENT/TREATMENT queries, provide COMPLETE and DETAILED information:\n"
        "   - General/supportive management measures with full details\n"
        "   - Pharmacological management with:\n"
        "     * Specific drug names and formulations\n"
        "     * Complete dosing regimens (adult AND pediatric doses)\n"
        "     * Route of administration (oral, IV, IM, etc.)\n"
        "     * Timing and frequency of administration\n"
        "     * Duration of treatment\n"
        "   - Management protocols for both uncomplicated and severe/complicated cases\n"
        "   - Alternative treatment options when first-line therapy is unavailable or "
        "contraindicated\n"
        "   - Specific dosing adjustments (e.g., weight-based)\n"
        "   - Include ALL relevant details from both your knowledge base and the provided context\n"
        "7. Provide a well-structured, thorough response (typically 10-30 sentences for general "
        "queries, longer for comprehensive treatment protocols)\n"
        "8. Use ONLY valid HTML tags: <p>, <ul>, <li>, <ol>, <h3>, <h4>, <b>, <strong>, <em>, "
        "<i>, <br>\n"
        "9. Be accurate, detailed, and directly address the user's question\n\n"
        "CRITICAL OUTPUT FORMAT REQUIREMENTS:\n"
        "- Output ONLY raw HTML - NO markdown syntax\n"
        "- Do NOT use code fences (```html or ```)\n"
        "- Do NOT use markdown bold (**text**) - use <b>text</b> or <strong>text</strong>\n"
        "- Do NOT use markdown italics (*text*) - use <i>text</i> or <em>text</em>\n"
        "- Do NOT use markdown headers (# or ##) - use <h3> or <h4> tags\n"
        "- Start immediately with an HTML tag (like <h3> or <p>)\n"
        "- End with a closing HTML tag\n\n"
        "Your response must be pure HTML that can be directly inserted into a webpage without "
        "any preprocessing.",
        query, context);

    yyjson_mut_obj_add_str(req_doc, part_obj, "text", prompt);
    yyjson_mut_arr_append(parts_arr, part_obj);
    yyjson_mut_obj_add_val(req_doc, content_obj, "parts", parts_arr);
    yyjson_mut_arr_append(contents_arr, content_obj);
    yyjson_mut_obj_add_val(req_doc, req_root, "contents", contents_arr);

    char* json_body = yyjson_mut_write(req_doc, 0, NULL);
    yyjson_mut_doc_free(req_doc);

    if (!json_body) {
        fprintf(stderr, "[AI Summary] Error: Failed to serialize JSON request body\n");
        curl_easy_cleanup(curl);
        return NULL;
    }
    defer({ free(json_body); });

    fprintf(stderr, "[AI Summary] Info: Sending request to Gemini API for query: \"%s\"\n", query);
    fprintf(stderr, "[AI Summary] Info: Context size: %zu characters\n", context_len);

    // Setup curl with longer timeout for larger context
    struct curl_slist* headers = NULL;
    headers                    = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // Increased timeout for larger context

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[AI Summary] Error: curl_easy_perform() failed: %s (code: %d)\n",
                curl_easy_strerror(res), res);
        free(response.data);
        return NULL;
    }

    if (http_code != 200) {
        fprintf(stderr, "[AI Summary] Error: HTTP request failed with status code: %ld\n",
                http_code);
        if (response.data && response.size > 0) {
            fprintf(stderr, "[AI Summary] Error response body: %.*s\n",
                    (int)(response.size > 500 ? 500 : response.size), response.data);
        }
        free(response.data);
        return NULL;
    }

    if (!response.data || response.size == 0) {
        fprintf(stderr, "[AI Summary] Error: Empty response from API\n");
        free(response.data);
        return NULL;
    }

    fprintf(stderr, "[AI Summary] Info: Received response (%zu bytes)\n", response.size);

    // Parse response to extract summary text
    yyjson_doc* resp_doc = yyjson_read(response.data, response.size, 0);
    if (!resp_doc) {
        fprintf(stderr, "[AI Summary] Error: Failed to parse JSON response\n");
        free(response.data);
        return NULL;
    }
    free(response.data);
    defer({ yyjson_doc_free(resp_doc); });

    yyjson_val* root = yyjson_doc_get_root(resp_doc);
    if (!root) {
        fprintf(stderr, "[AI Summary] Error: No root object in JSON response\n");
        return NULL;
    }

    // Check for error in response
    yyjson_val* error_obj = yyjson_obj_get(root, "error");
    if (error_obj) {
        yyjson_val* error_msg = yyjson_obj_get(error_obj, "message");
        const char* error_str = yyjson_get_str(error_msg);
        fprintf(stderr, "[AI Summary] Error: API returned error: %s\n",
                error_str ? error_str : "Unknown error");
        return NULL;
    }

    yyjson_val* candidates = yyjson_obj_get(root, "candidates");
    if (!candidates) {
        fprintf(stderr, "[AI Summary] Error: No 'candidates' field in response\n");
        return NULL;
    }

    if (!yyjson_is_arr(candidates)) {
        fprintf(stderr, "[AI Summary] Error: 'candidates' is not an array\n");
        return NULL;
    }

    yyjson_val* first_candidate = yyjson_arr_get_first(candidates);
    if (!first_candidate) {
        fprintf(stderr, "[AI Summary] Error: 'candidates' array is empty\n");
        return NULL;
    }

    yyjson_val* content = yyjson_obj_get(first_candidate, "content");
    if (!content) {
        fprintf(stderr, "[AI Summary] Error: No 'content' field in candidate\n");
        return NULL;
    }

    yyjson_val* parts = yyjson_obj_get(content, "parts");
    if (!parts) {
        fprintf(stderr, "[AI Summary] Error: No 'parts' field in content\n");
        return NULL;
    }

    if (!yyjson_is_arr(parts)) {
        fprintf(stderr, "[AI Summary] Error: 'parts' is not an array\n");
        return NULL;
    }

    yyjson_val* first_part = yyjson_arr_get_first(parts);
    if (!first_part) {
        fprintf(stderr, "[AI Summary] Error: 'parts' array is empty\n");
        return NULL;
    }

    yyjson_val* text = yyjson_obj_get(first_part, "text");
    if (!text) {
        fprintf(stderr, "[AI Summary] Error: No 'text' field in part\n");
        return NULL;
    }

    if (!yyjson_is_str(text)) {
        fprintf(stderr, "[AI Summary] Error: 'text' field is not a string\n");
        return NULL;
    }

    const char* summary_text = yyjson_get_str(text);
    if (!summary_text) {
        fprintf(stderr, "[AI Summary] Error: Failed to extract text string\n");
        return NULL;
    }
    return strdup(summary_text);
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

    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[]     = {file_id_str, page_num_str};

    pgconn_t* db  = DB(conn);
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
    snprintf(cache_key, sizeof(cache_key), "render-page:file:%lld:page:%d", (long long)file_id,
             page_num);

    char* png_data  = nullptr;
    size_t png_size = 0;
    if (cache_get(g_response_cache, cache_key, &png_data, &png_size)) {
        static const char headers[] =
            "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
        conn_writeheader_raw(conn, headers, sizeof(headers) - 1);
        conn_write(conn, png_data, png_size);
        free(png_data);
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = DB(conn);
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
        static const char headers[] =
            "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
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
 * Fetches complete page content and sends to Gemini for AI-powered answers.
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
    char* cached_json = nullptr;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    pgconn_t* db = DB(conn);

    // Build a query suffix
    char query_suffix[128];
    snprintf(query_suffix, sizeof(query_suffix), "%s:*", query);

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
            "  text as full_text "  // Add full text
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
            "  text as full_text "  // Add full text
            "FROM UniquePages "
            "ORDER BY rank DESC, name, page_num "
            "LIMIT 100";
    }

    PGresult* res = pgconn_query_params(db, search_query, param_count, params, nullptr);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    // Create mutable document for building JSON
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);

    // Build context from complete page content (top 10 results)
    // Use dynamic allocation to handle larger context
    size_t context_capacity = 64 * 1024;  // Start with 64KB
    char* context           = (char*)malloc(context_capacity);
    if (!context) {
        send_json_error(conn, "Failed to allocate memory for context");
        return;
    }
    defer({ free(context); });

    context[0]         = '\0';
    size_t context_len = 0;
    int ntuples        = PQntuples(res);
    bool file_id_valid, page_num_valid, num_pages_valid;

    // Limit to top 20 pages for context to avoid token limits
    int context_limit = (ntuples < 20) ? ntuples : 20;

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

        const char* file_name = PQgetvalue(res, i, 1);
        const char* snippet   = PQgetvalue(res, i, 4);
        const char* full_text = PQgetvalue(res, i, 5);  // Get full text

        ASSERT(file_name);  // we must have a filename
        ASSERT(full_text);  // we must have some text

        // Build context from first N results with COMPLETE page content
        if (i < context_limit) {
            // Calculate space needed
            int header_len_int = snprintf(NULL, 0, "\n=== PAGE %d: [%s, Page %d of %d] ===\n\n",
                                          i + 1, file_name, page_num, num_pages);
            if (header_len_int < 0) {
                continue;  // snprintf error, skip this entry
            }

            size_t full_text_len = strlen(full_text);
            size_t needed =
                context_len + (size_t)header_len_int + full_text_len + 3;  // +3 for "\n\n" + null

            // Reallocate if needed
            if (needed > context_capacity) {
                size_t new_capacity = context_capacity * 2;
                while (new_capacity < needed) {
                    new_capacity *= 2;
                }
                char* new_context = (char*)realloc(context, new_capacity);
                if (!new_context) {
                    fprintf(stderr, "[PDF Search] Warning: Failed to expand context buffer\n");
                    break;  // Stop adding to context but continue with what we have
                }
                context          = new_context;
                context_capacity = new_capacity;
            }

            // Add header
            int written = snprintf(context + context_len, context_capacity - context_len,
                                   "\n=== PAGE %d: [%s, Page %d of %d] ===\n\n", i + 1,
                                   file_name ? file_name : "Unknown", page_num, num_pages);

            if (written > 0) {
                context_len += (size_t)written;

                // Add full page text
                size_t remaining = context_capacity - context_len - 1;
                if (full_text_len < remaining) {
                    memcpy(context + context_len, full_text, full_text_len);
                    context_len += full_text_len;
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

    // Get AI summary with COMPLETE page content if we have results and API key
    const char* api_key = getenv("GEMINI_API_KEY");

    // Skip searches within the book itself.
    if (api_key && ntuples > 0 && context_len > 0 && fileId == NULL) {
        fprintf(stderr,
                "[PDF Search] Info: Sending %zu characters of full page content to Gemini\n",
                context_len);
        char* ai_summary = get_ai_summary(query, context, api_key);
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
    char* cached_json = nullptr;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    pgconn_t* db = DB(conn);

    // First, get the total count of files
    const char* count_query = "SELECT COUNT(*) FROM files";
    PGresult* count_res     = pgconn_query(db, count_query, nullptr);
    if (!count_res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(count_res); });

    int64_t total_count = 0;
    if (PQntuples(count_res) > 0) {
        const char* count_str = PQgetvalue(count_res, 0, 0);
        if (count_str) {
            total_count = strtoll(count_str, nullptr, 10);
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
        query     = "SELECT id, name, path, num_pages FROM files ORDER BY name LIMIT $1 OFFSET $2";
        params[0] = limit_str;
        params[1] = offset_str;
        param_count = 2;
    }
    PGresult* res = pgconn_query_params(db, query, param_count, params, nullptr);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
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
        int64_t file_id   = strtoll(file_id_str, nullptr, 10);
        int64_t num_pages = strtoll(num_pages_str, nullptr, 10);

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

    char* json_str = yyjson_mut_write(doc, 0, nullptr);
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

    char* cached_json = nullptr;
    size_t cached_len = 0;
    if (cache_get(g_response_cache, cache_key, &cached_json, &cached_len)) {
        send_cache_json(conn, cached_json, cached_len);
        free(cached_json);
        return;
    }

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = DB(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, nullptr);
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

    int64_t num_pages = strtoll(num_pages_str, nullptr, 10);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "id", file_id);
    yyjson_mut_obj_add_str(doc, root, "name", file_name ? file_name : "");
    yyjson_mut_obj_add_str(doc, root, "path", file_path ? file_path : "");
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
