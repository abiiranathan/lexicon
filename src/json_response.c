#include "../include/json_response.h"
#include "../include/database.h"

#include <pgconn/pgtypes.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

// Helper to serialize and return string
static char* serialize_doc(yyjson_mut_doc* doc, size_t* out_len) {
    size_t len     = 0;
    char* json_str = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);

    if (out_len) {
        *out_len = len;
    }
    return json_str;
}

char* json_create_error(const char* msg) {
    yyjson_mut_doc* doc  = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "error", msg ? msg : "Unknown error");
    return serialize_doc(doc, NULL);
}

char* json_create_page_response(int64_t file_id, int page_num, const char* text) {
    yyjson_mut_doc* doc  = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    yyjson_mut_obj_add_int(doc, root, "page_num", page_num);
    yyjson_mut_obj_add_str(doc, root, "text", text ? text : "");

    return serialize_doc(doc, NULL);
}

char* json_create_file_response(int64_t id, const char* name, const char* path, int64_t num_pages, size_t* out_len) {
    yyjson_mut_doc* doc  = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "name", name ? name : "");
    yyjson_mut_obj_add_str(doc, root, "path", path ? path : "");
    yyjson_mut_obj_add_int(doc, root, "num_pages", (int)num_pages);

    return serialize_doc(doc, out_len);
}

char* json_create_file_list(PGresult* res, int page, int limit, int64_t total_count, size_t* out_len) {
    yyjson_mut_doc* doc  = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "results", results_array);

    int ntuples = PQntuples(res);
    for (int i = 0; i < ntuples; i++) {
        // Columns based on: SELECT id, name, path, num_pages ...
        const char* file_id_str   = PQgetvalue(res, i, 0);
        const char* file_name     = PQgetvalue(res, i, 1);
        const char* file_path     = PQgetvalue(res, i, 2);
        const char* num_pages_str = PQgetvalue(res, i, 3);

        if (!file_id_str || !file_name || !file_path || !num_pages_str) continue;

        int64_t file_id   = strtoll(file_id_str, NULL, 10);
        int64_t num_pages = strtoll(num_pages_str, NULL, 10);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, result_obj, "id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "name", file_name);
        yyjson_mut_obj_add_str(doc, result_obj, "path", file_path);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", (int)num_pages);

        yyjson_mut_arr_append(results_array, result_obj);
    }

    yyjson_mut_obj_add_int(doc, root, "page", page);
    yyjson_mut_obj_add_int(doc, root, "limit", limit);
    yyjson_mut_obj_add_int(doc, root, "total_count", (int)total_count);

    int64_t total_pages = (total_count + limit - 1) / limit;
    if (total_pages == 0) total_pages = 1;

    yyjson_mut_obj_add_bool(doc, root, "has_next", page < total_pages);
    yyjson_mut_obj_add_bool(doc, root, "has_prev", page > 1);
    yyjson_mut_obj_add_int(doc, root, "total_pages", (int)total_pages);

    return serialize_doc(doc, out_len);
}

char* json_create_search_results(PGresult* res, const char* query, const char* ai_summary, size_t* out_len) {
    yyjson_mut_doc* doc  = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);

    int ntuples = PQntuples(res);
    bool valid;

    // Columns based on SQL in routes.c:
    // 0: file_id, 1: name, 2: num_pages, 3: page_num, 4: snippet, 5: extended_snippet
    for (int i = 0; i < ntuples; i++) {
        int64_t file_id = pg_get_longlong(res, i, 0, &valid);
        if (!valid) continue;
        int num_pages = pg_get_int(res, i, 2, &valid);
        if (!valid) continue;
        int page_num = pg_get_int(res, i, 3, &valid);
        if (!valid) continue;

        const char* file_name = PQgetvalue(res, i, 1);
        const char* snippet   = PQgetvalue(res, i, 4);

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

    if (ai_summary) {
        yyjson_mut_obj_add_str(doc, root, "ai_summary", ai_summary);
    } else {
        yyjson_mut_obj_add_null(doc, root, "ai_summary");
    }

    return serialize_doc(doc, out_len);
}
