#include "../include/pdf.h"
#include <string.h>  // for memcpy

/** Global mutex to serialize Cairo operations (Cairo is not fully thread-safe). */
static pthread_mutex_t cairo_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Cleanup function to destroy the mutex on program exit. */
__attribute__((destructor)) void destroy_cairo_mutex(void) {
    pthread_mutex_destroy(&cairo_mutex);
}

#define LOCK_CAIRO_MUTEX()   pthread_mutex_lock(&cairo_mutex)
#define UNLOCK_CAIRO_MUTEX() pthread_mutex_unlock(&cairo_mutex)

/** Default rendering resolution in DPI. */
static const double DEFAULT_RESOLUTION = 300.0;

PopplerDocument* open_document(const char* filename, int* num_pages) {
    if (filename == NULL || num_pages == NULL) {
        fprintf(stderr, "Error: Invalid parameters to open_document\n");
        return NULL;
    }

    GFile* file = g_file_new_for_path(filename);
    if (file == NULL) {
        fprintf(stderr, "Error: Could not create GFile for path: %s\n", filename);
        return NULL;
    }

    GError* error = NULL;
    GBytes* bytes = g_file_load_bytes(file, NULL, NULL, &error);
    g_object_unref(file);

    if (error != NULL) {
        fprintf(stderr, "Error loading file: %s\n", error->message);
        g_clear_error(&error);
        return NULL;
    }

    PopplerDocument* doc = poppler_document_new_from_bytes(bytes, NULL, &error);
    if (error != NULL) {
        fprintf(stderr, "Error creating Poppler document: %s\n", error->message);
        g_clear_error(&error);
        g_bytes_unref(bytes);
        return NULL;
    }

    *num_pages = poppler_document_get_n_pages(doc);
    g_bytes_unref(bytes);
    return doc;
}

void render_page_to_image(PopplerPage* page, int width, int height, const char* output_file) {
    if (page == NULL || output_file == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_to_image\n");
        return;
    }

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: Invalid dimensions (%d x %d)\n", width, height);
        return;
    }

    // Calculate pixel dimensions at the target resolution
    int pixel_width  = (int)(width * DEFAULT_RESOLUTION / 72.0);
    int pixel_height = (int)(height * DEFAULT_RESOLUTION / 72.0);

    LOCK_CAIRO_MUTEX();

    // Create the Cairo image surface
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_width, pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo surface\n");
        UNLOCK_CAIRO_MUTEX();
        return;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO_MUTEX();
        return;
    }

    // Set white background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Disable text antialiasing for sharper rendering
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    // Scale to maintain aspect ratio
    double scale_x = (double)pixel_width / width;
    double scale_y = (double)pixel_height / height;
    cairo_scale(cr, scale_x, scale_y);

    // Render the PDF page
    poppler_page_render(page, cr);

    UNLOCK_CAIRO_MUTEX();

    // Write to PNG file
    cairo_status_t status = cairo_surface_write_to_png(surface, output_file);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Could not write PNG file: %s\n", cairo_status_to_string(status));
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

/**
 * Internal structure used for collecting PNG data from Cairo's write callback.
 */
typedef struct {
    unsigned char* data; /** Dynamically allocated buffer. */
    size_t size;         /** Current size of used data. */
    size_t capacity;     /** Total allocated capacity. */
} cairo_write_context_t;

/**
 * Cairo write callback that appends data to a dynamically growing buffer.
 *
 * @param closure Pointer to cairo_write_context_t.
 * @param data Data to write.
 * @param length Length of data in bytes.
 * @return CAIRO_STATUS_SUCCESS on success, CAIRO_STATUS_WRITE_ERROR on allocation failure.
 */
static cairo_status_t cairo_write_to_buffer(void* closure, const unsigned char* data, unsigned int length) {
    cairo_write_context_t* ctx = (cairo_write_context_t*)closure;

    // Ensure we have enough capacity
    size_t required = ctx->size + length;
    if (required > ctx->capacity) {
        // Grow buffer by doubling capacity
        size_t new_capacity = ctx->capacity == 0 ? 4096 : ctx->capacity * 2;
        while (new_capacity < required) {
            new_capacity *= 2;
        }

        unsigned char* new_data = realloc(ctx->data, new_capacity);
        if (new_data == NULL) {
            return CAIRO_STATUS_WRITE_ERROR;
        }

        ctx->data     = new_data;
        ctx->capacity = new_capacity;
    }

    // Copy data to buffer
    memcpy(ctx->data + ctx->size, data, length);
    ctx->size += length;

    return CAIRO_STATUS_SUCCESS;
}

