#ifndef PDF_SEARCH_ROUTES_H
#define PDF_SEARCH_ROUTES_H

#include <pulsar/pulsar.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the global response cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_response_cache(size_t capacity, uint32_t ttl_seconds);

/**
 * Destroys the global response cache.
 * Call this at application shutdown.
 */
void destroy_response_cache(void);

/**
 * Calls Gemini API to generate AI summary for search results.
 * Enhanced with better prompting to allow Gemini to provide comprehensive answers.
 * Set GEMINI_MODEL environment variable to customize the model.
 * @param query The search query
 * @param context The context from search results (complete page content)
 * @param api_key The Gemini API key
 * @return Allocated string with AI summary (caller must free), or NULL on error
 */
char* get_ai_summary(const char* query, const char* context, const char* api_key);

/**
 * Returns full text for a given file_id and page_num.
 * Path: /api/file/{file_id}/page/{page_num}
 */
void get_page_by_file_and_page(PulsarCtx* ctx);

// Renders a PDF page as PNG image.
void render_pdf_page_as_png(PulsarCtx* ctx);

/**
 * Performs full-text search on PDF pages.
 * Returns matching results with snippets, ranked by relevance.
 * Fetches complete page content and sends to Gemini for AI-powered answers.
 */
void pdf_search(PulsarCtx* ctx);

/**
 * Lists all PDF files in the database paginated on query params "page" & limit.
 * Returns JSON object with keys: "page", "limit", "results", "has_next", "has_prev","total_count",
 * "total_pages".
 */
void list_files(PulsarCtx* ctx);

/** Retrieves a single file by its ID. */
void get_file_by_id(PulsarCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif  // PDF_SEARCH_ROUTES_H
