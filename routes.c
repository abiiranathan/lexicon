#include <cjson/cJSON.h>
#include <pgpool.h>
#include <pulsar/pulsar.h>
#include <solidc/defer.h>
#include <solidc/str_to_num.h>

extern pgpool_t* pool;  // connection pool.
extern int TIMEOUT_MS;  // database default timeout

static const unsigned char indexHtml[] = {
#embed "index.html"
};

static inline void send_json_error(connection_t* conn, const char* msg) {
    char json_str[128];
    snprintf(json_str, sizeof(json_str) - 1, "{\"error\": \"%s\"}",
             msg ? msg : "Something wrong happened");

    conn_send_json(conn, StatusBadRequest, json_str);
}

void home_route(connection_t* conn) {
    static const char contentType[] = "Content-Type: text/html\r\n";
    conn_writeheader_raw(conn, contentType, sizeof(contentType) - 1);
    conn_write(conn, indexHtml, sizeof(indexHtml));
}

void pdf_search(connection_t* conn) {
    static const char* search_query =
        "SELECT p.file_id, f.name, p.page_num, "
        "ts_headline('english', p.text, plainto_tsquery('english', $1), 'MaxWords=50') as snippet, "
        "ts_rank(p.text_vector, plainto_tsquery('english', $1)) as rank "
        "FROM pages p "
        "JOIN files f ON p.file_id = f.id "
        "WHERE p.text_vector @@ plainto_tsquery('english', $1) "
        "ORDER BY rank DESC, f.name, p.page_num "
        "LIMIT 100";

    const char* query = query_get(conn, "q");
    if (!query || strcmp(query, "") == 0) {
        send_json_error(conn, "Missing search query");
        return;
    }

    // Acquire a connection
    pgconn_t* dbconn = pgpool_acquire(pool, TIMEOUT_MS);
    if (!dbconn) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ pgpool_release(pool, dbconn); });

    if (!pgpool_prepare(dbconn, "pdf_query", search_query, 1, NULL, TIMEOUT_MS)) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ pgpool_deallocate(dbconn, "pdf_query", 1000); });

    PGresult* res        = NULL;
    const char* params[] = {query};
    size_t nparams       = sizeof(params) / sizeof(params[0]);
    res = pgpool_execute_prepared(dbconn, "pdf_query", nparams, params, NULL, NULL, 0, TIMEOUT_MS);
    if (!res) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ PQclear(res); });

    // Check result status
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }

    // Read the rows and convert to JSON
    int ntuples = PQntuples(res);

    cJSON* json_response = cJSON_CreateObject();
    if (!json_response) {
        send_json_error(conn, "Failed to create JSON response object");
        return;
    }
    defer({ cJSON_Delete(json_response); });

    cJSON* results_array = cJSON_CreateArray();
    if (!results_array) {
        send_json_error(conn, "Failed to create JSON results array");
        return;
    }

    // Process each result row
    for (int i = 0; i < ntuples; i++) {
        cJSON* result_obj = cJSON_CreateObject();
        if (!result_obj) {
            continue;  // Skip this result on allocation failure
        }

        // Extract values from result row
        const char* file_id_str  = PQgetvalue(res, i, 0);
        const char* file_name    = PQgetvalue(res, i, 1);
        const char* page_num_str = PQgetvalue(res, i, 2);
        const char* snippet      = PQgetvalue(res, i, 3);
        const char* rank_str     = PQgetvalue(res, i, 4);

        // Convert and add to JSON object
        int64_t file_id = strtoll(file_id_str, NULL, 10);
        int page_num    = atoi(page_num_str);
        double rank     = strtod(rank_str, NULL);

        cJSON_AddNumberToObject(result_obj, "file_id", (double)file_id);
        cJSON_AddStringToObject(result_obj, "file_name", file_name ? file_name : "");
        cJSON_AddNumberToObject(result_obj, "page_num", page_num);
        cJSON_AddStringToObject(result_obj, "snippet", snippet ? snippet : "");
        cJSON_AddNumberToObject(result_obj, "rank", rank);

        cJSON_AddItemToArray(results_array, result_obj);
    }

    // Build final response JSON
    cJSON_AddItemToObject(json_response, "results", results_array);
    cJSON_AddNumberToObject(json_response, "count", ntuples);
    cJSON_AddStringToObject(json_response, "query", query);

    // Serialize JSON to string
    char* json_str = cJSON_Print(json_response);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    defer({ free(json_str); });

    // Send JSON response
    conn_send_json(conn, StatusOK, json_str);
}