bool render_page_to_buffer(PopplerPage* page, int width, int height, png_buffer_t* out_buffer) {
    if (page == NULL || out_buffer == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_to_buffer\n");
        return false;
    }

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: Invalid dimensions (%d x %d)\n", width, height);
        return false;
    }

    // Calculate pixel dimensions at the target resolution
    int pixel_width  = (int)(width * DEFAULT_RESOLUTION / 72.0);
    int pixel_height = (int)(height * DEFAULT_RESOLUTION / 72.0);

    LOCK_CAIRO_MUTEX();

    // Create the Cairo image surface
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_width, pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo surface\n");
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    // Set white background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Disable text antialiasing for sharper rendering
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    // Scale to maintain aspect ratio
    double scale_x = (double)pixel_width / width;
    double scale_y = (double)pixel_height / height;
    cairo_scale(cr, scale_x, scale_y);

    // Render the PDF page
    poppler_page_render(page, cr);

    UNLOCK_CAIRO_MUTEX();

    // Initialize write context for collecting PNG data
    cairo_write_context_t write_ctx = {.data = NULL, .size = 0, .capacity = 0};

    // Write PNG to memory buffer
    cairo_status_t status = cairo_surface_write_to_png_stream(surface, cairo_write_to_buffer, &write_ctx);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Could not write PNG to buffer: %s\n", cairo_status_to_string(status));
        free(write_ctx.data);
        return false;
    }

    // Transfer ownership to caller
    out_buffer->data = write_ctx.data;
    out_buffer->size = write_ctx.size;

    return true;
}

bool render_page_from_document(const char* pdf_path, int page_num, const char* output_png) {
    if (pdf_path == NULL || output_png == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_from_document\n");
        return false;
    }

    int num_pages        = 0;
    PopplerDocument* doc = open_document(pdf_path, &num_pages);
    if (doc == NULL) {
        fprintf(stderr, "Error: Could not open document: %s\n", pdf_path);
        return false;
    }

    if (page_num < 0 || page_num >= num_pages) {
        fprintf(stderr, "Error: Page number %d is out of range (0-%d)\n", page_num, num_pages - 1);
        g_object_unref(doc);
        return false;
    }

    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) {
        fprintf(stderr, "Error: Could not get page %d\n", page_num);
        g_object_unref(doc);
        return false;
    }

    double width, height;
    poppler_page_get_size(page, &width, &height);

    render_page_to_image(page, (int)width, (int)height, output_png);

    g_object_unref(page);
    g_object_unref(doc);
    return true;
}

bool render_page_from_document_to_buffer(const char* pdf_path, int page_num, png_buffer_t* out_buffer) {
    if (pdf_path == NULL || out_buffer == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_from_document_to_buffer\n");
        return false;
    }

    int num_pages        = 0;
    PopplerDocument* doc = open_document(pdf_path, &num_pages);
    if (doc == NULL) {
        fprintf(stderr, "Error: Could not open document: %s\n", pdf_path);
        return false;
    }

    if (page_num < 0 || page_num >= num_pages) {
        fprintf(stderr, "Error: Page number %d is out of range (0-%d)\n", page_num, num_pages - 1);
        g_object_unref(doc);
        return false;
    }

    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) {
        fprintf(stderr, "Error: Could not get page %d\n", page_num);
        g_object_unref(doc);
        return false;
    }

    double width, height;
    poppler_page_get_size(page, &width, &height);

    bool result = render_page_to_buffer(page, (int)width, (int)height, out_buffer);

    g_object_unref(page);
    g_object_unref(doc);
    return result;
}

