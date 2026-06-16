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
void get_page_text(PulsarCtx* ctx);

/**
 * GET /api/file/{file_id}/render-page/{page_num}
 *
 * Renders a single PDF page to an in-memory compressed image and streams it.
 * Optional query parameters:
 *   - scale: float scaling factor (default: 2.0)
 *   - type: format type, "png" or "jpg" (default: "jpg")
 */
void render_pdfpage_as_image(PulsarCtx* ctx);

/**
 * GET /api/file/{file_id}/text-layer/{page_num}
 *
 * Returns per-character bounding boxes for a PDF page, for building a
 * selectable text overlay on top of a separately-rendered image. Boxes
 * are in PDF point space; the client must scale them using the same
 * `scale` factor passed to the render endpoint.
 */
void get_pdfpage_text_layer(PulsarCtx* ctx);

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
