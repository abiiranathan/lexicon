#include "../include/pdf.h"
#include <jerror.h>   // for JMSG_LENGTH_MAX
#include <jpeglib.h>  // for jpeg_compress_struct, jpeg_mem_dest, etc.
#include <setjmp.h>   // for jmp_buf, setjmp, longjmp
#include <solidc/defer.h>
#include <stdint.h>   // for uint32_t
#include <string.h>   // for memcpy, memset

/* ==========================================================================
 * Cairo serialization mutex
 *
 * Poppler's rendering path writes into a Cairo surface; the surface itself
 * is not reference-counted for concurrent writers.  A single global mutex
 * is the correct granularity: we hold it from surface creation through the
 * final cairo_surface_finish / cairo_surface_destroy so that no two threads
 * ever touch Cairo objects simultaneously.
 * ========================================================================== */
/* ==========================================================================
 * Constants
 * ========================================================================== */

/** Rendering resolution for screen display (pixels per inch). */
static const double SCREEN_RESOLUTION = 150.0;

/** JPEG encode quality  */
static const int JPEG_QUALITY = 150;

/** Global mutex serializing all Cairo/Poppler rendering operations. */
static pthread_mutex_t cairo_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Destroys the Cairo mutex at process exit. */
__attribute__((destructor)) static void destroy_cairo_mutex(void) {
    pthread_mutex_destroy(&cairo_mutex);
}

#define LOCK_CAIRO()   pthread_mutex_lock(&cairo_mutex)
#define UNLOCK_CAIRO() pthread_mutex_unlock(&cairo_mutex)

/* ==========================================================================
 * Cairo write callback — plain memory buffer
 * ========================================================================== */

/**
 * Collects raw bytes emitted by a Cairo write stream into a growable heap
 * buffer.  Used for PDF-to-PDF (vector) buffer rendering.
 */
typedef struct {
    unsigned char* data; /** Dynamically allocated buffer; NULL until first write. */
    size_t size;         /** Bytes written so far. */
    size_t capacity;     /** Total allocated capacity. */
} cairo_membuf_t;

/**
 * Cairo write-stream callback that appends @p length bytes of @p data to the
 * buffer held in @p closure.
 *
 * @param closure Pointer to a cairo_membuf_t.
 * @param data    Bytes to append; must not be NULL.
 * @param length  Number of bytes to append.
 * @return CAIRO_STATUS_SUCCESS, or CAIRO_STATUS_WRITE_ERROR on allocation
 *         failure.
 */
static cairo_status_t membuf_write(void* closure, const unsigned char* data, unsigned int length) {
    cairo_membuf_t* buf = (cairo_membuf_t*)closure;

    size_t required = buf->size + (size_t)length;
    if (required > buf->capacity) {
        size_t new_cap = buf->capacity == 0 ? 65536 : buf->capacity * 2;
        while (new_cap < required) {
            new_cap *= 2;
        }

        unsigned char* p = realloc(buf->data, new_cap);
        if (p == NULL) { return CAIRO_STATUS_WRITE_ERROR; }
        buf->data = p;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, data, length);
    buf->size += (size_t)length;
    return CAIRO_STATUS_SUCCESS;
}

/** Releases any memory held by @p buf.  Safe to call on a zero-initialised
 *  struct. */
