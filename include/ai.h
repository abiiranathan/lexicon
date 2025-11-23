
#ifndef AI_H
#define AI_H

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calls Gemini API to generate AI summary for search results.
 * Optimized for direct, concise answers that prioritize the actual question.
 * @param query The search query
 * @param context The context from search results (extended snippets)
 * @param api_key The Gemini API key
 * @param is_cached Bool pointer that will be set to true if returned poiinter
 * if of a cached response. In this case, you MUST call deref_ai_response() to
 * decrement the ref count in the underlying cache.
 * @return Allocated string with AI summary (caller must free), or NULL on error
 */
char* get_ai_summary(const char* query, const char* context, const char* api_key, bool* is_cached);

/**
 * Initializes the AI cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_ai_cache(size_t capacity, uint32_t ttl_seconds);

// Release response pointer back to the cache.
void deref_ai_response(char* response);

/**
 * Destroys the global response cache.
 * Call this at application shutdown.
 */
void destroy_ai_cache(void);

#ifdef __cplusplus
}
#endif

#endif  // AI_H
