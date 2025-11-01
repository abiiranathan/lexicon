#include "cache.h"
#include <stdio.h>  // for snprintf

/** Simple hash function (FNV-1a). */
static inline uint32_t hash_key(const char* key) {
    uint32_t hash = 2166136261u;
    for (const char* p = key; *p != '\0'; p++) {
        hash ^= (uint32_t)(*p);
        hash *= 16777619u;
    }
    return hash;
}

/** Removes an entry from the LRU list. */
static inline void lru_remove(response_cache_t* cache, cache_entry_t* entry) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->tail = entry->prev;
    }

    entry->prev = nullptr;
    entry->next = nullptr;
}

/** Moves an entry to the front of the LRU list. */
static inline void lru_move_to_front(response_cache_t* cache, cache_entry_t* entry) {
    if (cache->head == entry) {
        return;  // Already at front
    }

    lru_remove(cache, entry);

    entry->next = cache->head;
    entry->prev = nullptr;

    if (cache->head) {
        cache->head->prev = entry;
    }
    cache->head = entry;

    if (!cache->tail) {
        cache->tail = entry;
    }
}

/** Adds an entry to the front of the LRU list. */
static inline void lru_add_to_front(response_cache_t* cache, cache_entry_t* entry) {
    entry->next = cache->head;
    entry->prev = nullptr;

    if (cache->head) {
        cache->head->prev = entry;
    }
    cache->head = entry;

    if (!cache->tail) {
        cache->tail = entry;
    }
}

/** Frees a cache entry and its resources. */
static inline void cache_entry_free(cache_entry_t* entry) {
    if (!entry) {
        return;
    }
    free(entry->value);
    free(entry);
}

/** Evicts the least recently used entry. */
static void evict_lru(response_cache_t* cache) {
    if (!cache->tail) {
        return;
    }

    cache_entry_t* victim = cache->tail;

    // Remove from hash table
    uint32_t hash_val = hash_key(victim->key);
    size_t bucket_idx = hash_val % cache->bucket_count;

    cache_entry_t** slot = &cache->buckets[bucket_idx];
    while (*slot && *slot != victim) {
        slot = &(*slot)->hash_next;
    }
    if (*slot) {
        *slot = victim->hash_next;
    }

    // Remove from LRU list
    lru_remove(cache, victim);

    cache_entry_free(victim);
    cache->size--;
}

response_cache_t* response_cache_create(size_t capacity, uint32_t default_ttl) {
    response_cache_t* cache = malloc(sizeof(*cache));
    if (!cache) {
        return nullptr;
    }

    *cache = (response_cache_t){0};

    cache->capacity    = capacity > 0 ? capacity : CACHE_DEFAULT_CAPACITY;
    cache->default_ttl = default_ttl > 0 ? default_ttl : CACHE_DEFAULT_TTL;

    // Use a prime number of buckets for better distribution
    cache->bucket_count = cache->capacity * 2 + 1;
    cache->buckets      = calloc(cache->bucket_count, sizeof(cache_entry_t*));
    if (!cache->buckets) {
        free(cache);
        return nullptr;
    }

    if (pthread_rwlock_init(&cache->lock, nullptr) != 0) {
        free(cache->buckets);
        free(cache);
        return nullptr;
    }

    return cache;
}

void response_cache_destroy(response_cache_t* cache) {
    if (!cache) {
        return;
    }

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t* current = cache->head;
    while (current) {
        cache_entry_t* next = current->next;
        cache_entry_free(current);
        current = next;
    }

    free(cache->buckets);
    pthread_rwlock_unlock(&cache->lock);
    pthread_rwlock_destroy(&cache->lock);
    free(cache);
}

bool response_cache_get(response_cache_t* cache, const char* key, char** out_value, size_t* out_len) {
    if (!cache || !key || !out_value || !out_len) {
        return false;
    }

    pthread_rwlock_wrlock(&cache->lock);  // Write lock for LRU update

    uint32_t hash_val = hash_key(key);
    size_t bucket_idx = hash_val % cache->bucket_count;

    cache_entry_t* entry = cache->buckets[bucket_idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            // Check expiration
            time_t now = time(nullptr);
            if (now >= entry->expires_at) {
                // Expired - remove it
                pthread_rwlock_unlock(&cache->lock);
                response_cache_invalidate(cache, key);
                return false;
            }

            // Found and valid - duplicate value for caller
            *out_value = malloc(entry->value_len);
            if (!*out_value) {
                pthread_rwlock_unlock(&cache->lock);
                return false;
            }
            memcpy(*out_value, entry->value, entry->value_len);
            *out_len = entry->value_len;

            // Move to front of LRU
            lru_move_to_front(cache, entry);

            pthread_rwlock_unlock(&cache->lock);
            return true;
        }
        entry = entry->hash_next;
    }

    pthread_rwlock_unlock(&cache->lock);
    return false;
}

