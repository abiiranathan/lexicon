#include <pgconn/pgconn.h>
#include <pgconn/pgtypes.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/str_to_num.h>
#include <string.h>
#include <yyjson.h>
#include "pdf.h"

// Connections array with each worker getting its own connection.
extern pgconn_t* connections[NUM_WORKERS];

// Get current connection for worker.
#define DB(conn) ((connections[(size_t)conn_worker_id(conn)]))

/**
 * Sends a JSON error response to the client.
 * @param conn The connection to send the error on.
 * @param msg The error message. If NULL, uses a default message.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    char json_str[128];
    snprintf(json_str, sizeof(json_str) - 1, "{\"error\": \"%s\"}", msg ? msg : "Something wrong happened");
    conn_send_json(conn, StatusBadRequest, json_str);
}

/**
 * Returns full text for a given file_id and page_num.
 * Path: /api/file/{file_id}/page/{page_num}
 */
void get_page_by_file_and_page(PulsarCtx* ctx) {
    PulsarConn* conn         = ctx->conn;
    const char* file_id_str  = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

    int64_t file_id;
    int64_t page_num_ll;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }
    if (str_to_i64(page_num_str, &page_num_ll) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[]     = {file_id_str, page_num_str};

    pgconn_t* db  = DB(conn);
    PGresult* res = pgconn_query_params(db, query, 2, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No page found for the requested file and page number");
        return;
    }

    const char* text = PQgetvalue(res, 0, 0);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    yyjson_mut_obj_add_int(doc, root, "page_num", (int)page_num_ll);
    yyjson_mut_obj_add_str(doc, root, "text", text ? text : "");

    char* json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }
    defer({ free(json_str); });

    conn_send_json(conn, StatusOK, json_str);
}

void render_pdf_page_as_png(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;

    const char* file_id_str  = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

    int64_t file_id;
    int page_num;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_int(page_num_str, &page_num) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = DB(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No file found for the requested file");
        return;
    }

    const char* path = PQgetvalue(res, 0, 0);

    // page numbers are +1, so we subtract 1
    if (page_num < 1) {
        send_json_error(conn, "Page out of range");
        return;
    }

    png_buffer_t png_buffer = {0};
    if (render_page_from_document_to_buffer(path, page_num - 1, &png_buffer)) {
        static const char headers[] = "Content-Type: image/png\r\nCache-Control: public, max-age=3600\r\n";
        conn_writeheader_raw(conn, headers, sizeof(headers) - 1);
        conn_write(conn, png_buffer.data, png_buffer.size);
        free(png_buffer.data);
    } else {
        conn_set_status(conn, StatusInternalServerError);
        send_json_error(conn, "Error writing PNG image");
        return;
    }
}

/**
 * Performs full-text search on PDF pages.
 * Returns matching results with snippets, ranked by relevance.
 */