static void membuf_free(cairo_membuf_t* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

/* ==========================================================================
 * libjpeg error manager
 * ========================================================================== */

/**
 * Extended libjpeg error manager that longjmps on fatal errors rather than
 * calling exit(), allowing the caller to handle errors gracefully.
 */
typedef struct {
    struct jpeg_error_mgr base; /** Must be the first member (libjpeg ABI). */
    jmp_buf escape;             /** Jump target set by the caller via setjmp. */
} jpeg_err_ex_t;

/** @private libjpeg error_exit hook — longjmps into the caller's setjmp. */
static void jpeg_error_exit_ex(j_common_ptr cinfo) {
    jpeg_err_ex_t* err = (jpeg_err_ex_t*)cinfo->err;
    longjmp(err->escape, 1);
}

/* ==========================================================================
 * Document open helpers
 * ========================================================================== */

PopplerDocument* open_document(const char* filename, int* num_pages) {
    if (filename == NULL || num_pages == NULL) {
        fprintf(stderr, "Error: Invalid parameters to open_document\n");
        return NULL;
    }

    PopplerDocument* doc = NULL;
    GBytes* bytes = NULL;

    GFile* file = g_file_new_for_path(filename);
    if (file == NULL) {
        fprintf(stderr, "Error: Could not create GFile for path: %s\n", filename);
        return NULL;
    }

    GError* error = NULL;
    bytes = g_file_load_bytes(file, NULL, NULL, &error);
    g_object_unref(file);

    if (error != NULL) {
        fprintf(stderr, "Error loading file: %s\n", error->message);
        g_clear_error(&error);
        goto out_unref_bytes;
    }

    doc = poppler_document_new_from_bytes(bytes, NULL, &error);
    if (error != NULL) {
        fprintf(stderr, "Error creating Poppler document: %s\n", error->message);
        g_clear_error(&error);
        goto out_unref_bytes;
    }

    *num_pages = poppler_document_get_n_pages(doc);

out_unref_bytes:
    g_bytes_unref(bytes);
    return doc;
}

PopplerDocument* open_vfs_document(vfs_t* vfs, const char* vfs_path, int* num_pages) {
    PopplerDocument* doc = NULL;
    GBytes* bytes = NULL;
    char* buffer = NULL;

    vfs_fd_t fd = vfs_fopen(vfs, vfs_path, VFS_O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error opening VFS path %s: %s\n", vfs_path, vfs_strerror((vfs_status_t)fd));
        return NULL;
    }

    vfs_stat_t st;
    vfs_status_t status = vfs_stat(vfs, vfs_path, &st);
    if (status != VFS_OK) {
        fprintf(stderr, "Error getting stats for VFS path %s: %s\n", vfs_path, vfs_strerror(status));
        goto out_close;
    }

    buffer = g_try_malloc(st.size);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Failed to allocate %lu bytes for VFS read\n", (unsigned long)st.size);
        goto out_close;
    }

    size_t bytes_read = 0;
    status = vfs_fread(vfs, fd, buffer, st.size, &bytes_read);
    if (status != VFS_OK) {
        fprintf(stderr, "vfs_fread failed for %s: %s\n", vfs_path, vfs_strerror(status));
        goto out_free_buffer;
    }

    /* g_bytes_new_take takes ownership of buffer; must not free it on success. */
    bytes = g_bytes_new_take(buffer, st.size);
    buffer = NULL;

    GError* error = NULL;
    doc = poppler_document_new_from_bytes(bytes, NULL, &error);
    if (doc == NULL) {
        if (error != NULL) {
            fprintf(stderr, "Error opening doc from bytes: %s\n", error->message);
            g_clear_error(&error);
        } else {
            fprintf(stderr, "Error opening doc from bytes (unknown error)\n");
        }
        goto out_unref_bytes;
    }

    *num_pages = poppler_document_get_n_pages(doc);

out_unref_bytes:
    g_bytes_unref(bytes);
out_free_buffer:
    g_free(buffer);
out_close:
    vfs_fclose(vfs, fd);
    return doc;
}

/* ==========================================================================
 * Internal rendering primitive
 *
 * Renders a Poppler page into a Cairo RGB24 image surface.  The caller owns
 * the returned surface (and must cairo_surface_destroy it).  The Cairo mutex
 * must NOT be held by the caller; this function acquires and releases it
 * internally for the full rendering operation including surface creation.
 * ========================================================================== */

/**
 * Renders @p page into a newly created Cairo RGB24 image surface.
 *
 * @param page         Poppler page to render.
 * @param pixel_width  Output image width in pixels.
 * @param pixel_height Output image height in pixels.
 * @param pt_width     Page width in PDF points (for scale calculation).
 * @param pt_height    Page height in PDF points (for scale calculation).
 * @return Newly allocated cairo_surface_t on success, NULL on failure.
 *         Caller must cairo_surface_destroy() the returned surface.
 * @note Acquires and releases the global Cairo mutex internally.
 */
