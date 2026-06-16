#include <lexicon/lexicon_pdf.h>
#include <malloc.h>
#include <pulsar/error.h>
#include <solidc/defer.h>
#include <solidc/str_to_num.h>
#include <vfs.h>

#include "../include/database.h"
#include "../include/json_response.h"
#include "../include/routes.h"

extern vfs_t* vfs;  // Instance of VFS defined in main.c.

#define acquire_db(var)                                \
    pgconn_t* var = pgpool_acquire(pool, 1000);        \
    if (!(var)) {                                      \
        send_json_error(conn, "Database unavailable"); \
        return;                                        \
    }                                                  \
    defer {                                            \
        pgpool_release(pool, (var));                   \
    };

#define handle_record_not_found(res, conn, errmsg) \
    if (PQntuples(res) == 0) {                     \
        conn_set_status(conn, StatusNotFound);     \
        send_json_error(conn, errmsg);             \
        return;                                    \
    }

#define handle_query_error(res, conn)                    \
    if (!(res)) {                                        \
        send_json_error(conn, pgpool_error_message(db)); \
        return;                                          \
    }

/**
 * Opens a document from the VFS if one exists, otherwise uses the host filesystem.
 *
 * @param path       Path to the file.
 * @param num_pages  Receives the page count on success.
 * @param vfs_data  Pointer to write allocated VFS data that must then be freed after pdf_document_close.
 * @return           true on success.
 */
bool gen_open_document(pdf_document_t* doc, const char* path, int* num_pages, void** vfs_data) {
    pdf_status_t status = PDF_OK;
    if (vfs != NULL) {
        size_t size = 0;
        *vfs_data = vfs_read_file(vfs, path, &size);
        if (!*vfs_data) return false;
        // takes ownership of the data but must be freed after pdf_document_close.
        status = pdf_document_open_mem(doc, *vfs_data, size, NULL);
    } else {
        status = pdf_document_open(doc, path, NULL);
    }

    if (status == PDF_OK) {
        *num_pages = pdf_document_page_count(doc);
        return true;
    }
    return false;
}

/**
 * Sends a JSON-encoded error body with HTTP 400 Bad Request.
 *
 * @param conn PulsarConn to write the response to.
 * @param msg  Human-readable error message; must be NUL-terminated.
 */
static inline void send_json_error(PulsarConn* conn, const char* msg) {
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    size_t outlen;
    char* err_str = json_create_error(&alc, msg, &outlen);
    if (err_str) {
        conn_send_json(conn, StatusBadRequest, err_str, outlen);
    } else {
        conn_send_json(conn, StatusInternalServerError, "{\"error\":\"Internal error\"}", 26);
    }
}

/**
 * GET /files/:file_id/pages/:page_num
 *
 * Returns the extracted text of a single PDF page as JSON.
 */
void get_page_by_file_and_page(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    const char* file_id_str = get_path_param(conn, "file_id");
    const char* page_num_str = get_path_param(conn, "page_num");

    int64_t file_id = 0;
    int64_t page_num = 0;

    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID");
        return;
    }
    if (str_to_i64(page_num_str, &page_num) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number");
        return;
    }

    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[] = {file_id_str, page_num_str};

    acquire_db(db);
    PGresult* res = pgpool_query_params(db, query, 2, params, -1);
    defer_pqclear(res);
    handle_query_error(res, conn);
    handle_record_not_found(res, conn, "No page found for the requested file and page number");

    const char* text = PQgetvalue(res, 0, 0);
    StrSlice response = json_create_page_response(&alc, file_id, (int)page_num, text);
    if (!ss_is_valid(response)) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }
    conn_send_json(conn, StatusOK, response.data, response.len);
}

/**
 * GET /files/:file_id/pages/:page_num/render
 *
 * Renders a single PDF page to an in-memory compressed image and streams it.
 * Optional query parameters:
 *   - scale: float scaling factor (default: 2.0)
 *   - type: format type, "png" or "jpg" (default: "jpg")
 */