void pdf_search(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;

    const char* query = query_get(conn, "q");
    if (!query || query[0] == '\0') {
        send_json_error(conn, "Missing search query");
        return;
    }

    static const char* search_query =
        "WITH query AS ("
        "  SELECT websearch_to_tsquery('english', $1) as tsq"
        "), "
        "RankedChunks AS ("
        "  SELECT "
        "    p.file_id, f.name, f.num_pages, p.page_num, p.text, "
        "    ts_rank(p.text_vector, query.tsq) as rank "
        "  FROM pages p "
        "  CROSS JOIN query "
        "  JOIN files f ON p.file_id = f.id "
        "  WHERE p.text_vector @@ query.tsq "
        "    AND ts_rank(p.text_vector, query.tsq) >= 0.005 "
        "), "
        "UniquePages AS ("
        "  SELECT DISTINCT ON (file_id, page_num) "
        "    file_id, name, num_pages, page_num, text, rank "
        "  FROM RankedChunks "
        "  ORDER BY file_id, page_num, rank DESC"
        ") "
        "SELECT "
        "  file_id, name, num_pages, page_num, "
        "  ts_headline('english', text, (SELECT tsq FROM query), "
        "    'MaxWords=30, MinWords=10, MaxFragments=3') as snippet, "
        "  rank "
        "FROM UniquePages "
        "ORDER BY rank DESC, name, page_num "
        "LIMIT 100";

    pgconn_t* db = DB(conn);

    char query_suffix[128];
    snprintf(query_suffix, sizeof(query_suffix), "%s:*", query);
    const char* params[] = {query_suffix};
    PGresult* res        = pgconn_query_params(db, search_query, 1, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    // Create mutable document for building JSON
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* results_array = yyjson_mut_arr(doc);

    int ntuples = PQntuples(res);
    bool file_id_valid, page_num_valid, rank_valid, num_pages_valid;

    for (int i = 0; i < ntuples; i++) {
        int64_t file_id;
        int page_num, num_pages;
        double rank;

        // Parse all numeric fields - skip row if any fail
        file_id   = pg_get_longlong(res, i, 0, &file_id_valid);
        num_pages = pg_get_int(res, i, 2, &num_pages_valid);
        page_num  = pg_get_int(res, i, 3, &page_num_valid);
        rank      = pg_get_double(res, i, 5, &rank_valid);
        if (!file_id_valid || !page_num_valid || !rank_valid || !num_pages_valid) {
            continue;
        }

        const char* file_name = PQgetvalue(res, i, 1);
        const char* snippet   = PQgetvalue(res, i, 4);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        if (!result_obj) {
            continue;
        }

        yyjson_mut_obj_add_int(doc, result_obj, "file_id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "file_name", file_name ? file_name : "");
        yyjson_mut_obj_add_int(doc, result_obj, "page_num", page_num);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", num_pages);
        yyjson_mut_obj_add_str(doc, result_obj, "snippet", snippet ? snippet : "");
        yyjson_mut_obj_add_real(doc, result_obj, "rank", rank);

        yyjson_mut_arr_append(results_array, result_obj);
    }

    yyjson_mut_obj_add_val(doc, root, "results", results_array);
    yyjson_mut_obj_add_uint(doc, root, "count", yyjson_mut_arr_size(results_array));
    yyjson_mut_obj_add_str(doc, root, "query", query);

    // Serialize to string
    char* json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }
    defer({ free(json_str); });

    conn_send_json(conn, StatusOK, json_str);
}

/** Lists all PDF files in the database paginated on query params "page" & limit
Returns JSON object with keys: "page", "limit", "results", "has_next", "has_prev","total_count", "total_pages"
*/
void list_files(PulsarCtx* ctx) {
    PulsarConn* conn          = ctx->conn;
    const char* page_str      = query_get(conn, "page");
    const char* page_size_str = query_get(conn, "limit");
    const char* name          = query_get(conn, "name");

    int page = 1, page_size = 10;
    if (page_str) {
        str_to_int(page_str, &page);
    }

    if (page_size_str) {
        str_to_int(page_size_str, &page_size);
    }

    // Ensure page is at least 1
    if (page < 1) {
        page = 1;
    }
    // Ensure page_size is reasonable
    if (page_size < 1) {
        page_size = 25;
    }
    if (page_size > 100) {
        page_size = 100;
    }

    pgconn_t* db = DB(conn);

    // First, get the total count of files
    const char* count_query = "SELECT COUNT(*) FROM files";
    PGresult* count_res     = pgconn_query(db, count_query, NULL);
    if (!count_res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(count_res); });

    int64_t total_count = 0;
    if (PQntuples(count_res) > 0) {
        const char* count_str = PQgetvalue(count_res, 0, 0);
        if (count_str) {
            total_count = strtoll(count_str, NULL, 10);
        }
    }

    // Calculate offset for pagination
    int offset = (page - 1) * page_size;

    // After validating page_size, add name validation
    const char* query;
    int param_count;
    const char* params[3];  // Increased from 2 to accommodate name parameter

    // Prepare limit and offset parameters
    char limit_str[32], offset_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", page_size);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    if (name && name[0] != '\0') {
        // Filter by name using case-insensitive ILIKE
        char nameFilter[128];
        snprintf(nameFilter, sizeof(nameFilter), "%%%s%%", name);
        query     = "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT $2 OFFSET $3";
        params[0] = nameFilter;
        params[1] = limit_str;
        params[2] = offset_str;
        param_count = 3;
    } else {
        // No name filter
        query       = "SELECT id, name, path, num_pages FROM files ORDER BY name LIMIT $1 OFFSET $2";
        params[0]   = limit_str;
        params[1]   = offset_str;
        param_count = 2;
    }
    PGresult* res = pgconn_query_params(db, query, param_count, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    // Create root object instead of array
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    if (!root) {
        send_json_error(conn, "Failed to create JSON root object");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);

    // Create results array
    yyjson_mut_val* results_array = yyjson_mut_arr(doc);
    if (!results_array) {
        send_json_error(conn, "Failed to create JSON results array");
        return;
    }
    yyjson_mut_obj_add_val(doc, root, "results", results_array);

    int ntuples = PQntuples(res);
    for (int i = 0; i < ntuples; i++) {
        const char* file_id_str   = PQgetvalue(res, i, 0);
        const char* file_name     = PQgetvalue(res, i, 1);
        const char* file_path     = PQgetvalue(res, i, 2);
        const char* num_pages_str = PQgetvalue(res, i, 3);

        if (!file_id_str || !file_name || !file_path || !num_pages_str) {
            continue;
        }

        // Convert file ID and num_pages to integers
        int64_t file_id   = strtoll(file_id_str, NULL, 10);
        int64_t num_pages = strtoll(num_pages_str, NULL, 10);

        yyjson_mut_val* result_obj = yyjson_mut_obj(doc);
        if (!result_obj) {
            continue;
        }

        yyjson_mut_obj_add_int(doc, result_obj, "id", file_id);
        yyjson_mut_obj_add_str(doc, result_obj, "name", file_name);
        yyjson_mut_obj_add_str(doc, result_obj, "path", file_path);
        yyjson_mut_obj_add_int(doc, result_obj, "num_pages", (int)num_pages);

        yyjson_mut_arr_append(results_array, result_obj);
    }

    // Add pagination metadata
    yyjson_mut_obj_add_int(doc, root, "page", page);
    yyjson_mut_obj_add_int(doc, root, "limit", page_size);
    yyjson_mut_obj_add_int(doc, root, "total_count", (int)total_count);

    // Calculate has_next and has_prev
    int64_t total_pages = (total_count + page_size - 1) / page_size;
    if (total_pages == 0) {
        total_pages = 1;  // At least one page even if empty
    }
    yyjson_mut_obj_add_bool(doc, root, "has_next", page < total_pages);
    yyjson_mut_obj_add_bool(doc, root, "has_prev", page > 1);
    yyjson_mut_obj_add_int(doc, root, "total_pages", (int)total_pages);

    char* json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    defer({ free(json_str); });

    conn_send_json(conn, StatusOK, json_str);
}

