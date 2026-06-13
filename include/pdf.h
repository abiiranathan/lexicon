#ifndef PDF_H
#define PDF_H

#include <cairo/cairo-pdf.h>
#include <cairo/cairo.h>
#include <poppler/glib/poppler.h>
#include <pthread.h>  // for pthread_mutex_t (used in implementation)
#include <stdbool.h>  // for bool
#include <stdio.h>    // for fprintf, stderr
#include <stdlib.h>   // for free
#include <time.h>     // for time_t
#include <vfs.h>

/* ==========================================================================
 * Types
 * ========================================================================== */

/**
 * In-memory buffer holding encoded page data (JPEG, PNG, or PDF).
 * Ownership of @p data transfers to the caller on successful render;
 * free with free().
 */
typedef struct {
    unsigned char* data; /** Encoded bytes. Caller must free() after use. */
    size_t size;         /** Length of @p data in bytes. */
} pdf_buffer_t;

/**
 * Metadata extracted from a PDF document.
 * All string fields are heap-allocated and owned by the caller.
 * Free the entire struct with free_pdf_metadata().
 */
typedef struct {
    char* title;         /** Document title, or NULL if absent. */
    char* author;        /** Document author, or NULL if absent. */
    char* subject;       /** Document subject, or NULL if absent. */
    char* keywords;      /** Document keywords, or NULL if absent. */
    char* creator;       /** Creating application, or NULL if absent. */
    char* producer;      /** PDF producer, or NULL if absent. */
    char* creation_date; /** Creation date as "YYYY-MM-DD HH:MM:SS", or NULL. */
    char* mod_date;      /** Modification date as "YYYY-MM-DD HH:MM:SS", or NULL. */
    char* pdf_version;   /** PDF version string (e.g. "1.7"), or NULL. */
    int page_count;      /** Total number of pages in the document. */
    bool is_encrypted;   /** Whether the document is encrypted. */
} pdf_metadata_t;

/* ==========================================================================
 * Document lifecycle
 * ========================================================================== */

/**
 * Opens a PDF file from disk and returns a PopplerDocument.
 *
 * @param filename  Path to the PDF file; must not be NULL.
 * @param num_pages Output parameter receiving the total page count.
 * @return Pointer to PopplerDocument on success, NULL on failure.
 * @note Caller must g_object_unref() the returned document when done.
 * @note Thread-safe.
 */
PopplerDocument* open_document(const char* filename, int* num_pages);

/**
 * Opens a PDF file from a VFS and returns a PopplerDocument.
 *
 * @param vfs      Initialised VFS instance; must not be NULL.
 * @param vfs_path Path to the PDF file within the VFS; must not be NULL.
 * @param num_pages Output parameter receiving the total page count.
 * @return Pointer to PopplerDocument on success, NULL on failure.
 * @note Caller must g_object_unref() the returned document when done.
 * @note Thread-safe.
 */
PopplerDocument* open_vfs_document(vfs_t* vfs, const char* vfs_path, int* num_pages);

/* ==========================================================================
 * JPEG rendering — primary path for HTTP delivery
 *
 * JPEG at 96 DPI produces 25–80 KB per page versus 200–500 KB for PNG at
 * 150 DPI, making it the correct format for serving pages over the network.
 * ========================================================================== */

/**
 * Renders a Poppler page to a JPEG-encoded in-memory buffer.
 *
 * @param page       Page to render; must not be NULL.
 * @param width      Page width in PDF points (from poppler_page_get_size).
 * @param height     Page height in PDF points.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false on any rendering or encoding failure.
 * @note Thread-safe: acquires an internal Cairo mutex for the render step.
 */
bool render_page_to_jpeg_buffer(PopplerPage* page, int width, int height, pdf_buffer_t* out_buffer);

/**
 * Renders a page from an open document to a JPEG buffer.
 *
 * @param doc        Open PopplerDocument; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false if the page cannot be retrieved or rendered.
 * @note Thread-safe.
 */
bool render_page_from_document_to_jpeg_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer);

/* ==========================================================================
 * PNG rendering — lossless path for archival / high-fidelity use
 * ========================================================================== */

