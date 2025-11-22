#include "../include/ai.h"
#include <curl/curl.h>
#include <solidc/cache.h>
#include <solidc/defer.h>
#include <solidc/macros.h>
#include <stdio.h>
#include <yyjson.h>

// In-memory cache for AI responses.
static cache_t* ai_response_cache = NULL;

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
 * Initializes the AI cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_ai_cache(size_t capacity, uint32_t ttl_seconds) {
    ai_response_cache = cache_create(capacity, ttl_seconds);
    return ai_response_cache != NULL;
}

/**
 * Destroys the AI cache.
 * Call this at application shutdown.
 */
void destroy_ai_cache(void) {
    cache_destroy(ai_response_cache);
    ai_response_cache = NULL;
}

/**
 * Builds the prompt for Gemini API with query and context.
 * @param query The search query
 * @param context The context from search results
 * @return Allocated prompt string (caller must free), or NULL on error
 */
static char* _build_gemini_prompt(const char* query, const char* context, size_t context_len) {
    size_t prompt_size = context_len + 3072;  // 3K for static prompt
    char* prompt       = (char*)malloc(prompt_size);
    if (!prompt) {
        LOG_ERROR("Failed to allocate memory for prompt");
        return NULL;
    }

    snprintf(prompt, prompt_size,
             "You are an expert AI assistant helping users find information about their query. Queries "
             "are mostly about Medical and Programming topics. "
             "Use your comprehensive knowledge to provide accurate answers. "
             "PDF page excerpts are provided below as additional context to supplement your response.\n\n"
             "USER QUERY: \"%s\"\n\n"
             "SUPPLEMENTARY PDF CONTEXT:\n"
             "%s\n\n"
             "CRITICAL RESPONSE RULES:\n"
             "1. ANSWER THE EXACT QUESTION ASKED - Be direct and specific\n"
             "2. For specific questions (dosing, definitions, procedures):\n"
             "   - Lead with the DIRECT ANSWER in the first sentence or paragraph\n"
             "   - Make the key information prominent and easy to find\n"
             "   - Add supporting details AFTER the main answer\n"
             "3. For broad questions (\"tell me about X\", \"explain Y\"):\n"
             "   - Provide comprehensive coverage\n"
             "   - Include multiple aspects and details\n"
             "4. DOSING/MEDICATION QUERIES - Answer format:\n"
             "   - Start with the exact regimen: \"For [condition], the dosing is: [specific regimen]\"\n"
             "   - Include dose, route, frequency, and duration in the first paragraph\n"
             "   - Then add important considerations (contraindications, monitoring, alternatives)\n"
             "   - Keep additional context brief unless specifically requested\n"
             "5. Do NOT bury the answer in background information\n"
             "6. Do NOT provide extensive context before answering the question\n"
             "7. Synthesize information from both your knowledge and the PDF excerpts\n"
             "8. If PDF content is incomplete, supplement with your expert knowledge\n"
             "9. Be accurate and cite sources when using specific PDF information\n\n"
             "OUTPUT FORMAT REQUIREMENTS:\n"
             "- Use ONLY valid HTML tags: <p>, <ul>, <li>, <ol>, <h3>, <h4>, <b>, <strong>, <em>, "
             "<i>, <br>\n"
             "- Output ONLY raw HTML - NO markdown syntax\n"
             "- Do NOT use code fences (```html or ```)\n"
             "- Do NOT use markdown bold (**text**) - use <b>text</b> or <strong>text</strong>\n"
             "- Do NOT use markdown italics (*text*) - use <i>text</i> or <em>text</em>\n"
             "- Do NOT use markdown headers (# or ##) - use <h3> or <h4> tags\n"
             "- Start immediately with an HTML tag (like <h3> or <p>)\n"
             "- For specific questions, use <p> tags with <b> for key information\n"
             "- For broader topics, you may use <h3> for sections\n\n"
             "RESPONSE LENGTH:\n"
             "- Specific questions: 10-20 sentences focused on the answer\n"
             "- Broad questions: 50-100 sentences with comprehensive coverage\n"
             "- Medical treatment protocols: Complete but prioritize the core regimen first\n\n"
             "Your response must be pure HTML that directly answers the user's question.",
             query, context);

    return prompt;
}

