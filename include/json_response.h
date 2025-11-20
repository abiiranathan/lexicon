#ifndef JSON_RESPONSES_H
#define JSON_RESPONSES_H

#include <libpq-fe.h>  // For PGresult
#include <stddef.h>
#include <stdint.h>

// Returns a malloc'd JSON string. Caller must free.
char* json_create_error(const char* msg);

// Returns a malloc'd JSON string. Caller must free.
char* json_create_page_response(int64_t file_id, int page_num, const char* text);

// Returns a malloc'd JSON string. Caller must free.
char* json_create_file_response(int64_t id, const char* name, const char* path, int64_t num_pages, size_t* out_len);

// Returns a malloc'd JSON string. Caller must free.
char* json_create_file_list(PGresult* res, int page, int limit, int64_t total_count, size_t* out_len);

// Returns a malloc'd JSON string. Caller must free.
// 'res' is the PGresult from the search query.
// 'ai_summary' can be NULL.
char* json_create_search_results(PGresult* res, const char* query, const char* ai_summary, size_t* out_len);

#endif  // JSON_RESPONSES_H