void render_pdfpage_as_image(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param(file_id_str, conn, "file_id");
    require_path_param(page_num_str, conn, "page_num");

    int64_t file_id = 0;
    int page = 0;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_int(page_num_str, &page) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    /* Extract scale and type from query options */
    float scale = 2.0f;
    const char* scale_str = query_get(conn, "scale");
    if (scale_str) {
        char* endptr = NULL;
        float parsed = strtof(scale_str, &endptr);
        if (endptr != scale_str && parsed > 0.0f) { scale = parsed; }
    }

    const char* type_str = query_get(conn, "type");
    bool is_png = (type_str && strcasecmp(type_str, "png") == 0);

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    acquire_db(db);
    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    defer_pqclear(res);

    handle_query_error(res, conn);
    handle_record_not_found(res, conn, "No file found for the requested file");

    const char* path = PQgetvalue(res, 0, 0);
    printf("Serving page %d of file: %s\n", page, path);

    int num_pages = 0;
    pdf_document_t doc_var = {0};
    pdf_document_t* doc = &doc_var;
    void* vfs_data = NULL;
    if (!gen_open_document(doc, path, &num_pages, &vfs_data)) {
        send_json_error(conn, "Failed to open PDF document");
        return;
    }

    defer {
        pdf_document_close(doc);
        free(vfs_data);
    };

    if (page < 1 || page > num_pages) {
        send_json_error(conn, "Page out of range");
        return;
    }

    printf("Opening PDF page\n");
    pdf_page_t ppage = {0};
    if (pdf_page_open(doc, page - 1, &ppage) != PDF_OK) {
        send_json_error(conn, "Error getting page from PDF");
        return;
    }
    defer {
        pdf_page_close(&ppage);
    };

    pdf_bitmap_t bmp = {0};
    if (pdf_page_render(&ppage, scale, &bmp) != PDF_OK) {
        send_json_error(conn, "Failed to render PDF page");
        return;
    }
    defer {
        pdf_bitmap_free(&bmp);
    };

    uint8_t* img_data = NULL;
    size_t img_size = 0;
    pdf_image_format_t format = is_png ? PDF_IMAGE_PNG : PDF_IMAGE_JPEG;

    /* Encode the rendered bitmap in-memory, avoiding all disk I/O */
    pdf_status_t est = pdf_bitmap_encode(&bmp, format, 88, &img_data, &img_size);
    if (est != PDF_OK || !img_data) {
        send_json_error(conn, "Failed to encode rendered image in memory");
        return;
    }

    char headers[128];
    snprintf(headers, sizeof(headers), "Content-Type: %s\r\nCache-Control: public, max-age=3600\r\n",
             is_png ? "image/png" : "image/jpeg");
    conn_writeheader_raw(conn, headers, strlen(headers));
    conn_write(conn, img_data, img_size);
    free(img_data);
}

/**
 * GET /files/:file_id/pages/:page_num/text-layer
 *
 * Returns per-character bounding boxes for a PDF page, for building a
 * selectable text overlay on top of a separately-rendered image. Boxes
 * are in PDF point space; the client must scale them using the same
 * `scale` factor passed to the render endpoint.
 */
