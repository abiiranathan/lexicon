#include "../include/json_response.h"
#include <math.h>
#include "../include/database.h"

/**
 * Serializes @p doc to JSON, frees the doc, and returns the output buffer.
 *
 * @param doc     The mutable document to serialize. Always consumed.
 * @param alc     Allocator used for the output buffer. If NULL, yyjson uses
 *                malloc and the caller must free() the returned pointer.
 *                If arena-backed, the buffer lives until arena_destroy().
 * @param out_len Set to the byte length of the returned string (excluding
 *                the null terminator). Ignored if NULL.
 * @return        Pointer to the serialized JSON string, or NULL on failure.
 */
static char* serialize_doc(yyjson_mut_doc* doc, yyjson_alc* alc, size_t* out_len) {
    size_t len = 0;
    char* json_str = yyjson_mut_write_opts(doc, 0, alc, &len, NULL);
    yyjson_mut_doc_free(doc);

    if (out_len) { *out_len = len; }
    return json_str;
}

/**
 * Builds a JSON error object: {"error": "<msg>"}.
 *
 * @param alc     Allocator for the output buffer. See serialize_doc().
 * @param msg     Error message string. Falls back to "Unknown error" if NULL.
 * @param out_len Set to the byte length of the returned string. May be NULL.
 * @return        Caller-owned JSON string. Lifetime governed by @p alc.
 */
char* json_create_error(yyjson_alc* alc, const char* msg, size_t* out_len) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "error", msg ? msg : "Unknown error");
    return serialize_doc(doc, alc, out_len);
}

/**
 * Builds a JSON page object: {"file_id": ..., "page_num": ..., "text": ...}.
 *
 * The returned StrSlice's .data pointer is owned by @p alc. If @p alc is
 * NULL, the caller must free() ss.data. If arena-backed, ss.data is valid
 * until arena_destroy().
 *
 * @param alc      Allocator for the output buffer. See serialize_doc().
 * @param file_id  Database file identifier.
 * @param page_num 1-based page number.
 * @param text     Page text content. Falls back to "" if NULL.
 * @return         StrSlice pointing into the serialized JSON string.
 */
StrSlice json_create_page_response(yyjson_alc* alc, int64_t file_id, int page_num, const char* text) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    yyjson_mut_obj_add_int(doc, root, "page_num", page_num);
    yyjson_mut_obj_add_str(doc, root, "text", text ? text : "");

    size_t out_len = 0;
    char* data = serialize_doc(doc, alc, &out_len);
    return ss_from(data, out_len);
}

/**
 * Builds a JSON file object: {"id": ..., "name": ..., "path": ...,
 *                              "num_pages": ...}.
 *
 * @param alc       Allocator for the output buffer. See serialize_doc().
 * @param id        Database file identifier.
 * @param name      File name. Falls back to "" if NULL.
 * @param path      File path. Falls back to "" if NULL.
 * @param num_pages Total page count.
 * @param out_len   Set to the byte length of the returned string. May be NULL.
 * @return          JSON string. Lifetime governed by @p alc.
 */
char* json_create_file_response(yyjson_alc* alc, int64_t id, const char* name, const char* path, int64_t num_pages,
                                size_t* out_len) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "name", name ? name : "");
    yyjson_mut_obj_add_str(doc, root, "path", path ? path : "");
    yyjson_mut_obj_add_int(doc, root, "num_pages", (int)num_pages);

    return serialize_doc(doc, alc, out_len);
}

/**
 * Builds a paginated JSON file list from a PGresult.
 *
 * Expected column order in @p res: id, name, path, num_pages.
 * Rows with any NULL column are silently skipped.
 *
 * Output shape:
 *   {"results": [...], "page": N, "limit": N, "total_count": N,
 *    "total_pages": N, "has_next": bool, "has_prev": bool}
 *
 * @param alc         Allocator for the output buffer. See serialize_doc().
 * @param res         PGresult from a SELECT id, name, path, num_pages query.
 * @param page        Current 1-based page number.
 * @param limit       Page size used in the query.
 * @param total_count Total row count across all pages.
 * @param out_len     Set to the byte length of the returned string. May be NULL.
 * @return            JSON string. Lifetime governed by @p alc.
 */