/**
 * Builds the JSON request body for Gemini API.
 * @param prompt The prompt to send to Gemini
 * @return Allocated JSON string (caller must free), or NULL on error
 */
static char* _build_gemini_request(const char* prompt) {
    yyjson_mut_doc* req_doc = yyjson_mut_doc_new(NULL);
    if (!req_doc) {
        LOG_ERROR("Failed to create JSON document");
        return NULL;
    }

    yyjson_mut_val* req_root = yyjson_mut_obj(req_doc);
    yyjson_mut_doc_set_root(req_doc, req_root);

    yyjson_mut_val* contents_arr = yyjson_mut_arr(req_doc);
    yyjson_mut_val* content_obj  = yyjson_mut_obj(req_doc);
    yyjson_mut_val* parts_arr    = yyjson_mut_arr(req_doc);
    yyjson_mut_val* part_obj     = yyjson_mut_obj(req_doc);

    yyjson_mut_obj_add_str(req_doc, part_obj, "text", prompt);
    yyjson_mut_arr_append(parts_arr, part_obj);
    yyjson_mut_obj_add_val(req_doc, content_obj, "parts", parts_arr);
    yyjson_mut_arr_append(contents_arr, content_obj);
    yyjson_mut_obj_add_val(req_doc, req_root, "contents", contents_arr);

    char* json_body = yyjson_mut_write(req_doc, 0, NULL);
    yyjson_mut_doc_free(req_doc);

    return json_body;
}

/**
 * Makes HTTP request to Gemini API.
 * @param url The Gemini API URL
 * @param json_body The JSON request body
 * @param response Pointer to curl_response_t to store response
 * @return true on success (HTTP 200), false on error
 */
static bool _call_gemini_api(const char* url, const char* json_body, curl_response_t* response) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize curl");
        return false;
    }

    struct curl_slist* headers = NULL;
    headers                    = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res   = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("curl_easy_perform() failed: %s (code: %d)\n", curl_easy_strerror(res), res);
        return false;
    }

    if (http_code != 200) {
        LOG_ERROR("HTTP request failed with status code: %ld\n", http_code);
        if (response->data && response->size > 0) {
            LOG_ERROR("[AI Summary] Error response body: %.*s\n", (int)(response->size > 500 ? 500 : response->size),
                      response->data);
        }
        return false;
    }
    return true;
}

/**
 * Parses Gemini API response and extracts the summary text.
 * @param response_data The raw JSON response
 * @param response_size Size of the response data
 * @return Allocated summary string (caller must free), or NULL on error
 */