/** Retrieves a single file by its ID. */
void get_file_by_id(PulsarCtx* ctx) {
    PulsarConn* conn        = ctx->conn;
    const char* file_id_str = get_path_param(conn, "file_id");
    int64_t file_id;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[]     = {file_id_str};

    pgconn_t* db  = DB(conn);
    PGresult* res = pgconn_query_params(db, query, 1, params, NULL);
    if (!res) {
        send_json_error(conn, pgconn_error_message(db));
        return;
    }
    defer({ PQclear(res); });

    if (PQntuples(res) == 0) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No book matches the requested ID");
        return;
    }

    const char* file_name     = PQgetvalue(res, 0, 0);
    const char* file_path     = PQgetvalue(res, 0, 1);
    const char* num_pages_str = PQgetvalue(res, 0, 2);

    int64_t num_pages = strtoll(num_pages_str, NULL, 10);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        send_json_error(conn, "Failed to create JSON document");
        return;
    }
    defer({ yyjson_mut_doc_free(doc); });

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "id", file_id);
    yyjson_mut_obj_add_str(doc, root, "name", file_name ? file_name : "");
    yyjson_mut_obj_add_str(doc, root, "path", file_path ? file_path : "");
    yyjson_mut_obj_add_int(doc, root, "num_pages", (int)num_pages);

    char* json_str = yyjson_mut_write(doc, 0, NULL);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }
    defer({ free(json_str); });

    conn_send_json(conn, StatusOK, json_str);
}
