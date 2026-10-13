#include <errno.h>
#include <lexicon/lexicon_pdf.h>
#include <malloc.h>
#include <math.h>
#include <pulsar/error.h>
#include <solidc/defer.h>
#include <solidc/str_to_num.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vfs.h>

#include "../include/auth.h"
#include "../include/database.h"
#include "../include/error_macros.h"
#include "../include/json_response.h"
#include "../include/ocr.h"
#include "../include/routes.h"
#include "../include/upload.h"
#include "../include/vfs_store.h"

#define handle_record_not_found(res, conn, errmsg) require_row((res), (conn), (errmsg))

/**
 * Opens a document from the VFS if one exists, otherwise uses the host filesystem.
 *
 * @param path       Path to the file.
 * @param num_pages  Receives the page count on success.
 * @param vfs_data  Pointer to write allocated VFS data that must then be freed after
 * pdf_document_close.
 * @return           true on success.
 */
bool gen_open_document(pdf_document_t* doc, const char* path, int* num_pages, void** vfs_data) {
    pdf_status_t status = PDF_OK;
    if (vfs_store_active()) {
        size_t size = 0;
        *vfs_data = vfs_store_read_file(path, &size);
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
 * GET /files/:file_id/pages/:page_num
 *
 * Returns the extracted text of a single PDF page as JSON.
 */
void get_page_text(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    require_path_param_i64(file_id, conn, "file_id");
    require_path_param_i64(page_num, conn, "page_num");

    static const char* query = "SELECT text FROM pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* params[] = {file_id_str, page_num_str};

    acquire_db(db);
    run_query(res, db, query, 2, params);
    char* text = "";
    DEFER_VAR bool heap_allocated = false;

    if (PQntuples(res) == 1) {
        text = PQgetvalue(res, 0, 0);
    } else {
        // ============= parse the text from the document ============
        // 1. Get right path from the database.
        const char* query2 = "SELECT path FROM files WHERE id=$1 LIMIT 1";
        const char* params2[] = {file_id_str};
        run_query(res2, db, query2, 1, params2);
        require_row(res2, conn, "No file matches the provided Identifier");
        const char* path = PQgetvalue(res2, 0, 0);

        open_pdf_document(doc, path, num_pages);
        open_pdf_page(page, doc, page_num - 1, conn);
        pdf_text_t* th = NULL;
        if (pdf_text_open(&page, &th) != PDF_OK) {
            text = "";
        }
        char* utf8 = NULL;
        pdf_text_utf8(th, &utf8);

        // Okay to set because its heap allocated.
        if (utf8 != NULL) {
            text = utf8;
            heap_allocated = true;
        }
    }

    // Free heap allocated text.
    defer {
        if (heap_allocated) {
            free(text);
        }
    };

    StrSlice response = json_create_page_response(&alc, file_id, (int)page_num, text);
    if (!ss_is_valid(response)) {
        send_json_error(conn, "Failed to serialize JSON");
        return;
    }

    // cache for 1 hour since page text rarely changes and is derived from the PDF itself.
    static const char* headers = "Cache-Control: public, max-age=3600\r\n";
    conn_writeheader_raw(conn, headers, strlen(headers));
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
    require_path_param_i64(file_id, conn, "file_id");
    require_path_param_int(page, conn, "page_num");

    /* Extract scale and type from query options */
    float scale = 2.0f;
    const char* scale_str = query_get(conn, "scale");
    if (scale_str) {
        char* endptr = NULL;
        float parsed = strtof(scale_str, &endptr);
        if (endptr != scale_str && parsed > 0.0f) {
            scale = parsed;
        }
    }

    const char* type_str = query_get(conn, "type");
    bool is_png = (type_str && strcasecmp(type_str, "png") == 0);

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};

    acquire_db(db);
    run_query(res, db, query, 1, params);
    require_row(res, conn, "No file found for the requested file");

    const char* path = PQgetvalue(res, 0, 0);
    open_pdf_document(doc, path, num_pages);
    require_page_in_range(page, num_pages, conn);
    open_pdf_page(ppage, doc, page - 1, conn);

    pdf_bitmap_t bmp = {0};
    if (pdf_page_render(&ppage, scale, &bmp) != PDF_OK) {
        send_json_error(conn, "Failed to render PDF page");
        return;
    }
    defer { pdf_bitmap_free(&bmp); };

    uint8_t* img_data = NULL;
    size_t img_size = 0;
    pdf_image_format_t format = is_png ? PDF_IMAGE_PNG : PDF_IMAGE_JPEG;

    /* Encode the rendered bitmap in-memory, avoiding all disk I/O */
    pdf_status_t est = pdf_bitmap_encode(&bmp, format, 88, &img_data, &img_size);
    if (est != PDF_OK || !img_data) {
        send_json_error(conn, "Failed to encode rendered image in memory");
        return;
    }

    char headers[128] = {0};

    // cache for 12 hours since rendered images rarely change and are derived from the PDF itself.
    snprintf(headers, sizeof(headers),
             "Content-Type: %s\r\nCache-Control: public, max-age=43200\r\n",
             is_png ? "image/png" : "image/jpeg");
    conn_writeheader_raw(conn, headers, strlen(headers));
    conn_write(conn, img_data, img_size);
    free(img_data);
}

/**
 * GET /files/:file_id/pages/:page_num/text-layer
 *
 * Returns word-level boxes in PDF point space. Embedded text is used when
 * available; scanned pages are OCRed once and cached in PostgreSQL.
 */

static bool has_native_text(const pdf_char_box_t* boxes, int count) {
    int visible_count = 0;
    for (int i = 0; i < count; i++) {
        const uint32_t cp = boxes[i].codepoint;
        const bool whitespace = cp == 0x20 || (cp >= 0x09 && cp <= 0x0d) || cp == 0x00a0 ||
                                (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
                                cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (!whitespace && boxes[i].right > boxes[i].left && boxes[i].top > boxes[i].bottom &&
            ++visible_count >= 16) {
            return true;
        }
    }
    return false;
}

void get_pdfpage_text_layer(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param_i64(file_id, conn, "file_id");
    require_path_param_int(page, conn, "page_num");

    acquire_db(db);

    static const char* cached_query =
        "SELECT result::text FROM ocr_pages WHERE file_id=$1 AND page_num=$2 LIMIT 1";
    const char* cache_params[] = {file_id_str, page_str};
    run_query(cached_res, db, cached_query, 2, cache_params);
    if (PQntuples(cached_res) == 1) {
        const char* cached_json = PQgetvalue(cached_res, 0, 0);
        static const char* cache_headers = "Cache-Control: public, max-age=43200\r\n";
        conn_writeheader_raw(conn, cache_headers, strlen(cache_headers));
        conn_send_json(conn, StatusOK, cached_json, strlen(cached_json));
        return;
    }

    static const char* query = "SELECT path FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};
    run_query(res, db, query, 1, params);
    require_row(res, conn, "No file found for the requested file");

    const char* path = PQgetvalue(res, 0, 0);
    open_pdf_document(doc, path, num_pages);
    require_page_in_range(page, num_pages, conn);
    open_pdf_page(ppage, doc, page - 1, conn);

    const float page_width = pdf_page_width(&ppage);
    const float page_height = pdf_page_height(&ppage);
    if (!isfinite(page_width) || !isfinite(page_height) || page_width <= 0.0f ||
        page_height <= 0.0f) {
        send_json_error_status(conn, StatusInternalServerError, "Invalid PDF page dimensions");
        return;
    }
    pdf_text_t text = {0};
    if (pdf_text_init(&ppage, &text) != PDF_OK) {
        send_json_error_status(conn, StatusInternalServerError,
                               "Failed to load text layer for page");
        return;
    }
    defer { pdf_text_destroy(&text); };

    const int char_count = pdf_text_char_count(&text);
    pdf_char_box_t* boxes = char_count ? malloc((size_t)char_count * sizeof(*boxes)) : NULL;
    if (char_count && !boxes) {
        send_json_error_status(conn, StatusInternalServerError,
                               "Out of memory extracting text layer");
        return;
    }
    defer { free(boxes); };
    if (char_count && pdf_text_char_boxes(&text, 0, char_count, boxes) != PDF_OK) {
        send_json_error_status(conn, StatusInternalServerError, "Failed to extract text boxes");
        return;
    }

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    size_t out_len = 0;
    char* json = NULL;
    if (has_native_text(boxes, char_count)) {
        json = json_create_text_layer(&alc, page_width, page_height, boxes, char_count, &out_len);
    } else {
        if (!ocr_available()) {
            send_json_error_status(conn, StatusServiceUnavailable,
                                   "OCR is unavailable; install Tesseract and its language data");
            return;
        }

        const float max_pixels = 24.0f * 1024.0f * 1024.0f;
        const float bounded_scale = fminf(4.0f, sqrtf(max_pixels / (page_width * page_height)));
        if (!isfinite(bounded_scale) || bounded_scale <= 0.0f) {
            send_json_error_status(conn, StatusInternalServerError,
                                   "Invalid PDF page dimensions for OCR");
            return;
        }

        pdf_bitmap_t bitmap = {0};
        if (pdf_page_render(&ppage, bounded_scale, &bitmap) != PDF_OK) {
            send_json_error_status(conn, StatusInternalServerError,
                                   "Failed to render page for OCR");
            return;
        }
        defer { pdf_bitmap_free(&bitmap); };

        text_word_t* words = NULL;
        int word_count = 0;
        if (!ocr_page_words(&bitmap, page_width, page_height, &words, &word_count)) {
            send_json_error_status(conn, StatusInternalServerError,
                                   "Tesseract failed to recognize page text");
            return;
        }
        defer { ocr_free_words(words, word_count); };

        json = json_create_text_layer_words(&alc, page_width, page_height, words, word_count,
                                            &out_len);
        if (json) {
            static const char* save_query =
                "INSERT INTO ocr_pages (file_id, page_num, result) "
                "VALUES ($1, $2, $3::jsonb) "
                "ON CONFLICT (file_id, page_num) DO UPDATE SET result=EXCLUDED.result";
            const char* save_params[] = {file_id_str, page_str, json};
            run_query(save_res, db, save_query, 3, save_params);
        }
    }

    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "Failed to serialize text layer");
        return;
    }

    static const char* headers = "Cache-Control: public, max-age=43200\r\n";
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

    static const char common_cte[] =
        "WITH input_queries AS ("
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

    static const char query_suffix[] =
        "  ORDER BY rank DESC LIMIT 100"
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
        snprintf(full_query, sizeof(full_query), "%s AND p.file_id = $2 %s", common_cte,
                 query_suffix);
    } else {
        snprintf(full_query, sizeof(full_query), "%s %s", common_cte, query_suffix);
    }

    acquire_db(db);

    PGresult* res = pgpool_query_params(db, full_query, param_count, params, -1);
    abort_if_nullptr(res, conn, "Error running query for the search");
    defer_pqclear(res);

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
    if (page_str) {
        str_to_int(page_str, &page);
    }
    if (page_size_str) {
        str_to_int(page_size_str, &page_size);
    }

    if (page < 1) {
        page = 1;
    }
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
        if (count_str) {
            str_to_i64(count_str, &total_count);
        }
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
        query =
            "SELECT id, name, path, num_pages FROM files WHERE name ILIKE $1 ORDER BY name LIMIT "
            "$2 OFFSET $3";
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

    run_query(res, db, query, param_count, params);

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
 * GET /api/list-books
 *
 * Returns every indexed PDF file as JSON.
 */
