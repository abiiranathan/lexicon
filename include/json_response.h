#ifndef JSON_RESPONSES_H
#define JSON_RESPONSES_H

#include <lexicon/lexicon_pdf.h>
#include <libpq-fe.h>  // For PGresult
#include <pgpool/pgtypes.h>
#include <solidc/str_slice.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  // for strtoll
#include <string.h>  // for memcpy
#include <yyjson.h>

/**
 * Builds a JSON error object: {"error": "<msg>"}.
 *
 * @param alc     Allocator for the output buffer. See serialize_doc().
 * @param msg     Error message string. Falls back to "Unknown error" if NULL.
 * @param out_len Set to the byte length of the returned string. May be NULL.
 * @return        Caller-owned JSON string. Lifetime governed by @p alc.
 */
char* json_create_error(yyjson_alc* alc, const char* msg, size_t* out_len);

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
StrSlice json_create_page_response(yyjson_alc* alc, int64_t file_id, int page_num, const char* text);

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
                                size_t* out_len);

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
char* json_create_file_list(yyjson_alc* alc, PGresult* res, int page, int limit, int64_t total_count, size_t* out_len);

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
char* json_create_search_results(yyjson_alc* alc, PGresult* res, const char* query, size_t* out_len);

/**
 * Builds a JSON text-layer object for a rendered PDF page:
 *   {"width": ..., "height": ..., "chars": [{"c":N,"l":F,"r":F,"b":F,"t":F}, ...]}
 *
 * Coordinates are in PDF point space, matching pdf_page_width()/height().
 *
 * @param alc         Allocator for the output buffer. See serialize_doc().
 * @param page_width  Page width in PDF points.
 * @param page_height Page height in PDF points.
 * @param boxes       Array of character boxes, as filled by pdf_text_char_boxes().
 * @param count       Number of entries in @p boxes.
 * @param out_len     Set to the byte length of the returned string. May be NULL.
 * @return            JSON string. Lifetime governed by @p alc.
 */
char* json_create_text_layer(yyjson_alc* alc, float page_width, float page_height, const pdf_char_box_t* boxes,
                             int count, size_t* out_len);

#endif  // JSON_RESPONSES_H