char* json_create_file_list(yyjson_alc* alc, PGresult* res, int page, int limit, int64_t total_count, size_t* out_len) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "results", results_array);

    int ntuples = PQntuples(res);
    for (int i = 0; i < ntuples; i++) {
        const char* file_id_str = PQgetvalue(res, i, 0);
        const char* file_name = PQgetvalue(res, i, 1);
        const char* file_path = PQgetvalue(res, i, 2);
        const char* num_pages_str = PQgetvalue(res, i, 3);

        if (!file_id_str || !file_name || !file_path || !num_pages_str) continue;

        int64_t file_id = strtoll(file_id_str, NULL, 10);
        int64_t num_pages = strtoll(num_pages_str, NULL, 10);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, result_obj, "id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "name", file_name);
        yyjson_mut_obj_add_str(doc, result_obj, "path", file_path);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", (int)num_pages);

        yyjson_mut_arr_append(results_array, result_obj);
    }

    int64_t total_pages = (total_count + limit - 1) / limit;
    if (total_pages == 0) total_pages = 1;

    yyjson_mut_obj_add_int(doc, root, "page", page);
    yyjson_mut_obj_add_int(doc, root, "limit", limit);
    yyjson_mut_obj_add_int(doc, root, "total_count", (int)total_count);
    yyjson_mut_obj_add_int(doc, root, "total_pages", (int)total_pages);
    yyjson_mut_obj_add_bool(doc, root, "has_next", page < total_pages);
    yyjson_mut_obj_add_bool(doc, root, "has_prev", page > 1);

    return serialize_doc(doc, alc, out_len);
}

/**
 * Builds a JSON search result list from a PGresult.
 *
 * Expected column order in @p res:
 *   0: file_id, 1: name, 2: num_pages, 3: page_num, 4: snippet
 * Rows where any integer column fails to parse are silently skipped.
 *
 * Output shape:
 *   {"results": [...], "count": N, "query": "..."}
 *
 * @param alc     Allocator for the output buffer. See serialize_doc().
 * @param res     PGresult from the full-text search query.
 * @param query   The original search query string echoed into the response.
 * @param out_len Set to the byte length of the returned string. May be NULL.
 * @return        JSON string. Lifetime governed by @p alc.
 */
char* json_create_search_results(yyjson_alc* alc, PGresult* res, const char* query, size_t* out_len) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);

    int ntuples = PQntuples(res);
    bool valid;

    for (int i = 0; i < ntuples; i++) {
        int64_t file_id = pg_get_longlong(res, i, 0, &valid);
        if (!valid) continue;
        int num_pages = pg_get_int(res, i, 2, &valid);
        if (!valid) continue;
        int page_num = pg_get_int(res, i, 3, &valid);
        if (!valid) continue;

        const char* file_name = PQgetvalue(res, i, 1);
        const char* snippet = PQgetvalue(res, i, 4);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, result_obj, "file_id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "file_name", file_name ? file_name : "");
        yyjson_mut_obj_add_int(doc, result_obj, "page_num", page_num);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", num_pages);
        yyjson_mut_obj_add_str(doc, result_obj, "snippet", snippet ? snippet : "");

        yyjson_mut_arr_append(results_array, result_obj);
    }

    yyjson_mut_obj_add_val(doc, root, "results", results_array);
    yyjson_mut_obj_add_uint(doc, root, "count", yyjson_mut_arr_size(results_array));
    yyjson_mut_obj_add_str(doc, root, "query", query);

    return serialize_doc(doc, alc, out_len);
}

/** Rounds a value to 2 decimal places to shrink JSON output size. */
static inline double round2(double v) {
    return round(v * 100.0) / 100.0;
}
/**
 * Builds a JSON text-layer object from per-character boxes.
 *
 * @param alc         Allocator for the output buffer. See serialize_doc().
 * @param page_width  Page width in PDF points.
 * @param page_height Page height in PDF points.
 * @param boxes       Array of character boxes. May be NULL if count is 0.
 * @param count       Number of entries in @p boxes.
 * @param out_len     Set to the byte length of the returned string. May be NULL.
 * @return            JSON string. Lifetime governed by @p alc.
 */
char* json_create_text_layer(yyjson_alc* alc, float page_width, float page_height, const pdf_char_box_t* boxes,
                             int count, size_t* out_len) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_real(doc, root, "width", round2(page_width));
    yyjson_mut_obj_add_real(doc, root, "height", round2(page_height));

    yyjson_mut_val* chars_array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "chars", chars_array);

    for (int i = 0; i < count; i++) {
        const pdf_char_box_t* b = &boxes[i];

        yyjson_mut_val* box_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, box_obj, "c", b->codepoint);
        yyjson_mut_obj_add_real(doc, box_obj, "l", round2(b->left));
        yyjson_mut_obj_add_real(doc, box_obj, "r", round2(b->right));
        yyjson_mut_obj_add_real(doc, box_obj, "b", round2(b->bottom));
        yyjson_mut_obj_add_real(doc, box_obj, "t", round2(b->top));

        yyjson_mut_arr_append(chars_array, box_obj);
    }

    return serialize_doc(doc, alc, out_len);
}