static cairo_surface_t* render_page_to_surface(PopplerPage* page, int pixel_width, int pixel_height, double pt_width,
                                               double pt_height) {
    LOCK_CAIRO();

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pixel_width, pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo image surface\n");
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO();
        return NULL;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO();
        return NULL;
    }

    /* White background. */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    /* Scale from PDF points to pixels. */
    cairo_scale(cr, (double)pixel_width / pt_width, (double)pixel_height / pt_height);

    poppler_page_render(page, cr);

    /* Flush ensures pixel data is coherent before we read it. */
    cairo_surface_flush(surface);

    cairo_destroy(cr);

    /* Surface stays alive; caller must destroy it.
     * We hold the lock until the context is destroyed and the surface
     * is flushed — any further access by the caller is to pixel memory
     * that is now stable, so unlocking here is safe. */
    UNLOCK_CAIRO();

    return surface;
}

/* ==========================================================================
 * JPEG rendering — primary path for network delivery
 * ========================================================================== */

/**
 * Renders a Poppler page to a JPEG-encoded in-memory buffer.
 *
 * JPEG produces files 5–10× smaller than PNG at equivalent perceptual quality
 * for rendered document pages, making it the preferred format for HTTP
 * delivery.  The encoded bytes are owned by the caller via @p out_buffer.
 *
 * @param page      Poppler page to render; must not be NULL.
 * @param width     Page width in PDF points (from poppler_page_get_size).
 * @param height    Page height in PDF points.
 * @param out_buffer Output buffer populated on success.  Caller must free
 *                   out_buffer->data with free().
 * @return true on success, false on any rendering or encoding failure.
 * @note Thread-safe: acquires the global Cairo mutex internally.
 */
bool render_page_to_jpeg_buffer(PopplerPage* page, int width, int height, pdf_buffer_t* out_buffer) {
    if (page == NULL || out_buffer == NULL) { return false; }

    int pixel_width = (int)(width * SCREEN_RESOLUTION / 72.0);
    int pixel_height = (int)(height * SCREEN_RESOLUTION / 72.0);

    cairo_surface_t* surface = render_page_to_surface(page, pixel_width, pixel_height, (double)width, (double)height);
    if (surface == NULL) { return false; }

    /* Cairo RGB24 pixel layout: 0x00RRGGBB packed into a uint32_t in native
     * byte order, with stride padding to a 4-byte boundary per row.  We must
     * unpack to a packed 3-byte RGB row that libjpeg expects. */
    const unsigned char* pixels = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);

    /* Scratch buffer for one unpacked RGB scanline. */
    unsigned char* row_rgb = malloc((size_t)pixel_width * 3);
    if (row_rgb == NULL) {
        fprintf(stderr, "Error: Failed to allocate JPEG scanline buffer\n");
        cairo_surface_destroy(surface);
        return false;
    }

    /* --- libjpeg setup ---------------------------------------------------- */
    struct jpeg_compress_struct cinfo;
    jpeg_err_ex_t jerr;
    cinfo.err = jpeg_std_error(&jerr.base);
    jerr.base.error_exit = jpeg_error_exit_ex;

    unsigned char* jpeg_data = NULL;
    unsigned long jpeg_size = 0;
    bool success = false;

    /* libjpeg calls error_exit on fatal errors, which longjmps here. */
    if (setjmp(jerr.escape)) {
        char msg[JMSG_LENGTH_MAX];
        (*cinfo.err->format_message)((j_common_ptr)&cinfo, msg);
        fprintf(stderr, "Error: libjpeg: %s\n", msg);
        jpeg_destroy_compress(&cinfo);
        free(jpeg_data);
        goto cleanup;
    }

    jpeg_create_compress(&cinfo);

    /* jpeg_mem_dest allocates the output buffer via its own allocator.
     * We copy to a malloc-owned buffer afterwards so callers can free()
     * uniformly regardless of how the buffer was produced. */
    jpeg_mem_dest(&cinfo, &jpeg_data, &jpeg_size);

    cinfo.image_width = (JDIMENSION)pixel_width;
    cinfo.image_height = (JDIMENSION)pixel_height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);

    /* 4:2:0 chroma subsampling: visually transparent for document pages and
     * reduces file size by ~15% versus 4:4:4. */
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;

    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint32_t* src = (const uint32_t*)(pixels + (size_t)cinfo.next_scanline * (size_t)stride);

        for (int x = 0; x < pixel_width; x++) {
            uint32_t px = src[x];
            row_rgb[x * 3 + 0] = (unsigned char)((px >> 16) & 0xFF); /* R */
            row_rgb[x * 3 + 1] = (unsigned char)((px >> 8) & 0xFF);  /* G */
            row_rgb[x * 3 + 2] = (unsigned char)((px >> 0) & 0xFF);  /* B */
        }

        JSAMPROW row_ptr = row_rgb;
        jpeg_write_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    /* Copy from libjpeg's allocator to a malloc-owned buffer. */
    out_buffer->data = malloc(jpeg_size);
    if (out_buffer->data == NULL) {
        fprintf(stderr, "Error: Failed to allocate JPEG output buffer\n");
        free(jpeg_data);
        goto cleanup;
    }
    memcpy(out_buffer->data, jpeg_data, jpeg_size);
    out_buffer->size = (size_t)jpeg_size;
    free(jpeg_data);
    success = true;

