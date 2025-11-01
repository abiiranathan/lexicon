#ifndef RESPONSE_CACHE_H
#define RESPONSE_CACHE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Maximum cache key length. */
#define CACHE_KEY_MAX_LEN 256

/** Default cache capacity (number of entries). */
#define CACHE_DEFAULT_CAPACITY 1000

/** Default TTL in seconds (5 minutes). */
#define CACHE_DEFAULT_TTL 300

/** Represents a cached response entry. */
typedef struct cache_entry {
    char key[CACHE_KEY_MAX_LEN];    // Cache key
    unsigned char* value;           // Cached JSON response (heap-allocated). No null-termination guarantee.
    size_t value_len;               // Length of cached value
    time_t expires_at;              // Expiration timestamp
    struct cache_entry* prev;       // LRU list previous
    struct cache_entry* next;       // LRU list next
    struct cache_entry* hash_next;  // Hash collision chain
} cache_entry_t;

/** Response cache with LRU eviction. Thread-safe. */
typedef struct {
    cache_entry_t** buckets;  // Hash table buckets
    size_t bucket_count;      // Number of hash buckets
    cache_entry_t* head;      // LRU list head (most recent)
    cache_entry_t* tail;      // LRU list tail (least recent)
    size_t capacity;          // Maximum number of entries
    size_t size;              // Current number of entries
    uint32_t default_ttl;     // Default TTL in seconds
    pthread_rwlock_t lock;    // Read-write lock for thread safety
} response_cache_t;

/**
 * Creates a new response cache.
 * @param capacity Maximum number of cache entries (0 uses default).
 * @param default_ttl Default time-to-live in seconds (0 uses default).
 * @return Pointer to new cache on success, nullptr on failure.
 * @note Caller must free with response_cache_destroy().
 */
response_cache_t* response_cache_create(size_t capacity, uint32_t default_ttl);

/**
 * Destroys a response cache and frees all resources.
 * @param cache The cache to destroy. Safe to pass nullptr.
 */
void response_cache_destroy(response_cache_t* cache);

/**
 * Retrieves a cached response.
 * @param cache The cache to query.
 * @param key The cache key.
 * @param out_value Pointer to store the cached value (caller must free). The data is not null terminated.
 * @param out_len Pointer to store the value length.
 * @return true if found and not expired, false otherwise.
 * @note If true is returned, caller must free *out_value.
 */
bool response_cache_get(response_cache_t* cache, const char* key, char** out_value, size_t* out_len);

/**
 * Stores a response in the cache.
 * @param cache The cache to store in.
 * @param key The cache key.
 * @param value The value to cache (will be copied).
 * @param value_len Length of the value.
 * @param ttl_override Time-to-live override (0 uses default).
 * @return true on success, false on failure.
 */
bool response_cache_set(response_cache_t* cache, const char* key, const unsigned char* value, size_t value_len,
                        uint32_t ttl_override);

/**
 * Invalidates a cache entry.
 * @param cache The cache to modify.
 * @param key The cache key to invalidate.
 */
void response_cache_invalidate(response_cache_t* cache, const char* key);

/**
 * Clears all entries from the cache.
 * @param cache The cache to clear.
 */
void response_cache_clear(response_cache_t* cache);

/**
 * Generates a cache key for file/page lookups.
 * @param buffer Buffer to store the key.
 * @param buffer_size Size of the buffer.
 * @param file_id File ID.
 * @param page_num Page number (use -1 for file-only keys).
 * @return Number of characters written (excluding null terminator).
 */
int response_cache_make_key(char* buffer, size_t buffer_size, int64_t file_id, int page_num);

#endif  // RESPONSE_CACHE_H
