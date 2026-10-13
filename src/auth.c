#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <pulsar/error.h>
#include <solidc/defer.h>
#include <yyjson.h>

#include "../include/auth.h"
#include "../include/database.h"
#include "../include/error_macros.h"
#include "../include/json_allocator.h"
#include "../include/json_response.h"

#define SESSION_COOKIE "lexicon_session"
#define SESSION_TOKEN_BYTES 32
#define SESSION_TOKEN_LEN 43 // base64url(32 bytes), no padding
#define OAUTH_STATE_BYTES 32
#define OAUTH_STATE_HEXLEN 64
#define OAUTH_STATE_TTL_SECS 600
#define OAUTH_HTTP_TIMEOUT_SECS 20
#define OAUTH_HTTP_MAX_BYTES (2u * 1024u * 1024u)
#define DEFAULT_SESSION_TTL_DAYS 30

typedef struct {
    const char* name;
    const char* id_env;
    const char* secret_env;
    const char* scope;
    const char* auth_url;
    const char* token_url;
    const char* userinfo_url;
    const char* emails_url; // optional second lookup (GitHub)
    bool pkce_in_token;     // send code_verifier (both providers support it)
    bool grant_in_token;    // send grant_type=authorization_code (Google only)
} oauth_provider_def_t;

static const oauth_provider_def_t OAUTH_PROVIDERS[] = {
    {
        .name = "google",
        .id_env = "GOOGLE_CLIENT_ID",
        .secret_env = "GOOGLE_CLIENT_SECRET",
        .scope = "openid email profile",
        .auth_url = "https://accounts.google.com/o/oauth2/v2/auth",
        .token_url = "https://oauth2.googleapis.com/token",
        .userinfo_url = "https://openidconnect.googleapis.com/v1/userinfo",
        .emails_url = NULL,
        .pkce_in_token = true,
        .grant_in_token = true,
    },
    {
        .name = "github",
        .id_env = "GITHUB_CLIENT_ID",
        .secret_env = "GITHUB_CLIENT_SECRET",
        .scope = "read:user user:email",
        .auth_url = "https://github.com/login/oauth/authorize",
        .token_url = "https://github.com/login/oauth/access_token",
        .userinfo_url = "https://api.github.com/user",
        .emails_url = "https://api.github.com/user/emails",
        .pkce_in_token = true,
        .grant_in_token = false,
    },
};
#define OAUTH_PROVIDER_COUNT (sizeof(OAUTH_PROVIDERS) / sizeof(OAUTH_PROVIDERS[0]))

static const oauth_provider_def_t* provider_by_name(const char* name) {
    for (size_t i = 0; i < OAUTH_PROVIDER_COUNT; i++) {
        if (strcmp(OAUTH_PROVIDERS[i].name, name) == 0) return &OAUTH_PROVIDERS[i];
    }
    return NULL;
}

static const char* provider_client_id(const oauth_provider_def_t* def) {
    const char* id = getenv(def->id_env);
    return (id && *id) ? id : NULL;
}

static const char* provider_client_secret(const oauth_provider_def_t* def) {
    const char* secret = getenv(def->secret_env);
    return (secret && *secret) ? secret : NULL;
}

static bool provider_configured(const oauth_provider_def_t* def) {
    return provider_client_id(def) && provider_client_secret(def);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool random_bytes(uint8_t* out, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(out + done, len - done, 0);
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static void base64url_encode(const uint8_t* in, size_t in_len, char* out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < in_len;) {
        uint32_t a = in[i++];
        uint32_t b = i < in_len ? in[i++] : 0;
        uint32_t c = i < in_len ? in[i++] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out[o++] = alphabet[(triple >> 18) & 63];
        out[o++] = alphabet[(triple >> 12) & 63];
        if (o < ((in_len * 4 + 2) / 3)) out[o++] = alphabet[(triple >> 6) & 63];
        if (o < ((in_len * 4 + 2) / 3)) out[o++] = alphabet[triple & 63];
    }
    out[o] = '\0';
}

static bool sha256_hex(const char* input, char out_hex[65]) {
    uint8_t digest[32];
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(ctx, input, strlen(input)) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &out_len) == 1 && out_len == 32;
    EVP_MD_CTX_free(ctx);
    if (!ok) return false;
    for (unsigned i = 0; i < 32; i++) snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    return true;
}

