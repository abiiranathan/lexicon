#ifndef LEXICON_VFS_STORE_H
#define LEXICON_VFS_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

// The server mounts its VFS image once and serves every page render and text
// request from that same handle. VFS path lookups resolve against the
// handle's in-memory index, so a file imported later (e.g. an admin upload)
// is invisible until the handle is replaced with a freshly opened one.
//
// Discipline: serve-time reads MUST go through vfs_store_read_file, which
// takes the read side of the lock. vfs_store_replace swaps in a fresh handle
// under the write side and closes the old one, so in-flight readers can
// never observe a freed handle. The raw `vfs` global stays visible only for
// single-threaded batch contexts (CLI indexer, forked index children).

/** Read-locked vfs_read_file on the current server handle. */
void* vfs_store_read_file(const char* path, size_t* out_size);

/** True when a VFS image is currently mounted (read-locked check). */
bool vfs_store_active(void);

/**
 * Installs a freshly opened handle, closing the previous one.
 * Returns false (keeping the old handle) when fresh is NULL.
 */
bool vfs_store_replace(vfs_t* fresh);

/** Closes the current handle (process shutdown). */
void vfs_store_close(void);

#ifdef __cplusplus
}
#endif

#endif  // LEXICON_VFS_STORE_H