cleanup:
    free(row_rgb);
    cairo_surface_destroy(surface);
    return success;
}

/**
 * Renders a page from an open document to a JPEG buffer.
 *
 * @param doc      Open Poppler document; must not be NULL.
 * @param page_num Zero-based page index.
 * @param out_buffer Output buffer populated on success.
 * @return true on success, false if the page cannot be retrieved or rendered.
 */
bool render_page_from_document_to_jpeg_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer) {
    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) { return false; }

    double width, height;
    poppler_page_get_size(page, &width, &height);
    bool result = render_page_to_jpeg_buffer(page, (int)width, (int)height, out_buffer);
    g_object_unref(page);
    return result;
}

/* ==========================================================================
 * PNG rendering — lossless path, useful for archival or high-fidelity use
 * ========================================================================== */

/**
 * Renders a Poppler page to a PNG-encoded in-memory buffer.
 *
 * Prefer render_page_to_jpeg_buffer for network delivery.  This function
 * exists for use-cases where lossless pixel-perfect output is required
 * (e.g. archival, diffing, thumbnail generation for cover pages).
 *
 * @param page      Poppler page to render; must not be NULL.
 * @param width     Page width in PDF points.
 * @param height    Page height in PDF points.
 * @param out_buffer Output buffer populated on success.  Caller must free
 *                   out_buffer->data with free().
 * @return true on success, false on failure.
 * @note Thread-safe: acquires the global Cairo mutex internally.
 */
bool render_page_to_png_buffer(PopplerPage* page, int width, int height, pdf_buffer_t* out_buffer) {
    if (page == NULL || out_buffer == NULL) { return false; }

    int pixel_width = (int)(width * SCREEN_RESOLUTION / 72.0);
    int pixel_height = (int)(height * SCREEN_RESOLUTION / 72.0);

    cairo_surface_t* surface = render_page_to_surface(page, pixel_width, pixel_height, (double)width, (double)height);
    if (surface == NULL) { return false; }

    cairo_membuf_t buf = {.data = NULL, .size = 0, .capacity = 0};

    /* cairo_surface_write_to_png_stream operates on already-rendered pixel
     * data and does not touch Cairo drawing state, so no lock is needed. */
    cairo_status_t status = cairo_surface_write_to_png_stream(surface, membuf_write, &buf);
    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: cairo_surface_write_to_png_stream: %s\n", cairo_status_to_string(status));
        membuf_free(&buf);
        return false;
    }

    out_buffer->data = buf.data;
    out_buffer->size = buf.size;
    return true;
}

