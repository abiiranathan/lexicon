#include <solidc/arena.h>  // for Arena, arena_alloc, arena_destroy
#include <stddef.h>        // for size_t, NULL
#include <string.h>        // for memcpy
#include <yyjson.h>        // for yyjson_alc, yyjson_read_opts, etc.

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
void yyjson_alc_from_arena(yyjson_alc* alc, Arena* arena);