void get_pdfpage_text_layer(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param(file_id_str, conn, "file_id");
    require_path_param(page_num_str, conn, "page_num");

    int64_t file_id = 0;
    int page = 0;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID: must be a valid integer");
        return;
    }

    if (str_to_int(page_num_str, &page) != STO_SUCCESS) {
        send_json_error(conn, "Invalid page number: must be a valid integer");
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    acquire_db(db);
    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    defer_pqclear(res);

    handle_query_error(res, conn);
    handle_record_not_found(res, conn, "No file found for the requested file");

    const char* path = PQgetvalue(res, 0, 0);

    int num_pages = 0;
    pdf_document_t doc_var = {0};
    pdf_document_t* doc = &doc_var;
    void* vfs_data = NULL;
    if (!gen_open_document(doc, path, &num_pages, &vfs_data)) {
        send_json_error(conn, "Failed to open PDF document");
        return;
    }

    defer {
        pdf_document_close(doc);
        free(vfs_data);
    };

    if (page < 1 || page > num_pages) {
        send_json_error(conn, "Page out of range");
        return;
    }

    pdf_page_t ppage = {0};
    if (pdf_page_open(doc, page - 1, &ppage) != PDF_OK) {
        send_json_error(conn, "Error getting page from PDF");
        return;
    }
    defer {
        pdf_page_close(&ppage);
    };

    pdf_text_t text = {0};
    if (pdf_text_init(&ppage, &text) != PDF_OK) {
        send_json_error(conn, "Failed to load text layer for page");
        return;
    }
    defer {
        pdf_text_destroy(&text);
    };

    yyjson_alc alc;
    Arena* arena = conn->arena;
    yyjson_alc_from_arena(&alc, arena);

    int char_count = pdf_text_char_count(&text);
    pdf_char_box_t* boxes = NULL;

    if (char_count > 0) {
        boxes = arena_alloc(arena, (size_t)char_count * sizeof(*boxes));
        if (boxes == NULL) {
            send_json_error(conn, "Out of memory extracting text layout");
            return;
        }
        if (pdf_text_char_boxes(&text, 0, char_count, boxes) != PDF_OK) {
            send_json_error(conn, "Failed to extract character boxes");
            return;
        }
    }

    size_t out_len = 0;
    char* json = json_create_text_layer(&alc, pdf_page_width(&ppage), pdf_page_height(&ppage), boxes, char_count,
                                        &out_len);
    if (json == NULL) {
        send_json_error(conn, "Failed to serialize text layout");
        return;
    }

    static const char* headers = "Content-Type: application/json\r\nCache-Control: public, max-age=3600\r\n";
    conn_writeheader_raw(conn, headers, strlen(headers));
    conn_write(conn, json, out_len);
}

/**
 * GET /search?q=<query>[&file_id=<id>]
 *
 * Full-text search across all pages, optionally scoped to a single file.
 */
void pdf_search(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    const char* file_id = query_get(conn, "file_id");
    const char* query_str = query_get(conn, "q");

    const char* params[2];
    int param_count = 1;
    params[0] = query_str;

    if (file_id) {
        params[1] = file_id;
        param_count = 2;
    }

    static const char common_cte
        [] = "WITH input_queries AS ("
             "  SELECT"
             "    websearch_to_tsquery('english', $1) AS broad_query,"
             "    phraseto_tsquery('english', $1)     AS phrase_query"
             "),"
             "RankedPages AS ("
             "  SELECT"
             "    p.file_id, p.page_num,"
             "    ("
             "      ts_rank_cd(p.text_vector, inputs.broad_query)"
             "      + (CASE WHEN p.text_vector @@ inputs.phrase_query THEN 10.0 ELSE 0.0 END)"
             "    ) AS rank"
             "  FROM pages p"
             "  CROSS JOIN input_queries inputs"
             "  WHERE p.text_vector @@ inputs.broad_query";

    static const char query_suffix
        [] = "  ORDER BY rank DESC LIMIT 100"
             "),"
             "UniquePages AS ("
             "  SELECT DISTINCT ON (file_id, page_num) file_id, page_num, rank"
             "  FROM RankedPages ORDER BY file_id, page_num, rank DESC"
             ")"
             "SELECT"
             "  u.file_id, f.name, f.num_pages, u.page_num,"
             "  ts_headline('english', p.text, inputs.broad_query,"
             "    'StartSel=<b>, StopSel=</b>, MaxWords=200, MinWords=20') AS snippet,"
             "  LEFT(p.text, 2000) AS extended_snippet,"
             "  u.rank"
             " FROM UniquePages u"
             " CROSS JOIN input_queries inputs"
             " JOIN files f ON u.file_id = f.id"
             " JOIN pages p ON u.file_id = p.file_id AND u.page_num = p.page_num"
             " ORDER BY u.rank DESC, f.name, u.page_num LIMIT 100";

    char full_query[4096];
    if (file_id) {
        snprintf(full_query, sizeof(full_query), "%s AND p.file_id = $2 %s", common_cte, query_suffix);
    } else {
        snprintf(full_query, sizeof(full_query), "%s %s", common_cte, query_suffix);
    }

    acquire_db(db);

    PGresult* res = NULL;
    TIME_BLOCK("pdfsearch", { res = pgpool_query_params(db, full_query, param_count, params, -1); });
    defer_pqclear(res);

    handle_query_error(res, conn);

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    size_t jsonlen = 0;
    char* json_str = json_create_search_results(&alc, res, query_str, &jsonlen);
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}