void list_books(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    acquire_db(db);

    static const char* query = "SELECT id, name, path, num_pages FROM files ORDER BY path, name";
    run_query(res, db, query, 0, NULL);

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));
    size_t jsonlen = 0;
    char* json_str = json_create_book_list(&alc, res, &jsonlen);
    if (!json_str) {
        send_json_error(conn, "Failed to serialize JSON response");
        return;
    }

    // Since books are not paginated and rarely change, we can set a long cache duration to reduce
    // load on the server. Here we cache for 24 hours (86400 seconds). The client can still choose
    // to revalidate if needed.
    static const char* headers = "Cache-Control: public, max-age=86400\r\n";
    conn_writeheader_raw(conn, headers, strlen(headers));

    // Send the JSON response with HTTP 200 OK.
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}

/**
 * GET /files/:file_id
 *
 * Returns metadata for a single file as JSON.
 */
void get_file_by_id(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    require_path_param_i64(file_id, conn, "file_id");
    acquire_db(db);

    static const char* query = "SELECT name, path, num_pages FROM files WHERE id=$1 LIMIT 1";
    const char* params[] = {file_id_str};
    run_query(res, db, query, 1, params);
    require_row(res, conn, "No book matches the requested ID");

    const char* file_name = PQgetvalue(res, 0, 0);
    const char* file_path = PQgetvalue(res, 0, 1);
    const char* num_pages_str = PQgetvalue(res, 0, 2);

    int64_t num_pages = 0;
    if (num_pages_str) {
        str_to_i64(num_pages_str, &num_pages);
    }

    yyjson_alc alc;
    yyjson_alc_from_arena(&alc, pulsar_get_arena(conn));

    size_t jsonlen = 0;
    char* json_str =
        json_create_file_response(&alc, file_id, file_name, file_path, num_pages, &jsonlen);
    abort_if_nullptr(json_str, conn, "Failed to serialize JSON");

    // cache for 24 hours since file metadata rarely changes.
    static const char* headers = "Cache-Control: public, max-age=86400\r\n";
    conn_writeheader_raw(conn, headers, strlen(headers));
    conn_send_json(conn, StatusOK, json_str, jsonlen);
}