bool poppler_page_to_pdf(PopplerPage* page, const char* output_pdf) {
    if (page == NULL || output_pdf == NULL) {
        fprintf(stderr, "Error: Invalid parameters to poppler_page_to_pdf\n");
        return false;
    }

    double width, height;
    poppler_page_get_size(page, &width, &height);

    int pixel_width  = (int)(width * DEFAULT_RESOLUTION / 72.0);
    int pixel_height = (int)(height * DEFAULT_RESOLUTION / 72.0);

    LOCK_CAIRO_MUTEX();

    cairo_surface_t* surface = cairo_pdf_surface_create(output_pdf, pixel_width, pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create PDF surface\n");
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    // Set white background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Scale to maintain aspect ratio
    cairo_scale(cr, (double)pixel_width / width, (double)pixel_height / height);

    // Render the PDF page
    poppler_page_render(page, cr);

    cairo_surface_finish(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    UNLOCK_CAIRO_MUTEX();
    return true;
}

bool render_page_to_pdf(const char* pdf_path, int page_num, const char* output_pdf) {
    if (pdf_path == NULL || output_pdf == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_to_pdf\n");
        return false;
    }

    int num_pages        = 0;
    PopplerDocument* doc = open_document(pdf_path, &num_pages);
    if (doc == NULL) {
        fprintf(stderr, "Error: Could not open document: %s\n", pdf_path);
        return false;
    }

    if (page_num < 0 || page_num >= num_pages) {
        fprintf(stderr, "Error: Page number %d is out of range (0-%d)\n", page_num, num_pages - 1);
        g_object_unref(doc);
        return false;
    }

    PopplerPage* page = poppler_document_get_page(doc, page_num);
    if (page == NULL) {
        fprintf(stderr, "Error: Could not get page %d\n", page_num);
        g_object_unref(doc);
        return false;
    }

    bool status = poppler_page_to_pdf(page, output_pdf);

    g_object_unref(page);
    g_object_unref(doc);
    return status;
}

// GZIP compression of DOCUMENT into PNG
//====================================================
/**
 * Context structure for streaming gzip compression during Cairo PNG writing.
 */
typedef struct {
    z_stream stream;     /** zlib compression stream state. */
    unsigned char* data; /** Dynamically allocated compressed buffer. */
    size_t size;         /** Current size of compressed data. */
    size_t capacity;     /** Total allocated capacity. */
    bool initialized;    /** Whether zlib stream has been initialized. */
    bool error_occurred; /** Flag to track if compression error occurred. */
} cairo_gzip_write_context_t;

/**
 * Cairo write callback that compresses PNG data with gzip on-the-fly.
 *
 * @param closure Pointer to cairo_gzip_write_context_t.
 * @param data PNG data chunk to compress.
 * @param length Length of data in bytes.
 * @return CAIRO_STATUS_SUCCESS on success, CAIRO_STATUS_WRITE_ERROR on failure.
 */
static cairo_status_t cairo_write_to_gzip_buffer(void* closure, const unsigned char* data, unsigned int length) {
    cairo_gzip_write_context_t* ctx = (cairo_gzip_write_context_t*)closure;

    if (ctx->error_occurred) {
        return CAIRO_STATUS_WRITE_ERROR;
    }

    // Initialize deflate stream on first call
    if (!ctx->initialized) {
        ctx->stream = (z_stream){0};
        // windowBits = 15 + 16 for gzip format
        int ret = deflateInit2(&ctx->stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            fprintf(stderr, "Error: Failed to initialize gzip compression: %d\n", ret);
            ctx->error_occurred = true;
            return CAIRO_STATUS_WRITE_ERROR;
        }
        ctx->initialized = true;

        // Initialize buffer with reasonable starting capacity
        ctx->capacity = 8192;
        ctx->data     = malloc(ctx->capacity);
        if (ctx->data == NULL) {
            deflateEnd(&ctx->stream);
            ctx->error_occurred = true;
            return CAIRO_STATUS_WRITE_ERROR;
        }
    }

    // Set input for this chunk
    ctx->stream.next_in  = (unsigned char*)data;
    ctx->stream.avail_in = length;

    // Compress input data
    while (ctx->stream.avail_in > 0) {
        // Ensure we have output space
        if (ctx->size >= ctx->capacity) {
            size_t new_capacity     = ctx->capacity * 2;
            unsigned char* new_data = realloc(ctx->data, new_capacity);
            if (new_data == NULL) {
                ctx->error_occurred = true;
                return CAIRO_STATUS_WRITE_ERROR;
            }
            ctx->data     = new_data;
            ctx->capacity = new_capacity;
        }

        ctx->stream.next_out  = ctx->data + ctx->size;
        ctx->stream.avail_out = (uInt)(ctx->capacity - ctx->size);

        int ret = deflate(&ctx->stream, Z_NO_FLUSH);
        if (ret != Z_OK) {
            fprintf(stderr, "Error: Compression failed during deflate: %d\n", ret);
            ctx->error_occurred = true;
            return CAIRO_STATUS_WRITE_ERROR;
        }

        // Update size based on how much was written
        ctx->size = ctx->capacity - ctx->stream.avail_out;
    }

    return CAIRO_STATUS_SUCCESS;
}

/**
 * Finalizes gzip compression and ensures all data is flushed.
 *
 * @param ctx The compression context to finalize.
 * @return true on success, false on failure.
 */
static bool finalize_gzip_compression(cairo_gzip_write_context_t* ctx) {
    if (!ctx->initialized || ctx->error_occurred) {
        return false;
    }

    // Finish compression (flush all remaining data)
    ctx->stream.avail_in = 0;
    int ret;
    do {
        // Ensure we have output space
        if (ctx->size >= ctx->capacity) {
            size_t new_capacity     = ctx->capacity * 2;
            unsigned char* new_data = realloc(ctx->data, new_capacity);
            if (new_data == NULL) {
                return false;
            }
            ctx->data     = new_data;
            ctx->capacity = new_capacity;
        }

        ctx->stream.next_out  = ctx->data + ctx->size;
        ctx->stream.avail_out = (uInt)(ctx->capacity - ctx->size);

        ret       = deflate(&ctx->stream, Z_FINISH);
        ctx->size = ctx->capacity - ctx->stream.avail_out;

        if (ret != Z_STREAM_END && ret != Z_OK) {
            fprintf(stderr, "Error: Compression failed during finalization: %d\n", ret);
            return false;
        }
    } while (ret != Z_STREAM_END);

    deflateEnd(&ctx->stream);
    return true;
}

bool render_page_to_compressed_buffer(PopplerPage* page, int width, int height, png_buffer_t* out_buffer) {
    if (page == NULL || out_buffer == NULL) {
        fprintf(stderr, "Error: Invalid parameters to render_page_to_compressed_buffer\n");
        return false;
    }

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: Invalid dimensions (%d x %d)\n", width, height);
        return false;
    }

    // Calculate pixel dimensions at the target resolution
    int pixel_width  = (int)(width * DEFAULT_RESOLUTION / 72.0);
    int pixel_height = (int)(height * DEFAULT_RESOLUTION / 72.0);

    LOCK_CAIRO_MUTEX();

    // Create the Cairo image surface
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_width, pixel_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo surface\n");
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to create Cairo context\n");
        cairo_surface_destroy(surface);
        UNLOCK_CAIRO_MUTEX();
        return false;
    }

    // Set white background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Disable text antialiasing for sharper rendering
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

    // Scale to maintain aspect ratio
    double scale_x = (double)pixel_width / width;
    double scale_y = (double)pixel_height / height;
    cairo_scale(cr, scale_x, scale_y);

    // Render the PDF page
    poppler_page_render(page, cr);

    UNLOCK_CAIRO_MUTEX();

    // Initialize gzip compression context
    cairo_gzip_write_context_t write_ctx = {
        .stream = {0}, .data = NULL, .size = 0, .capacity = 0, .initialized = false, .error_occurred = false};

    // Write PNG to compressed buffer via streaming callback
    cairo_status_t status = cairo_surface_write_to_png_stream(surface, cairo_write_to_gzip_buffer, &write_ctx);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Could not write PNG to buffer: %s\n", cairo_status_to_string(status));
        if (write_ctx.initialized) {
            deflateEnd(&write_ctx.stream);
        }
        free(write_ctx.data);
        return false;
    }

    // Finalize compression (flush remaining data)
    if (!finalize_gzip_compression(&write_ctx)) {
        fprintf(stderr, "Error: Failed to finalize gzip compression\n");
        free(write_ctx.data);
        return false;
    }

    // Transfer ownership to caller
    out_buffer->data = write_ctx.data;
    out_buffer->size = write_ctx.size;
    printf("Sending buffer of size: %zu\n", out_buffer->size);
    return true;
}