void list_files(connection_t* conn) {
    const char* query = "SELECT id, name, path FROM files ORDER BY name";

    // Acquire a connection
    pgconn_t* dbconn = pgpool_acquire(pool, TIMEOUT_MS);
    if (!dbconn) {
        send_json_error(conn, "Failed to acquire database connection");
        return;
    }
    defer({ pgpool_release(pool, dbconn); });

    PGresult* res;
    res = pgpool_query(dbconn, query, TIMEOUT_MS);
    if (!res) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ PQclear(res); });

    // Check result status
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }

    // Read the rows and convert to JSON
    int ntuples = PQntuples(res);

    cJSON* results_array = cJSON_CreateArray();
    if (!results_array) {
        send_json_error(conn, "Failed to create JSON results array");
        return;
    }
    defer({ cJSON_Delete(results_array); });

    // Process each result row
    for (int i = 0; i < ntuples; i++) {
        cJSON* result_obj = cJSON_CreateObject();
        if (!result_obj) {
            continue;  // Skip this result on allocation failure
        }

        // Extract values from result row
        const char* file_id_str = PQgetvalue(res, i, 0);
        const char* file_name   = PQgetvalue(res, i, 1);
        const char* file_path   = PQgetvalue(res, i, 2);

        // Convert and add to JSON object
        int64_t file_id = strtoll(file_id_str, NULL, 10);

        cJSON_AddNumberToObject(result_obj, "id", (double)file_id);
        cJSON_AddStringToObject(result_obj, "name", file_name);
        cJSON_AddStringToObject(result_obj, "path", file_path);
        cJSON_AddItemToArray(results_array, result_obj);
    }

    // Serialize JSON to string
    char* json_str = cJSON_Print(results_array);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    defer({ free(json_str); });

    // Send JSON response
    conn_send_json(conn, StatusOK, json_str);
}

void get_file_by_id(connection_t* conn) {
    const char* query    = "SELECT name, path FROM files WHERE id=$1 LIMIT 1";
    const char* stmt     = "get_file_by_id";
    const char* file_id  = get_path_param(conn, "file_id");
    const char* params[] = {file_id};

    int64_t fileId;
    if (str_to_i64(file_id, &fileId) != STO_SUCCESS) {
        send_json_error(conn, "Invalid File ID: Must be a valid integer");
        return;
    }

    // Acquire a connection
    pgconn_t* dbconn = pgpool_acquire(pool, TIMEOUT_MS);
    if (!dbconn) {
        send_json_error(conn, "Failed to acquire database connection");
        return;
    }
    defer({ pgpool_release(pool, dbconn); });

    if (!pgpool_prepare(dbconn, stmt, query, 1, NULL, TIMEOUT_MS)) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ pgpool_deallocate(dbconn, stmt, 1000); });

    PGresult* res;
    res = pgpool_execute_prepared(dbconn, stmt, 1, params, NULL, NULL, 0, TIMEOUT_MS);
    if (!res) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }
    defer({ PQclear(res); });

    // Check result status
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        send_json_error(conn, pgpool_error_message(dbconn));
        return;
    }

    int ntuples = PQntuples(res);
    if (ntuples < 1) {
        conn_set_status(conn, StatusNotFound);
        send_json_error(conn, "No book matches the requested ID");
        return;
    }

    cJSON* result_object = cJSON_CreateObject();
    if (!result_object) {
        send_json_error(conn, "Failed to create JSON results array");
        return;
    }
    defer({ cJSON_Delete(result_object); });

    // Process each result row
    for (int i = 0; i < ntuples; i++) {
        const char* file_name = PQgetvalue(res, i, 0);
        const char* file_path = PQgetvalue(res, i, 1);

        cJSON_AddNumberToObject(result_object, "id", (double)fileId);
        cJSON_AddStringToObject(result_object, "name", file_name);
        cJSON_AddStringToObject(result_object, "path", file_path);
    }

    // Serialize JSON to string
    char* json_str = cJSON_Print(result_object);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    defer({ free(json_str); });
    conn_send_json(conn, StatusOK, json_str);
}
