#ifndef PDF_SEARCH_ROUTES_H
#define PDF_SEARCH_ROUTES_H

#include <pulsar/pulsar.h>
#include <stddef.h>
#include <stdint.h>
#include "json_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns full text for a given file_id and page_num.
 * Path: /api/file/{file_id}/page/{page_num}
 */
void get_page_by_file_and_page(PulsarCtx* ctx);

// Renders a PDF page as PNG image.
void render_pdfpage_as_image(PulsarCtx* ctx);

/**
 * Performs full-text search on PDF pages.
 * Returns matching results with snippets, ranked by relevance.
 * Fetches complete page content and sends to Gemini for AI-powered answers.
 */
void pdf_search(PulsarCtx* ctx);

/**
 * Lists all PDF files in the database paginated on query params "page" & limit.
 * Returns JSON object with keys: "page", "limit", "results", "has_next", "has_prev","total_count",
 * "total_pages".
 */
void list_files(PulsarCtx* ctx);

/** Retrieves a single file by its ID. */
void get_file_by_id(PulsarCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif  // PDF_SEARCH_ROUTES_H
