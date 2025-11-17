
#ifndef AI_H
#define AI_H

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Calls Gemini API to generate AI summary for search results.
 * Enhanced with better prompting to allow Gemini to provide comprehensive answers.
 * @param query The search query
 * @param context The context from search results (complete page content)
 * @param api_key The Gemini API key
 * @return Allocated string with AI summary (caller must free), or NULL on error
 */
char* ai_get_answer(const char* query, const char* context, const char* api_key);

/**
 * Initializes the AI cache.
 * Call this once at application startup.
 * @param capacity Maximum cache entries (0 for default 1000).
 * @param ttl_seconds Time-to-live in seconds (0 for default 300).
 * @return true on success, false on failure.
 */
bool init_ai_cache(size_t capacity, uint32_t ttl_seconds);

/**
 * Destroys the global response cache.
 * Call this at application shutdown.
 */
void destroy_ai_cache(void);

#ifdef __cplusplus
}
#endif

#endif  // AI_H