/**
 * GET /files?page=<n>&limit=<n>[&name=<filter>]
 *
 * Returns a paginated JSON list of all files.
 */
void list_files(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;

    const char* page_str = query_get(conn, "page");
    const char* page_size_str = query_get(conn, "limit");
    const char* name = query_get(conn, "name");

    int page = 1;
    int page_size = 10;
    if (page_str) { str_to_int(page_str, &page); }
    if (page_size_str) { str_to_int(page_size_str, &page_size); }

    if (page < 1) { page = 1; }
    if (page_size < 1) {
        page_size = 25;
    } else if (page_size > 100) {
        page_size = 100;
    }

    acquire_db(db);

    static const char* count_query = "SELECT COUNT(*) FROM files";
    PGresult* count_res = pgpool_query(db, count_query, 5000);
    if (!count_res) {
        send_json_error(conn, pgpool_error_message(db));
        return;
    }
    defer_pqclear(count_res);

    int64_t total_count = 0;
    if (PQntuples(count_res) > 0) {
        const char* count_str = PQgetvalue(count_res, 0, 0);
        if (count_str) { str_to_i64(count_str, &total_count); }
    }

    int offset = (page - 1) * page_size;
    const char* query = NULL;
    int param_count = 0;
    const char* params[3];
    char limit_str[32];
    char offset_str[32];
    snprintf(limit_str, sizeof(limit_str), "%d", page_size);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    char name_filter[128] = {0};
    if (name) {
        snprintf(name_filter, sizeof(name_filter), "%%%s%%", name);
        query = "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT $2 OFFSET $3";
        params[0] = name_filter;
        params[1] = limit_str;
        params[2] = offset_str;
        param_count = 3;
    } else {
        query = "SELECT id, name, path, num_pages FROM files ORDER BY name LIMIT $1 OFFSET $2";
        params[0] = limit_str;
        params[1] = offset_str;
        param_count = 2;
    }

    PGresult* res = pgpool_query_params(db, query, param_count, params, -1);
    handle_query_error(res, conn);
    defer_pqclear(res);

    size_t jsonlen = 0;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    char* json_str = json_create_file_list(&alc, res, page, page_size, total_count, &jsonlen);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}

/**
 * GET /files/:file_id
 *
 * Returns metadata for a single file as JSON.
 */
void get_file_by_id(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    const char* file_id_str = get_path_param(conn, "file_id");

    int64_t file_id = 0;
    if (str_to_i64(file_id_str, &file_id) != STO_SUCCESS) {
        send_json_error(conn, "Invalid file ID");
        return;
    }

    acquire_db(db);

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    PGresult* res = pgpool_query_params(db, query, 1, params, -1);
    handle_query_error(res, conn);
    defer_pqclear(res);

    handle_record_not_found(res, conn, "No book matches the requested ID");

    const char* file_name = PQgetvalue(res, 0, 0);
    const char* file_path = PQgetvalue(res, 0, 1);
    const char* num_pages_str = PQgetvalue(res, 0, 2);

    int64_t num_pages = 0;
    if (num_pages_str) { str_to_i64(num_pages_str, &num_pages); }

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    size_t jsonlen = 0;
    char* json_str = json_create_file_response(&alc, file_id, file_name, file_path, num_pages, &jsonlen);
    abort_if_nullptr(json_str, conn, "Failed to serialize JSON");
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}