/**
 * DELETE /api/admin/files/{file_id} (admin only).
 *
 * Removes the file record (pages and cached OCR follow via foreign keys)
 * and deletes the underlying PDF: from disk in host mode, from the image
 * (with a handle swap so readers stop seeing it) in VFS mode.
 */
void delete_file(PulsarCtx* ctx) {
    PulsarConn* conn = ctx->conn;
    int64_t admin_id = 0;
    if (!auth_require_admin(conn, &admin_id)) return;
    (void)admin_id;

    require_path_param_i64(file_id, conn, "file_id");
    if (file_id < 1) {
        send_json_error_status(conn, StatusBadRequest, "invalid file id");
        return;
    }

    acquire_db(db);

    static const char* query = "SELECT path FROM files WHERE id = $1 LIMIT 1";
    const char* params[] = {file_id_str};
    run_query(res, db, query, 1, params);
    require_row(res, conn, "No book matches the requested ID");
    char path[4096];
    snprintf(path, sizeof(path), "%s", PQgetvalue(res, 0, 0));
    if (path[0] == '\0' || strstr(path, "..")) {
        send_json_error_status(conn, StatusBadRequest, "stored file path is invalid");
        return;
    }

    static const char* delete_query = "DELETE FROM files WHERE id = $1";
    run_query(delete_res, db, delete_query, 1, params);
    if (strcmp(PQcmdTuples(delete_res), "1") != 0) {
        send_json_error_status(conn, StatusInternalServerError, "failed to delete file record");
        return;
    }

    const char* image = app_vfs_path();
    if (image && *image) {
        vfs_t* writer = NULL;
        if (vfs_open(image, false, &writer) != VFS_OK || !writer) {
            fprintf(stderr, "delete_file: cannot open VFS image for writing\n");
        } else {
            if (vfs_unlink(writer, path) != VFS_OK) {
                fprintf(stderr, "delete_file: cannot unlink %s from VFS image\n", path);
            }
            vfs_close(writer);
        }
        vfs_t* fresh = NULL;
        if (vfs_open(image, true, &fresh) != VFS_OK || !fresh) {
            fprintf(stderr, "delete_file: cannot reopen VFS image\n");
        } else if (!vfs_store_replace(fresh)) {
            vfs_close(fresh);
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            fprintf(stderr, "delete_file: cannot unlink %s: %s\n", path, strerror(errno));
        }
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
    yyjson_mut_obj_add_int(doc, root, "file_id", file_id);
    size_t out_len = 0;
    char* json = yyjson_mut_write_opts(doc, 0, &alc, &out_len, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error_status(conn, StatusInternalServerError, "failed to serialize response");
        return;
    }
    conn_send_json(conn, StatusOK, json, out_len);
}