static size_t url_encode_into(const char* in, char* out, size_t out_cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
        const unsigned char c = *p;
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            if (o + 1 >= out_cap) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_cap) return 0;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (o >= out_cap) return 0;
    out[o] = '\0';
    return o;
}

static void ascii_lower_inplace(char* s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// True when email (case-insensitive) appears in the ADMIN_EMAILS allowlist.
static bool admin_allowlisted(const char* email) {
    const char* list = getenv("ADMIN_EMAILS");
    if (!list || !*list || !email || !*email) return false;
    char* copy = strdup(list);
    if (!copy) return false;
    defer { free(copy); };
    char lowered[320];
    snprintf(lowered, sizeof(lowered), "%s", email);
    ascii_lower_inplace(lowered);
    bool found = false;
    char* save = NULL;
    for (char* entry = strtok_r(copy, ",", &save); entry; entry = strtok_r(NULL, ",", &save)) {
        while (*entry == ' ' || *entry == '\t') entry++;
        char* end = entry + strlen(entry);
        while (end > entry && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) *--end = '\0';
        if (*entry == '"' || *entry == '\'') entry++;
        size_t len = strlen(entry);
        if (len && (entry[len - 1] == '"' || entry[len - 1] == '\'')) entry[len - 1] = '\0';
        if (!*entry) continue;
        char candidate[320];
        snprintf(candidate, sizeof(candidate), "%s", entry);
        ascii_lower_inplace(candidate);
        if (strcmp(candidate, lowered) == 0) {
            found = true;
            break;
        }
    }
    return found;
}

static int session_ttl_secs(void) {
    const char* raw = getenv("SESSION_TTL_DAYS");
    long days = DEFAULT_SESSION_TTL_DAYS;
    if (raw && *raw) {
        char* end = NULL;
        const long parsed = strtol(raw, &end, 10);
        if (end != raw && parsed >= 1 && parsed <= 365) days = parsed;
    }
    return (int)(days * 86400L);
}

// Reads the session token from the Cookie request header. Returns false when
// absent or malformed. Token output always fits SESSION_TOKEN_LEN+1.
static bool request_session_token(PulsarConn* conn, char out[SESSION_TOKEN_LEN + 1]) {
    const headers_t* headers = req_headers(conn);
    if (!headers) return false;
    const StrSlice cookie = headers_get(headers, "Cookie");
    if (!cookie.data || !cookie.len) return false;
    const char* needle = SESSION_COOKIE "=";
    const size_t needle_len = strlen(needle);
    const char* end = cookie.data + cookie.len;
    for (const char* p = cookie.data; p + needle_len <= end; p++) {
        if (memcmp(p, needle, needle_len) != 0) continue;
        // Match a full cookie name: start-of-header, ';' or space before it.
        if (p != cookie.data && p[-1] != ';' && p[-1] != ' ') continue;
        const char* value = p + needle_len;
        const char* stop = value;
        while (stop < end && *stop != ';' && *stop != ' ' && *stop != '\r' && *stop != '\n') stop++;
        const size_t len = (size_t)(stop - value);
        if (len != SESSION_TOKEN_LEN) return false;
        for (size_t i = 0; i < len; i++) {
            const char c = value[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return false;
        }
        memcpy(out, value, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

static void set_session_cookie(PulsarConn* conn, const char* token, int max_age_secs) {
    bool secure = false;
    const char* flag = getenv("APP_SECURE_COOKIES");
    if (flag && (*flag == '1' || *flag == 't' || *flag == 'T')) {
        secure = true;
    } else if (!flag) {
        const char* base = getenv("APP_BASE_URL");
        secure = base && strncmp(base, "https://", 8) == 0;
    }
    char header[512];
    snprintf(header, sizeof(header),
             "Set-Cookie: " SESSION_COOKIE
             "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s\r\n",
             token, max_age_secs, secure ? "; Secure" : "");
    conn_writeheader_raw(conn, header, strlen(header));
}

static void clear_session_cookie(PulsarConn* conn) {
    conn_writeheader_raw(conn,
                         "Set-Cookie: " SESSION_COOKIE
                         "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n",
                         strlen("Set-Cookie: " SESSION_COOKIE
                                "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n"));
}

/* ------------------------------------------------------------------ */
/* Sessions in PostgreSQL (only token hashes are stored)               */
/* ------------------------------------------------------------------ */

static bool session_lookup(PulsarConn* conn, int64_t* out_user_id, bool* out_is_admin,
                           char email_buf[320]) {
    char token[SESSION_TOKEN_LEN + 1];
    if (!request_session_token(conn, token)) return false;
    char hash[65];
    if (!sha256_hex(token, hash)) return false;

    // Manual query handling: the run_query macro aborts with a bare return,
    // which is only valid inside void handlers.
    pgconn_t* db = pgpool_acquire(pool, 10000);
    if (!db) return false;
    defer { pgpool_release(pool, db); };
    static const char* query =
        "SELECT u.id, u.email, u.is_admin FROM sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = $1 AND s.expires_at > now()";
    const char* params[] = {hash};
    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    if (!res) return false;
    defer { PQclear(res); };
    if (PQntuples(res) != 1) return false;

    if (out_user_id) *out_user_id = atoll(PQgetvalue(res, 0, 0));
    if (email_buf) snprintf(email_buf, 320, "%s", PQgetvalue(res, 0, 1));
    if (out_is_admin) *out_is_admin = strcmp(PQgetvalue(res, 0, 2), "t") == 0;
    return true;
}

bool auth_require_login(PulsarConn* conn, int64_t* out_user_id) {
    int64_t user_id = 0;
    if (!session_lookup(conn, &user_id, NULL, NULL)) {
        send_json_error_status(conn, StatusUnauthorized, "authentication required");
        return false;
    }
    if (out_user_id) *out_user_id = user_id;
    return true;
}

bool auth_require_admin(PulsarConn* conn, int64_t* out_user_id) {
    int64_t user_id = 0;
    bool is_admin = false;
    if (!session_lookup(conn, &user_id, &is_admin, NULL)) {
        send_json_error_status(conn, StatusUnauthorized, "authentication required");
        return false;
    }
    if (!is_admin) {
        send_json_error_status(conn, StatusForbidden, "administrator access required");
        return false;
    }
    if (out_user_id) *out_user_id = user_id;
    return true;
}

// Best-effort cleanup; failures must never fail the surrounding request.
static void prune_expired(pgconn_t* db) {
    PGresult* res = pgpool_query_params(db, "DELETE FROM oauth_states WHERE expires_at <= now()",
                                        0, NULL, -1);
    if (res) PQclear(res);
    res = pgpool_query_params(db, "DELETE FROM sessions WHERE expires_at <= now()", 0, NULL, -1);
    if (res) PQclear(res);
}

/* ------------------------------------------------------------------ */
/* Outbound HTTPS via libcurl                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char* data;
    size_t len;
} curl_body_t;

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    curl_body_t* body = userdata;
    const size_t add = size * nmemb;
    if (body->len + add > OAUTH_HTTP_MAX_BYTES) return 0;
    char* grown = realloc(body->data, body->len + add + 1);
    if (!grown) return 0;
    body->data = grown;
    memcpy(body->data + body->len, ptr, add);
    body->len += add;
    body->data[body->len] = '\0';
    return add;
}

static pthread_once_t curl_init_once = PTHREAD_ONCE_INIT;

static void curl_global_init_once(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static CURL* curl_new(void) {
    pthread_once(&curl_init_once, curl_global_init_once);
    CURL* handle = curl_easy_init();
    if (!handle) return NULL;
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, (long)OAUTH_HTTP_TIMEOUT_SECS);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "lexicon");
    return handle;
}

// POST urlencoded fields. On HTTP 200 stores the response body (caller frees).
static bool http_post_form(const char* url, const char* fields, char** out_body,
                           long* out_status) {
    *out_body = NULL;
    if (out_status) *out_status = 0;
    CURL* handle = curl_new();
    if (!handle) return false;
    curl_body_t body = {0};
    struct curl_slist* headers = NULL;
    bool ok = false;
    long status = 0;

    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, fields);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    if (curl_easy_perform(handle) == CURLE_OK &&
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200 &&
        body.data) {
        *out_body = body.data;
        body.data = NULL;
        ok = true;
    }
    if (out_status) *out_status = status;
    free(body.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return ok;
}

// GET with bearer token. On HTTP 200 stores the response body (caller frees).
static bool http_get_bearer(const char* url, const char* token, char** out_body,
                            long* out_status) {
    *out_body = NULL;
    if (out_status) *out_status = 0;
    CURL* handle = curl_new();
    if (!handle) return false;
    curl_body_t body = {0};
    struct curl_slist* headers = NULL;
    char auth[600];
    bool ok = false;
    long status = 0;

    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, auth);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    if (curl_easy_perform(handle) == CURLE_OK &&
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200 &&
        body.data) {
        *out_body = body.data;
        body.data = NULL;
        ok = true;
    }
    if (out_status) *out_status = status;
    free(body.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return ok;
}

static char* json_string_copy(yyjson_val* obj, const char* key) {
    yyjson_val* val = yyjson_obj_get(obj, key);
    if (!yyjson_is_str(val)) return NULL;
    return strdup(yyjson_get_str(val));
}

/* ------------------------------------------------------------------ */
/* Route handlers                                                      */
/* ------------------------------------------------------------------ */

void auth_providers(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
    if (!doc) {
        send_json_error_status(conn, StatusInternalServerError, "Failed to serialize providers");
        return;
    }
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (size_t i = 0; i < OAUTH_PROVIDER_COUNT; i++) {
        yyjson_mut_obj_add_bool(doc, root, OAUTH_PROVIDERS[i].name,
                                provider_configured(&OAUTH_PROVIDERS[i]));
    }
    size_t out_len = 0;
    char* json = yyjson_mut_write_opts(doc, 0, &alc, &out_len, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "Failed to serialize providers");
        return;
    }
    conn_send_json(conn, StatusOK, json, out_len);
}

void auth_login(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param(provider_name, conn, "provider");
    const oauth_provider_def_t* def = provider_by_name(provider_name);
    if (!def) {
        send_json_error_status(conn, StatusNotFound, "unknown OAuth provider");
        return;
    }
    const char* client_id = provider_client_id(def);
    const char* client_secret = provider_client_secret(def);
    if (!client_id || !client_secret) {
        send_json_error_status(conn, StatusServiceUnavailable,
                               "OAuth provider is not configured on this server");
        return;
    }
    const char* base_url = getenv("APP_BASE_URL");
    if (!base_url || !*base_url) {
        send_json_error_status(conn, StatusServiceUnavailable,
                               "APP_BASE_URL is not configured on this server");
        return;
    }

    uint8_t raw_state[OAUTH_STATE_BYTES];
    uint8_t raw_verifier[SESSION_TOKEN_BYTES];
    if (!random_bytes(raw_state, sizeof(raw_state)) ||
        !random_bytes(raw_verifier, sizeof(raw_verifier))) {
        send_json_error_status(conn, StatusInternalServerError,
                               "Failed to initialize login flow");
        return;
    }
    char state[OAUTH_STATE_HEXLEN + 1];
    for (size_t i = 0; i < OAUTH_STATE_BYTES; i++)
        snprintf(state + i * 2, 3, "%02x", raw_state[i]);
    char verifier[SESSION_TOKEN_LEN + 1];
    base64url_encode(raw_verifier, sizeof(raw_verifier), verifier);

    // PKCE S256 challenge over the ASCII verifier.
    uint8_t challenge_raw[32];
    unsigned int challenge_len = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx || EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(mdctx, verifier, strlen(verifier)) != 1 ||
        EVP_DigestFinal_ex(mdctx, challenge_raw, &challenge_len) != 1 || challenge_len != 32) {
        EVP_MD_CTX_free(mdctx);
        send_json_error_status(conn, StatusInternalServerError,
                               "Failed to initialize login flow");
        return;
    }
    EVP_MD_CTX_free(mdctx);
    char challenge[SESSION_TOKEN_LEN + 1];
    base64url_encode(challenge_raw, sizeof(challenge_raw), challenge);

    acquire_db(db);
    static const char* insert_state =
        "INSERT INTO oauth_states(state, provider, code_verifier, expires_at) "
        "VALUES ($1, $2, $3, now() + make_interval(secs => 600))";
    const char* state_params[] = {state, def->name, verifier};
    run_query(state_res, db, insert_state, 3, state_params);
    (void)state_res;
    prune_expired(db);

    char redirect_uri[1024];
    snprintf(redirect_uri, sizeof(redirect_uri), "%s/api/auth/callback/%s", base_url, def->name);
    char enc_id[256], enc_uri[1024], enc_scope[256], enc_state[256], enc_challenge[128];
    if (!url_encode_into(client_id, enc_id, sizeof(enc_id)) ||
        !url_encode_into(redirect_uri, enc_uri, sizeof(enc_uri)) ||
        !url_encode_into(def->scope, enc_scope, sizeof(enc_scope)) ||
        !url_encode_into(state, enc_state, sizeof(enc_state)) ||
        !url_encode_into(challenge, enc_challenge, sizeof(enc_challenge))) {
        send_json_error_status(conn, StatusInternalServerError,
                               "Failed to initialize login flow");
        return;
    }
    char location[4096];
    snprintf(location, sizeof(location),
             "%s?client_id=%s&redirect_uri=%s&response_type=code&scope=%s&state=%s&"
             "code_challenge=%s&code_challenge_method=S256",
             def->auth_url, enc_id, enc_uri, enc_scope, enc_state, enc_challenge);
    conn_send_redirect(conn, location, false);
}

// Logs a provider error body to the server log only — never forwarded to
// clients, since it can echo back request details.
static void log_provider_error(const char* what, long status, const char* body) {
    if (!body) {
        fprintf(stderr, "oauth: %s failed (http %ld, empty response)\n", what, status);
        return;
    }
    char err_buf[256] = {0}, desc_buf[512] = {0};
    const char* err = NULL;
    const char* desc = NULL;
    yyjson_doc* doc = yyjson_read(body, strlen(body), 0);
    if (doc) {
        yyjson_val* root = yyjson_doc_get_root(doc);
        yyjson_val* e = yyjson_obj_get(root, "error");
        yyjson_val* d = yyjson_obj_get(root, "error_description");
        if (yyjson_is_str(e)) snprintf(err_buf, sizeof(err_buf), "%s", yyjson_get_str(e));
        if (yyjson_is_str(d)) snprintf(desc_buf, sizeof(desc_buf), "%s", yyjson_get_str(d));
        yyjson_doc_free(doc);
        if (err_buf[0]) err = err_buf;
        if (desc_buf[0]) desc = desc_buf;
    }
    fprintf(stderr, "oauth: %s failed (http %ld)%s%s%s%s\n", what, status, err ? ": " : "",
            err ? err : "", desc ? " - " : "", desc ? desc : "");
}

typedef struct {
    char* provider_sub;
    char* email;
} oauth_identity_t;

static void oauth_identity_free(oauth_identity_t* id) {
    if (!id) return;
    free(id->provider_sub);
    free(id->email);
}

// Exchanges the authorization code and loads the verified identity.
// Returns true with heap-allocated fields owned by the caller.
static bool oauth_exchange_identity(const oauth_provider_def_t* def, const char* code,
                                    const char* verifier, const char* redirect_uri,
                                    oauth_identity_t* out) {
    memset(out, 0, sizeof(*out));
    const char* client_id = provider_client_id(def);
    const char* client_secret = provider_client_secret(def);
    if (!client_id || !client_secret) return false;

    char enc_id[256], enc_secret[256], enc_code[1024], enc_uri[1024], enc_verifier[256];
    if (!url_encode_into(client_id, enc_id, sizeof(enc_id)) ||
        !url_encode_into(client_secret, enc_secret, sizeof(enc_secret)) ||
        !url_encode_into(code, enc_code, sizeof(enc_code)) ||
        !url_encode_into(redirect_uri, enc_uri, sizeof(enc_uri)) ||
        !url_encode_into(verifier, enc_verifier, sizeof(enc_verifier))) {
        return false;
    }
    char fields[4096];
    if (def->grant_in_token) {
        snprintf(fields, sizeof(fields),
                 "client_id=%s&client_secret=%s&code=%s&grant_type=authorization_code&"
                 "redirect_uri=%s&code_verifier=%s",
                 enc_id, enc_secret, enc_code, enc_uri, enc_verifier);
    } else {
        snprintf(fields, sizeof(fields),
                 "client_id=%s&client_secret=%s&code=%s&redirect_uri=%s&code_verifier=%s", enc_id,
                 enc_secret, enc_code, enc_uri, enc_verifier);
    }

    char* token_body = NULL;
    long token_status = 0;
    if (!http_post_form(def->token_url, fields, &token_body, &token_status)) {
        log_provider_error("token exchange", token_status, token_body);
        free(token_body);
        return false;
    }
    defer { free(token_body); };

    yyjson_doc* token_doc = yyjson_read(token_body, strlen(token_body), 0);
    if (!token_doc) return false;
    defer { yyjson_doc_free(token_doc); };
    yyjson_val* token_root = yyjson_doc_get_root(token_doc);
    char* access_token = json_string_copy(token_root, "access_token");
    if (!access_token) return false;
    defer { free(access_token); };

    char* user_body = NULL;
    long user_status = 0;
    if (!http_get_bearer(def->userinfo_url, access_token, &user_body, &user_status)) {
        log_provider_error("userinfo fetch", user_status, user_body);
        free(user_body);
        return false;
    }
    defer { free(user_body); };

    yyjson_doc* user_doc = yyjson_read(user_body, strlen(user_body), 0);
    if (!user_doc) return false;
    defer { yyjson_doc_free(user_doc); };
    yyjson_val* user_root = yyjson_doc_get_root(user_doc);

    if (strcmp(def->name, "google") == 0) {
        yyjson_val* verified = yyjson_obj_get(user_root, "email_verified");
        if (!yyjson_is_true(verified)) return false;
        out->provider_sub = json_string_copy(user_root, "sub");
        out->email = json_string_copy(user_root, "email");
        if (!out->provider_sub || !out->email) {
            oauth_identity_free(out);
            memset(out, 0, sizeof(*out));
            return false;
        }
        return true;
    }

    if (strcmp(def->name, "github") == 0) {
        yyjson_val* id_val = yyjson_obj_get(user_root, "id");
        if (yyjson_is_uint(id_val)) {
            char sub[32];
            snprintf(sub, sizeof(sub), "%llu", (unsigned long long)yyjson_get_uint(id_val));
            out->provider_sub = strdup(sub);
        } else if (yyjson_is_sint(id_val)) {
            char sub[32];
            snprintf(sub, sizeof(sub), "%lld", (long long)yyjson_get_sint(id_val));
            out->provider_sub = strdup(sub);
        } else if (yyjson_is_str(id_val)) {
            out->provider_sub = strdup(yyjson_get_str(id_val));
        }
        if (!out->provider_sub) return false;

        char* emails_body = NULL;
        long emails_status = 0;
        if (!http_get_bearer(def->emails_url, access_token, &emails_body, &emails_status)) {
            log_provider_error("github emails fetch", emails_status, emails_body);
            free(emails_body);
        } else {
            defer { free(emails_body); };
            yyjson_doc* emails_doc = yyjson_read(emails_body, strlen(emails_body), 0);
            if (emails_doc) {
                defer { yyjson_doc_free(emails_doc); };
                yyjson_val* emails_root = yyjson_doc_get_root(emails_doc);
                yyjson_val *item, *first_verified = NULL;
                size_t idx = 0, max = 0;
                yyjson_arr_foreach(emails_root, idx, max, item) {
                    (void)idx;
                    (void)max;
                    yyjson_val* email_val = yyjson_obj_get(item, "email");
                    if (!yyjson_is_str(email_val)) continue;
                    const bool primary = yyjson_is_true(yyjson_obj_get(item, "primary"));
                    const bool verified = yyjson_is_true(yyjson_obj_get(item, "verified"));
                    if (!verified) continue;
                    if (!first_verified) first_verified = email_val;
                    if (primary) {
                        first_verified = email_val;
                        break;
                    }
                }
                if (first_verified) out->email = strdup(yyjson_get_str(first_verified));
            }
        }
        if (!out->email) {
            free(out->provider_sub);
            out->provider_sub = NULL;
            return false;
        }
        return true;
    }

    return false;
}

void auth_callback(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param(provider_name, conn, "provider");
    const oauth_provider_def_t* def = provider_by_name(provider_name);
    if (!def) {
        send_json_error_status(conn, StatusNotFound, "unknown OAuth provider");
        return;
    }
    // NOTE: pulsar's require_query macro is avoided here — the installed
    // headers define abort_if_nullptr with an incompatible arity.
    const char* code = query_get(conn, "code");
    const char* state = query_get(conn, "state");
    if (!code || !state) {
        send_json_error_status(conn, StatusBadRequest, "missing required query parameter");
        return;
    }

    if (strlen(code) == 0 || strlen(code) > 2048 || strlen(state) != OAUTH_STATE_HEXLEN) {
        send_json_error_status(conn, StatusBadRequest, "invalid OAuth response");
        return;
    }

    acquire_db(db);
    static const char* consume_state =
        "DELETE FROM oauth_states WHERE state = $1 AND expires_at > now() "
        "RETURNING provider, code_verifier";
    const char* state_params[] = {state};
    run_query(state_res, db, consume_state, 1, state_params);
    if (PQntuples(state_res) != 1 ||
        strcmp(PQgetvalue(state_res, 0, 0), def->name) != 0) {
        send_json_error_status(conn, StatusBadRequest, "invalid or expired login session");
        return;
    }
    char* verifier = strdup(PQgetvalue(state_res, 0, 1));
    if (!verifier) {
        send_json_error_status(conn, StatusInternalServerError, "out of memory");
        return;
    }
    defer { free(verifier); };

    const char* base_url = getenv("APP_BASE_URL");
    if (!base_url || !*base_url) {
        send_json_error_status(conn, StatusServiceUnavailable,
                               "APP_BASE_URL is not configured on this server");
        return;
    }
    char redirect_uri[1024];
    snprintf(redirect_uri, sizeof(redirect_uri), "%s/api/auth/callback/%s", base_url, def->name);

    oauth_identity_t identity = {0};
    if (!oauth_exchange_identity(def, code, verifier, redirect_uri, &identity)) {
        send_json_error_status(conn, StatusBadGateway, "OAuth provider verification failed");
        return;
    }
    defer { oauth_identity_free(&identity); };

    const bool admin = admin_allowlisted(identity.email);
    static const char* upsert_user =
        "INSERT INTO users(provider, provider_sub, email, is_admin, last_login_at) "
        "VALUES ($1, $2, $3, $4, now()) "
        "ON CONFLICT (provider, provider_sub) DO UPDATE SET email = EXCLUDED.email, "
        "is_admin = EXCLUDED.is_admin, last_login_at = now() "
        "RETURNING id, is_admin";
    const char* user_params[] = {def->name, identity.provider_sub, identity.email,
                                 admin ? "true" : "false"};
    run_query(user_res, db, upsert_user, 4, user_params);
    if (PQntuples(user_res) != 1) {
        send_json_error_status(conn, StatusInternalServerError, "failed to provision user");
        return;
    }
    const char* user_id = PQgetvalue(user_res, 0, 0);
    const bool is_admin = strcmp(PQgetvalue(user_res, 0, 1), "t") == 0;
    if (!is_admin) {
        send_json_error_status(conn, StatusForbidden,
                               "this account is not authorized to use this application");
        return;
    }

    uint8_t raw_token[SESSION_TOKEN_BYTES];
    if (!random_bytes(raw_token, sizeof(raw_token))) {
        send_json_error_status(conn, StatusInternalServerError, "failed to create session");
        return;
    }
    char token[SESSION_TOKEN_LEN + 1];
    base64url_encode(raw_token, sizeof(raw_token), token);
    char token_hash[65];
    if (!sha256_hex(token, token_hash)) {
        send_json_error_status(conn, StatusInternalServerError, "failed to create session");
        return;
    }
    // Expiry computed in C (timestamptz literal) rather than SQL interval
    // math, so no parameter-type subtleties arise.
    char expires_at[32];
    {
        const time_t exp = time(NULL) + session_ttl_secs();
        struct tm tm_utc;
        gmtime_r(&exp, &tm_utc);
        strftime(expires_at, sizeof(expires_at), "%Y-%m-%d %H:%M:%S+00", &tm_utc);
    }
    static const char* insert_session =
        "INSERT INTO sessions(token_hash, user_id, expires_at) "
        "VALUES ($1, $2, $3::timestamptz)";
    const char* session_params[] = {token_hash, user_id, expires_at};
    run_query(session_res, db, insert_session, 3, session_params);
    (void)session_res;
    prune_expired(db);

    char app_root[1024];
    snprintf(app_root, sizeof(app_root), "%s", base_url);
    char* qmark = strchr(app_root, '?');
    if (qmark) *qmark = '\0';
    size_t root_len = strlen(app_root);
    while (root_len > 0 && app_root[root_len - 1] == '/') app_root[--root_len] = '\0';
    set_session_cookie(conn, token, session_ttl_secs());
    conn_send_redirect(conn, root_len ? app_root : "/", false);
}

void auth_logout(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    char token[SESSION_TOKEN_LEN + 1];
    // Best effort server-side invalidation; the cookie is cleared regardless.
    const bool have_token = request_session_token(conn, token);
    char hash[65] = {0};
    const bool hashed = have_token && sha256_hex(token, hash);
    clear_session_cookie(conn);
    if (hashed) {
        acquire_db(db);
        static const char* delete_session = "DELETE FROM sessions WHERE token_hash = $1";
        const char* params[] = {hash};
        run_query(res, db, delete_session, 1, params);
        (void)res;
    }

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
    if (!doc) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", true);
    size_t out_len = 0;
    char* json = yyjson_mut_write_opts(doc, 0, &alc, &out_len, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    conn_send_json(conn, StatusOK, json, out_len);
}

void auth_whoami(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    char token[SESSION_TOKEN_LEN + 1];
    if (!request_session_token(conn, token)) {
        send_json_error_status(conn, StatusUnauthorized, "authentication required");
        return;
    }
    char hash[65];
    if (!sha256_hex(token, hash)) {
        send_json_error_status(conn, StatusUnauthorized, "authentication required");
        return;
    }

    acquire_db(db);
    static const char* query =
        "SELECT u.id, u.email, u.provider, u.is_admin FROM sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = $1 AND s.expires_at > now()";
    const char* params[] = {hash};
    run_query(res, db, query, 1, params);
    if (PQntuples(res) != 1) {
        send_json_error_status(conn, StatusUnauthorized, "authentication required");
        return;
    }

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    yyjson_mut_doc* doc = yyjson_mut_doc_new(&alc);
    if (!doc) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "id", atoll(PQgetvalue(res, 0, 0)));
    yyjson_mut_obj_add_str(doc, root, "email", PQgetvalue(res, 0, 1));
    yyjson_mut_obj_add_str(doc, root, "provider", PQgetvalue(res, 0, 2));
    yyjson_mut_obj_add_bool(doc, root, "is_admin",
                            strcmp(PQgetvalue(res, 0, 3), "t") == 0);
    size_t out_len = 0;
    char* json = yyjson_mut_write_opts(doc, 0, &alc, &out_len, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    conn_send_json(conn, StatusOK, json, out_len);
}
