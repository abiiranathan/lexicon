#pragma once

#include <pulsar/error.h>
#include <solidc/str_to_num.h>

/* =========================================================================
 * §2b  Integer parsing with built-in error response
 * =========================================================================
 *
 * require_path_param_i64(var, conn, key)
 * require_path_param_int(var, conn, key)
 *
 *   Extracts a path parameter and parses it as int64_t / int, aborting with
 *   400 Bad Request if the parameter is missing or not a valid integer.
 *
 *   Example:
 *     require_path_param_i64(file_id, conn, "file_id");
 *     require_path_param_int(page, conn, "page_num");
 */
#define require_path_param_i64(var, conn, key)                                             \
    require_path_param(var##_str, (conn), key);                                            \
    int64_t var = 0;                                                                       \
    abort_if_lit(str_to_i64((var##_str), &(var)) != STO_SUCCESS, (conn), StatusBadRequest, \
                 "invalid " key ": must be a valid integer")

#define require_path_param_int(var, conn, key)                                             \
    require_path_param(var##_str, (conn), key);                                            \
    int var = 0;                                                                           \
    abort_if_lit(str_to_int((var##_str), &(var)) != STO_SUCCESS, (conn), StatusBadRequest, \
                 "invalid " key ": must be a valid integer")

/* =========================================================================
 * §2c  Database acquisition, query execution, and result-set checks
 * =========================================================================
 *
 * acquire_db(var)
 *
 *   Acquires a pooled connection named `var` from `pool`, aborting with a
 *   503-style JSON error and returning if the pool is exhausted/unavailable.
 *   Registers automatic release via defer.
 *
 *   NOTE: requires `pool` to be visible in the calling scope and
 *   `send_json_error` to be declared.
 */
#define acquire_db(var)                                                          \
    pgconn_t* var = pgpool_acquire(pool, 10000);                                 \
    if (!(var)) {                                                                \
        send_json_error(conn, "No available database connection from the pool"); \
        return;                                                                  \
    }                                                                            \
    defer {                                                                      \
        pgpool_release(pool, (var));                                             \
    }

/* run_query(res_var, db, query, param_count, params)
 *
 *   Executes a parameterized query, registers defer_pqclear, and aborts with
 *   a JSON error (via send_json_error) on query failure. Does NOT check for
 *   an empty result set — pair with require_row() for that.
 *
 *   Example:
 *     run_query(res, db, query, 1, params);
 *     require_row(res, conn, "No file found for the requested file");
 */
#define run_query(res_var, db, query, param_count, params)                               \
    PGresult* res_var = pgpool_query_params((db), (query), (param_count), (params), -1); \
    defer_pqclear(res_var);                                                              \
    if (!(res_var)) {                                                                    \
        send_json_error(conn, pgpool_error_message(db));                                 \
        return;                                                                          \
    }

/* require_row(res, conn, errmsg)
 *
 *   Aborts with 404 + JSON error if `res` has zero rows.
 */
#define require_row(res, conn, errmsg)           \
    if (PQntuples(res) == 0) {                   \
        conn_set_status((conn), StatusNotFound); \
        send_json_error((conn), (errmsg));       \
        return;                                  \
    }

/* =========================================================================
 * §2d  PDF document / page open helpers
 * =========================================================================
 *
 * open_pdf_document(doc_var, path, num_pages_var)
 *
 *   Opens `path` via gen_open_document() (VFS-aware), declaring `doc_var`
 *   (pdf_document_t) and `num_pages_var` (int), and registers cleanup of
 *   both the document and any VFS-backed memory. Aborts with a JSON error
 *   if the document cannot be opened.
 *
 *   NOTE: requires `gen_open_document` to be visible in the calling scope.
 */
#define open_pdf_document(doc_var, path, num_pages_var)                                   \
    pdf_document_t doc_var##_storage = {0};                                               \
    pdf_document_t* doc_var = &doc_var##_storage;                                         \
    int num_pages_var = 0;                                                                \
    void* doc_var##_vfs_data = NULL;                                                      \
    if (!gen_open_document((doc_var), (path), &(num_pages_var), &(doc_var##_vfs_data))) { \
        send_json_error(conn, "Failed to open PDF document");                             \
        return;                                                                           \
    }                                                                                     \
    defer {                                                                               \
        pdf_document_close((doc_var));                                                    \
        free((doc_var##_vfs_data));                                                       \
    }

/* require_page_in_range(page, num_pages, conn)
 *
 *   Aborts with 400 if the 1-based page number is out of [1, num_pages].
 */
#define require_page_in_range(page, num_pages, conn) \
    abort_if_lit((page) < 1 || (page) > (num_pages), (conn), StatusBadRequest, "Page out of range")

/* open_pdf_page(page_var, doc, zero_based_index, conn)
 *
 *   Opens a page at the given zero-based index, declaring `page_var`
 *   (pdf_page_t) and registering its cleanup. Aborts with a JSON error on
 *   failure.
 */
#define open_pdf_page(page_var, doc, zero_based_index, conn)               \
    pdf_page_t page_var = {0};                                             \
    if (pdf_page_open((doc), (zero_based_index), &(page_var)) != PDF_OK) { \
        send_json_error((conn), "Error getting page from PDF");            \
        return;                                                            \
    }                                                                      \
    defer {                                                                \
        pdf_page_close(&(page_var));                                       \
    }