bool response_cache_set(response_cache_t* cache, const char* key, const unsigned char* value, size_t value_len,
                        uint32_t ttl_override) {
    if (!cache || !key || !value || key[0] == '\0') {
        return false;
    }

    // Check key length
    size_t key_len = strlen(key);
    if (key_len >= CACHE_KEY_MAX_LEN) {
        return false;
    }

    pthread_rwlock_wrlock(&cache->lock);

    uint32_t hash_val = hash_key(key);
    size_t bucket_idx = hash_val % cache->bucket_count;

    // Check if key already exists
    cache_entry_t* existing = cache->buckets[bucket_idx];
    while (existing) {
        if (strcmp(existing->key, key) == 0) {
            // Update existing entry.
            // We don't want to add null terminator as this would break binary data.
            unsigned char* new_value = malloc(value_len);
            if (!new_value) {
                pthread_rwlock_unlock(&cache->lock);
                return false;
            }
            memcpy(new_value, value, value_len);

            free(existing->value);
            existing->value     = new_value;
            existing->value_len = value_len;

            uint32_t ttl         = ttl_override > 0 ? ttl_override : cache->default_ttl;
            existing->expires_at = time(nullptr) + ttl;

            lru_move_to_front(cache, existing);

            pthread_rwlock_unlock(&cache->lock);
            return true;
        }
        existing = existing->hash_next;
    }

    // Evict if at capacity
    if (cache->size >= cache->capacity) {
        evict_lru(cache);
    }

    // Create new entry
    cache_entry_t* entry = malloc(sizeof(*entry));
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }

    *entry = (cache_entry_t){0};
    strncpy(entry->key, key, CACHE_KEY_MAX_LEN - 1);
    entry->key[CACHE_KEY_MAX_LEN - 1] = '\0';

    entry->value = malloc(value_len);
    if (!entry->value) {
        free(entry);
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }
    memcpy(entry->value, value, value_len);
    entry->value_len = value_len;

    uint32_t ttl      = ttl_override > 0 ? ttl_override : cache->default_ttl;
    entry->expires_at = time(nullptr) + ttl;

    // Add to hash table
    entry->hash_next           = cache->buckets[bucket_idx];
    cache->buckets[bucket_idx] = entry;

    // Add to LRU list
    lru_add_to_front(cache, entry);

    cache->size++;

    pthread_rwlock_unlock(&cache->lock);
    return true;
}

void response_cache_invalidate(response_cache_t* cache, const char* key) {
    if (!cache || !key) {
        return;
    }

    pthread_rwlock_wrlock(&cache->lock);

    uint32_t hash_val = hash_key(key);
    size_t bucket_idx = hash_val % cache->bucket_count;

    cache_entry_t** slot = &cache->buckets[bucket_idx];
    while (*slot) {
        if (strcmp((*slot)->key, key) == 0) {
            cache_entry_t* victim = *slot;
            *slot                 = victim->hash_next;
            lru_remove(cache, victim);
            cache_entry_free(victim);
            cache->size--;
            break;
        }
        slot = &(*slot)->hash_next;
    }

    pthread_rwlock_unlock(&cache->lock);
}

void response_cache_clear(response_cache_t* cache) {
    if (!cache) {
        return;
    }

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t* current = cache->head;
    while (current) {
        cache_entry_t* next = current->next;
        cache_entry_free(current);
        current = next;
    }

    // Clear hash table buckets
    for (size_t i = 0; i < cache->bucket_count; i++) {
        cache->buckets[i] = nullptr;
    }

    cache->head = nullptr;
    cache->tail = nullptr;
    cache->size = 0;

    pthread_rwlock_unlock(&cache->lock);
}

int response_cache_make_key(char* buffer, size_t buffer_size, int64_t file_id, int page_num) {
    if (!buffer || buffer_size == 0) {
        return 0;
    }

    if (page_num >= 0) {
        return snprintf(buffer, buffer_size, "file:%lld:page:%d", (long long)file_id, page_num);
    } else {
        return snprintf(buffer, buffer_size, "file:%lld", (long long)file_id);
    }
}
