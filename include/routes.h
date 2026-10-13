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
 * Returns word-level boxes in PDF point space for a selectable text overlay.
 * Pages without embedded text are OCRed on demand and cached in PostgreSQL.
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

/**
 * GET /api/list-books
 *
 * Lists every indexed PDF without pagination. Returns {"results": [...],
 * "total_count": N}; each result contains id, name, path, and num_pages.
 */
void list_books(PulsarCtx* ctx);

/** Retrieves a single file by its ID. */
void get_file_by_id(PulsarCtx* ctx);

/**
 * DELETE /api/admin/files/{file_id} (admin only).
 *
 * Removes the file record (pages and cached OCR follow via foreign keys)
 * and deletes the underlying PDF: from disk in host mode, from the image
 * (with a handle swap so readers stop seeing it) in VFS mode.
 */
void delete_file(PulsarCtx* ctx);

#ifdef __cplusplus
}
#endif

#endif  // PDF_SEARCH_ROUTES_H
