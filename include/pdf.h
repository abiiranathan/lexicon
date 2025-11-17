#ifndef __PDF_H__
#define __PDF_H__

#include <cairo/cairo-pdf.h>
#include <cairo/cairo.h>
#include <locale.h>
#include <poppler/glib/poppler.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>  // for gzip compression

/**
 * Opens a PDF document and returns a PopplerDocument object.
 *
 * @param filename Path to the PDF file to open.
 * @param num_pages Output parameter that receives the total number of pages in the document.
 * @return Pointer to PopplerDocument on success, NULL on failure (file not found, invalid PDF, etc.).
 * @note Caller must call g_object_unref() on the returned document when done.
 * @note Thread-safe.
 */
PopplerDocument* open_document(const char* filename, int* num_pages);

/**
 * Renders a Poppler page to a PDF file using Cairo.
 *
 * @param page The PopplerPage to render. Must not be NULL.
 * @param output_pdf Path where the output PDF will be written.
 * @return true on success, false on failure (invalid page, I/O error, Cairo error).
 * @note Thread-safe. Uses internal mutex to serialize Cairo operations.
 * @note Output PDF is rendered at 300 DPI resolution.
 */
bool poppler_page_to_pdf(PopplerPage* page, const char* output_pdf);

/**
 * Renders a PDF page to a PNG image file.
 *
 * @param page The PopplerPage to render. Must not be NULL.
 * @param width Width of the page in points (72 DPI).
 * @param height Height of the page in points (72 DPI).
 * @param output_file Path where the output PNG will be written.
 * @note Thread-safe. Uses internal mutex to serialize Cairo operations.
 * @note Renders at 300 DPI resolution with white background and no text antialiasing.
 * @note On error, prints diagnostic message to stdout.
 */
void render_page_to_image(PopplerPage* page, int width, int height, const char* output_file);

/**
 * Structure to hold in-memory PNG image data.
 * Caller is responsible for freeing the data buffer.
 */
typedef struct {
    /** Raw PNG image data. Caller must free using free(). */
    unsigned char* data;
    /** Size of the image data in bytes. */
    size_t size;
} png_buffer_t;

/**
 * Renders a PDF page to a PNG image in memory.
 *
 * @param page The PopplerPage to render. Must not be NULL.
 * @param width Width of the page in points (72 DPI).
 * @param height Height of the page in points (72 DPI).
 * @param out_buffer Output parameter that receives the PNG data. Must not be NULL.
 * @return true on success, false on failure (invalid page, allocation error, Cairo error).
 * @note Thread-safe. Uses internal mutex to serialize Cairo operations.
 * @note Renders at 300 DPI resolution with white background and no text antialiasing.
 * @note Caller must free out_buffer->data using free() when done.
 * @note On failure, out_buffer is left unmodified.
 */
bool render_page_to_buffer(PopplerPage* page, int width, int height, png_buffer_t* out_buffer);

/**
 * Renders a PDF page to a gzip-compressed PNG buffer.
 *
 * @param page The Poppler page to render.
 * @param width The target width in points (72 DPI units).
 * @param height The target height in points (72 DPI units).
 * @param out_buffer Output buffer that will contain gzip-compressed PNG data.
 * @return true on success, false on failure.
 * @note Caller must free out_buffer->data using free() when done.
 */
bool render_page_to_compressed_buffer(PopplerPage* page, int width, int height, png_buffer_t* out_buffer);

/**
 * Renders a single page from a PDF document to a PNG file.
 * This is a convenience function that opens the document, extracts the page,
 * renders it, and cleans up - all in one call to minimize CGo overhead.
 *
 * @param pdf_path Path to the input PDF file.
 * @param page_num Zero-based page number to render.
 * @param output_png Path where the output PNG will be written.
 * @return true on success, false on failure (file not found, invalid page number, render error).
 * @note Thread-safe.
 * @note Automatically determines page dimensions and renders at 300 DPI.
 */
bool render_page_from_document(const char* pdf_path, int page_num, const char* output_png);

/**
 * Renders a single page from a PDF document to a PNG buffer in memory.
 * This is a convenience function that opens the document, extracts the page,
 * renders it to memory, and cleans up - all in one call to minimize CGo overhead.
 *
 * @param pdf_path Path to the input PDF file.
 * @param page_num Zero-based page number to render.
 * @param out_buffer Output parameter that receives the PNG data. Must not be NULL.
 * @return true on success, false on failure (file not found, invalid page number, render error).
 * @note Thread-safe.
 * @note Automatically determines page dimensions and renders at 300 DPI.
 * @note Caller must free out_buffer->data using free() when done.
 */
bool render_page_from_document_to_buffer(const char* pdf_path, int page_num, png_buffer_t* out_buffer);

/**
 * Renders a single page from a PDF document to a PDF file.
 * This is a convenience function that opens the document, extracts the page,
 * renders it, and cleans up - all in one call to minimize CGo overhead.
 *
 * @param pdf_path Path to the input PDF file.
 * @param page_num Zero-based page number to render.
 * @param output_pdf Path where the output PDF will be written.
 * @return true on success, false on failure (file not found, invalid page number, render error).
 * @note Thread-safe.
 * @note Output PDF is rendered at 300 DPI resolution.
 */
bool render_page_to_pdf(const char* pdf_path, int page_num, const char* output_pdf);

#endif /* __PDF_H__ */