/**
 * Renders a page from an open document to a PNG buffer.
 *
 * @param doc      Open Poppler document; must not be NULL.
 * @param page_num Zero-based page index.
 * @param out_buffer Output buffer populated on success.
 * @return true on success, false on failure.
 */
bool render_page_from_document_to_png_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer) {
    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) { return false; }

    double width, height;
    poppler_page_get_size(page, &width, &height);
    bool result = render_page_to_png_buffer(page, (int)width, (int)height, out_buffer);
    g_object_unref(page);
    return result;
}

/* ==========================================================================
 * PNG file output — debugging / offline rendering
 * ========================================================================== */

/**
 * Renders a Poppler page to a PNG file on disk.
 *
 * Intended for debugging and offline use.  For HTTP delivery use
 * render_page_to_jpeg_buffer instead.
 *
 * @param page        Poppler page to render; must not be NULL.
 * @param width       Page width in PDF points.
 * @param height      Page height in PDF points.
 * @param output_file Destination file path for the PNG.
 */
void render_page_to_image(PopplerPage* page, int width, int height, const char* output_file) {
    if (page == NULL || output_file == NULL) { return; }

    int pixel_width = (int)(width * SCREEN_RESOLUTION / 72.0);
    int pixel_height = (int)(height * SCREEN_RESOLUTION / 72.0);

    cairo_surface_t* surface = render_page_to_surface(page, pixel_width, pixel_height, (double)width, (double)height);
    if (surface == NULL) { return; }

    cairo_status_t status = cairo_surface_write_to_png(surface, output_file);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Could not write PNG file '%s': %s\n", output_file, cairo_status_to_string(status));
    }

    cairo_surface_destroy(surface);
}

/**
 * Renders a page from an open document to a PNG file.
 *
 * @param doc        Open Poppler document; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param output_png Destination file path.
 * @return true on success, false if the page cannot be retrieved.
 */
bool render_page_from_document(PopplerDocument* doc, int page_num, const char* output_png) {
    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) { return false; }

    double width, height;
    poppler_page_get_size(page, &width, &height);
    render_page_to_image(page, (int)width, (int)height, output_png);
    g_object_unref(page);
    return true;
}

/* ==========================================================================
 * Vector PDF buffer rendering
 * ========================================================================== */

/**
 * Renders a Poppler page to a PDF-encoded in-memory buffer using Cairo's PDF
 * surface.  This preserves vector content rather than rasterising it, and
 * produces the smallest possible output for text-heavy documents.
 *
 * @param page       Poppler page to render; must not be NULL.
 * @param out_buffer Output buffer populated on success.  Caller must free
 *                   out_buffer->data with free().
 * @return true on success, false on failure.
 * @note Thread-safe: acquires the global Cairo mutex internally.
 */
bool render_page_to_pdf_buffer(PopplerPage* page, pdf_buffer_t* out_buffer) {
    if (page == NULL || out_buffer == NULL) { return false; }

    double width, height;
    poppler_page_get_size(page, &width, &height);

    cairo_membuf_t buf = {.data = NULL, .size = 0, .capacity = 0};

    LOCK_CAIRO();

    cairo_surface_t* surface = cairo_pdf_surface_create_for_stream(membuf_write, &buf, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo PDF surface\n");
        cairo_surface_destroy(surface);
        membuf_free(&buf);
        UNLOCK_CAIRO();
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        membuf_free(&buf);
        UNLOCK_CAIRO();
        return false;
    }

    /* Render vector content; no raster scaling needed for PDF-to-PDF. */
    poppler_page_render(page, cr);

    /* cairo_surface_finish flushes all pending stream writes to membuf_write
     * before we inspect the buffer; must be called while holding the lock. */
    cairo_surface_finish(surface);
    cairo_status_t status = cairo_surface_status(surface);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    UNLOCK_CAIRO();

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Cairo PDF surface finish failed: %s\n", cairo_status_to_string(status));
        membuf_free(&buf);
        return false;
    }

    out_buffer->data = buf.data;
    out_buffer->size = buf.size;
    return true;
}

