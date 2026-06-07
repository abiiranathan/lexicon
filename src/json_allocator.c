#include "../include/json_allocator.h"

/* ---------- shim callbacks ---------- */

/**
 * Arena malloc shim for yyjson.
 * @param ctx  Pointer to the owning Arena.
 * @param size Number of bytes requested.
 * @return Pointer to allocated block, or NULL on failure.
 */
static void* arena_yyjson_malloc(void* ctx, size_t size) {
    return arena_alloc((Arena*)ctx, size);
}

/**
 * Arena realloc shim for yyjson.
 *
 * Arenas cannot reclaim individual allocations, so this allocates a fresh
 * block and copies the existing data. The old block is leaked inside the
 * arena and reclaimed only when the arena is destroyed.
 *
 * @param ctx      Pointer to the owning Arena.
 * @param ptr      Previously allocated block (may be NULL).
 * @param old_size Byte count of the existing block.
 * @param size     Byte count of the new block.
 * @return Pointer to new block, or NULL on failure.
 */
static void* arena_yyjson_realloc(void* ctx, void* ptr, size_t old_size, size_t size) {
    void* new_ptr = arena_alloc((Arena*)ctx, size);
    if (new_ptr == NULL) { return NULL; }
    if (ptr != NULL) { memcpy(new_ptr, ptr, old_size < size ? old_size : size); }
    return new_ptr;
}

/**
 * Arena free shim for yyjson — intentional no-op.
 *
 * Memory is reclaimed in bulk when the arena is destroyed. Individual
 * frees are not supported by the bump-allocator model.
 *
 * @param ctx Pointer to the owning Arena (unused).
 * @param ptr Block to "free" (unused).
 */
static void arena_yyjson_free(void* ctx, void* ptr) {
    (void)ctx;
    (void)ptr;
}

/* ---------- public helper ---------- */

/**
 * Initialises a yyjson_alc that delegates all allocations to @p arena.
 *
 * The arena must outlive every yyjson document created with this allocator.
 * Call arena_destroy() after all documents have been freed (or simply when
 * the arena's lifetime ends).
 *
 * @param alc   Output parameter — populated on return.
 * @param arena The arena to back all yyjson allocations.
 */
void yyjson_alc_from_arena(yyjson_alc* alc, Arena* arena) {
    alc->malloc = arena_yyjson_malloc;
    alc->realloc = arena_yyjson_realloc;
    alc->free = arena_yyjson_free;
    alc->ctx = arena;
}