static char* _parse_gemini_response(const char* response_data, size_t response_size) {
    if (!response_data || response_size == 0) {
        LOG_ERROR("Empty response from API");
        return NULL;
    }

    LOG_ERROR("[AI Summary] Info: Received response (%zu bytes)\n", response_size);

    yyjson_doc* resp_doc = yyjson_read(response_data, response_size, 0);
    if (!resp_doc) {
        LOG_ERROR("Failed to parse JSON response");
        return NULL;
    }
    defer({ yyjson_doc_free(resp_doc); });

    yyjson_val* root = yyjson_doc_get_root(resp_doc);
    if (!root) {
        LOG_ERROR("No root object in JSON response");
        return NULL;
    }

    // Check for error in response
    yyjson_val* error_obj = yyjson_obj_get(root, "error");
    if (error_obj) {
        yyjson_val* error_msg = yyjson_obj_get(error_obj, "message");
        const char* error_str = yyjson_get_str(error_msg);
        LOG_ERROR("API returned error: %s\n", error_str ? error_str : "Unknown error");
        return NULL;
    }

    // Navigate to the text field
    yyjson_val* candidates = yyjson_obj_get(root, "candidates");
    if (!candidates || !yyjson_is_arr(candidates)) {
        LOG_ERROR("Invalid 'candidates' field in response");
        return NULL;
    }

    yyjson_val* first_candidate = yyjson_arr_get_first(candidates);
    if (!first_candidate) {
        LOG_ERROR("'candidates' array is empty");
        return NULL;
    }

    yyjson_val* content = yyjson_obj_get(first_candidate, "content");
    if (!content) {
        LOG_ERROR("No 'content' field in candidate");
        return NULL;
    }

    yyjson_val* parts = yyjson_obj_get(content, "parts");
    if (!parts || !yyjson_is_arr(parts)) {
        LOG_ERROR("Invalid 'parts' field in content");
        return NULL;
    }

    yyjson_val* first_part = yyjson_arr_get_first(parts);
    if (!first_part) {
        LOG_ERROR("'parts' array is empty");
        return NULL;
    }

    yyjson_val* text = yyjson_obj_get(first_part, "text");
    if (!text || !yyjson_is_str(text)) {
        LOG_ERROR("Invalid 'text' field in part");
        return NULL;
    }

    const char* summary_text = yyjson_get_str(text);
    if (!summary_text) {
        LOG_ERROR("Failed to extract text string");
        return NULL;
    }
    return strdup(summary_text);
}

/**
 * Calls Gemini API to generate AI summary for search results.
 * Optimized for direct, concise answers that prioritize the actual question.
 * @param query The search query
 * @param context The context from search results (extended snippets)
 * @param api_key The Gemini API key
 * @return Allocated string with AI summary (caller must free), or NULL on error
 */
char* get_ai_summary(const char* query, const char* context, const char* api_key) {
    if (!query || !context || !api_key) {
        LOG_ERROR("Invalid arguments");
        return NULL;
    }

    // Check the cache first
    char* gemini_resp = NULL;
    if (ai_response_cache) {
        size_t outlen = 0;
        if (cache_get(ai_response_cache, query, &gemini_resp, &outlen)) {
            return gemini_resp;
        }
    }

    // Get model name (allow override via env var)
    const char* gemini_model = getenv("GEMINI_MODEL");
    if (!gemini_model || strcmp(gemini_model, "") == 0) {
        gemini_model = "gemini-2.0-flash";  // Mostly available. 2.5-flash is always 503 Unavailable
    }

    // Build the API URL
    char url[512];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/"
             "%s:generateContent?key=%s",
             gemini_model, api_key);

    size_t context_size = strlen(context);

    LOG_ERROR("[AI Summary] Info: Sending request to Gemini API for query: \"%s\"\n", query);
    LOG_ERROR("[AI Summary] Info: Context size: %zu characters\n", context_size);

    // Build the prompt
    char* prompt = _build_gemini_prompt(query, context, context_size);
    if (!prompt) {
        return NULL;
    }
    defer({ free(prompt); });

    // Build JSON request
    char* json_body = _build_gemini_request(prompt);
    if (!json_body) {
        LOG_ERROR("Failed to build JSON request");
        return NULL;
    }
    defer({ free(json_body); });

    // Make API call
    curl_response_t response = {0};
    if (!_call_gemini_api(url, json_body, &response)) {
        free(response.data);
        return NULL;
    }
    defer({ free(response.data); });

    // Parse response
    gemini_resp = _parse_gemini_response(response.data, response.size);

    // Cache the response
    if (gemini_resp && ai_response_cache) {
        if (!cache_set(ai_response_cache, query, (uint8_t*)gemini_resp, strlen(gemini_resp), 0)) {
            LOG_ERROR("Caching AI response failed");
        }
    }

    return gemini_resp;
}