/**
 * Renders a Poppler page to a PNG-encoded in-memory buffer.
 *
 * Prefer the JPEG path for network delivery. Use this where lossless,
 * pixel-perfect output is required (archival, diffing, cover thumbnails).
 *
 * @param page       Page to render; must not be NULL.
 * @param width      Page width in PDF points.
 * @param height     Page height in PDF points.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false on failure.
 * @note Thread-safe: acquires an internal Cairo mutex for the render step.
 */
bool render_page_to_png_buffer(PopplerPage* page, int width, int height, pdf_buffer_t* out_buffer);

/**
 * Renders a page from an open document to a PNG buffer.
 *
 * @param doc        Open PopplerDocument; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false on failure.
 * @note Thread-safe.
 */
bool render_page_from_document_to_png_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer);

/* ==========================================================================
 * PNG file output — offline / debugging
 * ========================================================================== */

/**
 * Renders a Poppler page to a PNG file on disk.
 *
 * Intended for debugging and offline use. For HTTP delivery, use
 * render_page_to_jpeg_buffer instead.
 *
 * @param page        Page to render; must not be NULL.
 * @param width       Page width in PDF points.
 * @param height      Page height in PDF points.
 * @param output_file Destination PNG file path; must not be NULL.
 * @note Thread-safe.
 */
void render_page_to_image(PopplerPage* page, int width, int height, const char* output_file);

/**
 * Renders a page from an open document to a PNG file.
 *
 * @param doc        Open PopplerDocument; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param output_png Destination PNG file path; must not be NULL.
 * @return true on success, false if the page cannot be retrieved.
 * @note Thread-safe.
 */
bool render_page_from_document(PopplerDocument* doc, int page_num, const char* output_png);

/* ==========================================================================
 * Vector PDF buffer rendering
 * ========================================================================== */

/**
 * Renders a Poppler page to a PDF-encoded in-memory buffer via Cairo's PDF
 * surface, preserving vector content rather than rasterising it.
 *
 * Produces the smallest output for text-heavy documents and avoids quality
 * loss from rasterisation.
 *
 * @param page       Page to render; must not be NULL.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false on failure.
 * @note Thread-safe: acquires an internal Cairo mutex for the full operation,
 *       including the stream-flush on cairo_surface_finish.
 */
bool render_page_to_pdf_buffer(PopplerPage* page, pdf_buffer_t* out_buffer);

/**
 * Renders a page from an open document to a PDF buffer.
 *
 * @param doc        Open PopplerDocument; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param out_buffer Populated on success; caller must free out_buffer->data.
 * @return true on success, false on failure.
 * @note Thread-safe.
 */
bool render_page_from_document_to_pdf_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer);

/* ==========================================================================
 * Single-page PDF file output
 * ========================================================================== */

/**
 * Writes a single Poppler page to a PDF file on disk via Cairo.
 *
 * @param page       Page to render; must not be NULL.
 * @param output_pdf Destination PDF file path; must not be NULL.
 * @return true on success, false on failure.
 * @note Thread-safe.
 */
bool poppler_page_to_pdf(PopplerPage* page, const char* output_pdf);

/**
 * Renders a page from an open document to a PDF file.
 *
 * @param doc        Open PopplerDocument; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param output_pdf Destination PDF file path; must not be NULL.
 * @return true on success, false on failure.
 * @note Thread-safe.
 */
bool render_page_to_pdf(PopplerDocument* doc, int page_num, const char* output_pdf);

/* ==========================================================================
 * PDF metadata
 * ========================================================================== */

/**
 * Extracts metadata from an open document into @p out_meta.
 *
 * All string fields in @p out_meta are heap-allocated. Call
 * free_pdf_metadata() when done to release them.
 *
 * @param doc       Open PopplerDocument; must not be NULL.
 * @param num_pages Total page count (as returned by open_document).
 * @param out_meta  Output struct to populate; must not be NULL.
 */
void get_pdf_metadata(PopplerDocument* doc, int num_pages, pdf_metadata_t* out_meta);

/**
 * Frees all heap-allocated string fields in @p meta and zeroes the struct.
 * Safe to call on a partially populated struct or NULL.
 *
 * @param meta Pointer to the metadata struct to free; may be NULL.
 */
void free_pdf_metadata(pdf_metadata_t* meta);

#endif /* PDF_H */
