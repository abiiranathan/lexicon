#ifndef LEXICON_AUTH_H
#define LEXICON_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#include <pulsar/pulsar.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validates the session cookie and loads the caller's admin flag.
 *
 * Sends 401 when no (valid) session exists, 403 when the user is not an
 * admin. On success stores the user id and returns true.
 */
bool auth_require_admin(PulsarConn* conn, int64_t* out_user_id);

/**
 * Validates the session cookie for any authenticated user (admin or not).
 *
 * Sends 401 when no (valid) session exists. On success stores the user id
 * and returns true.
 */
bool auth_require_login(PulsarConn* conn, int64_t* out_user_id);

/** GET /api/auth/providers — which OAuth providers are configured. */
void auth_providers(PulsarCtx* ctx);

/** GET /api/auth/login/{provider} — starts the OAuth flow (302 redirect). */
void auth_login(PulsarCtx* ctx);

/** GET /api/auth/callback/{provider} — finishes the OAuth flow. */
void auth_callback(PulsarCtx* ctx);

/** POST /api/auth/logout — clears the session. */
void auth_logout(PulsarCtx* ctx);

/** GET /api/auth/whoami — current session user or 401. */
void auth_whoami(PulsarCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif  // LEXICON_AUTH_H
