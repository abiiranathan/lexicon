#ifndef LEXICON_UPLOAD_H
#define LEXICON_UPLOAD_H

#include <pulsar/pulsar.h>

#ifdef __cplusplus
extern "C" {
#endif

/** POST /api/admin/upload/init — starts a chunked PDF upload (admin only). */
void upload_init(PulsarCtx* ctx);

/** POST /api/admin/upload/chunk?job=<id>&offset=<n> — appends raw bytes. */
void upload_chunk(PulsarCtx* ctx);

/** POST /api/admin/upload/complete?job=<id> — validates and queues indexing. */
void upload_complete(PulsarCtx* ctx);

/** GET /api/admin/upload/jobs/{job_id} — upload/index job status. */
void upload_job_status(PulsarCtx* ctx);

/** Starts the background indexing worker. Serve mode only. */
void upload_system_init(void);

/** Stops the background indexing worker. */
void upload_system_shutdown(void);

/** Upload base directory: UPLOAD_DIR, falling back to the library root. */
const char* app_upload_dir(void);

/** VFS image path when serving from an image, else NULL. */
const char* app_vfs_path(void);

/** Minimum PDF pages for indexing (server configuration). */
int app_min_pages(void);

/** Effective Postgres connection string (server configuration). */
const char* app_pgconn(void);

#ifdef __cplusplus
}
#endif

#endif  // LEXICON_UPLOAD_H