/**
 * Renders a page from an open document to a PDF buffer.
 *
 * @param doc      Open Poppler document; must not be NULL.
 * @param page_num Zero-based page index.
 * @param out_buffer Output buffer populated on success.
 * @return true on success, false on failure.
 */
bool render_page_from_document_to_pdf_buffer(PopplerDocument* doc, int page_num, pdf_buffer_t* out_buffer) {
    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) { return false; }

    bool result = render_page_to_pdf_buffer(page, out_buffer);
    g_object_unref(page);
    return result;
}

/* ==========================================================================
 * Single-page PDF file output
 * ========================================================================== */

/**
 * Writes a single Poppler page to a PDF file on disk via Cairo.
 *
 * @param page       Poppler page to render; must not be NULL.
 * @param output_pdf Destination file path for the PDF.
 * @return true on success, false on failure.
 */
bool poppler_page_to_pdf(PopplerPage* page, const char* output_pdf) {
    if (page == NULL || output_pdf == NULL) { return false; }

    double width, height;
    poppler_page_get_size(page, &width, &height);

    LOCK_CAIRO();

    cairo_surface_t* surface = cairo_pdf_surface_create(output_pdf, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo PDF surface for '%s'\n", output_pdf);
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO();
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO();
        return false;
    }

    poppler_page_render(page, cr);
    cairo_surface_finish(surface);
    cairo_status_t status = cairo_surface_status(surface);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    UNLOCK_CAIRO();

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Cairo PDF surface finish failed: %s\n", cairo_status_to_string(status));
        return false;
    }

    return true;
}

/**
 * Renders a page from an open document to a PDF file.
 *
 * @param doc        Open Poppler document; must not be NULL.
 * @param page_num   Zero-based page index.
 * @param output_pdf Destination file path.
 * @return true on success, false on failure.
 */
bool render_page_to_pdf(PopplerDocument* doc, int page_num, const char* output_pdf) {
    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) { return false; }

    bool result = poppler_page_to_pdf(page, output_pdf);
    g_object_unref(page);
    return result;
}

/* ==========================================================================
 * PDF metadata
 * ========================================================================== */

/** @private Formats a time_t as a "YYYY-MM-DD HH:MM:SS" string.
 *  Returns NULL if @p t is 0 or allocation fails. */
static char* format_time_property(time_t t) {
    if (t == 0) { return NULL; }

    char* buf = malloc(32);
    if (buf == NULL) { return NULL; }

    struct tm* tm_info = localtime(&t);
    strftime(buf, 32, "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

void get_pdf_metadata(PopplerDocument* doc, int num_pages, pdf_metadata_t* out_meta) {
    memset(out_meta, 0, sizeof(pdf_metadata_t));

    out_meta->page_count = num_pages;
    out_meta->is_encrypted = FALSE;

    const char* ver = poppler_document_get_pdf_version_string(doc);
    out_meta->pdf_version = ver != NULL ? strdup(ver) : NULL;

    /* Poppler returns newly allocated strings for these; we own them. */
    out_meta->title = poppler_document_get_title(doc);
    out_meta->author = poppler_document_get_author(doc);
    out_meta->subject = poppler_document_get_subject(doc);
    out_meta->keywords = poppler_document_get_keywords(doc);
    out_meta->creator = poppler_document_get_creator(doc);
    out_meta->producer = poppler_document_get_producer(doc);

    out_meta->creation_date = format_time_property(poppler_document_get_creation_date(doc));
    out_meta->mod_date = format_time_property(poppler_document_get_modification_date(doc));
}

void free_pdf_metadata(pdf_metadata_t* meta) {
    if (meta == NULL) { return; }
    free(meta->title);
    free(meta->author);
    free(meta->subject);
    free(meta->keywords);
    free(meta->creator);
    free(meta->producer);
    free(meta->creation_date);
    free(meta->mod_date);
    free(meta->pdf_version);
    memset(meta, 0, sizeof(pdf_metadata_t));
}
