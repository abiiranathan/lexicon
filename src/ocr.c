#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tesseract/capi.h>

#include <solidc/defer.h>

#include "../include/ocr.h"

static _Thread_local TessBaseAPI* worker_ocr_api;
static _Thread_local bool worker_ocr_init_attempted;

static TessBaseAPI* get_worker_ocr_api(void) {
    if (worker_ocr_init_attempted) return worker_ocr_api;
    worker_ocr_init_attempted = true;

    worker_ocr_api = TessBaseAPICreate();
    if (!worker_ocr_api) return NULL;

    const char* language = getenv("LEXICON_OCR_LANGUAGE");
    if (!language || !*language) language = "eng";
    if (TessBaseAPIInit3(worker_ocr_api, NULL, language) != 0) {
        TessBaseAPIDelete(worker_ocr_api);
        worker_ocr_api = NULL;
        return NULL;
    }

    TessBaseAPISetPageSegMode(worker_ocr_api, PSM_AUTO);
    TessBaseAPISetVariable(worker_ocr_api, "preserve_interword_spaces", "1");
    return worker_ocr_api;
}

bool ocr_available(void) {
    return get_worker_ocr_api() != NULL;
}

void ocr_free_words(text_word_t* words, int count) {
    if (!words) return;
    for (int i = 0; i < count; i++) free((void*)words[i].text);
    free(words);
}

bool ocr_page_words(const pdf_bitmap_t* bitmap, float page_width, float page_height,
                    text_word_t** out_words, int* out_count) {
    *out_words = NULL;
    *out_count = 0;

    TessBaseAPI* api = get_worker_ocr_api();
    if (!api) return false;

    if (bitmap->width <= 0 || bitmap->height <= 0 ||
        (size_t)bitmap->stride < (size_t)bitmap->width * 4 ||
        (size_t)bitmap->width > SIZE_MAX / (size_t)bitmap->height) {
        return false;
    }

    const size_t pixel_count = (size_t)bitmap->width * (size_t)bitmap->height;
    uint8_t* grayscale = malloc(pixel_count);
    if (!grayscale) return false;
    defer { free(grayscale); };

    const size_t source_stride = (size_t)bitmap->stride;
    const size_t row_width = (size_t)bitmap->width;
    for (int y = 0; y < bitmap->height; y++) {
        const uint8_t* src = bitmap->pixels + (size_t)y * source_stride;
        uint8_t* dst = grayscale + (size_t)y * row_width;
        for (int x = 0; x < bitmap->width; x++) {
            const uint8_t* pixel = src + (size_t)x * 4;
            dst[x] = (uint8_t)((77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2]) >> 8);
        }
    }

    TessBaseAPISetImage(api, grayscale, bitmap->width, bitmap->height, 1, bitmap->width);
    const float source_ppi = 72.0f * (float)bitmap->width / page_width;
    TessBaseAPISetSourceResolution(api, (int)fmaxf(70.0f, roundf(source_ppi)));
    if (TessBaseAPIRecognize(api, NULL) != 0) {
        TessBaseAPIClear(api);
        return false;
    }

    TessResultIterator* iterator = TessBaseAPIGetIterator(api);
    if (!iterator) {
        TessBaseAPIClear(api);
        return true;
    }

    int capacity = 0;
    text_word_t* words = NULL;
    const TessPageIterator* page_iterator = TessResultIteratorGetPageIteratorConst(iterator);
    if (!page_iterator) {
        TessBaseAPIClear(api);
        return false;
    }

    do {
        char* recognized = TessResultIteratorGetUTF8Text(iterator, RIL_WORD);
        int left = 0, top = 0, right = 0, bottom = 0;
        const bool has_box =
            TessPageIteratorBoundingBox(page_iterator, RIL_WORD, &left, &top, &right, &bottom);
        if (recognized && *recognized && has_box && right > left && bottom > top) {
            if (*out_count == capacity) {
                const int next_capacity = capacity ? capacity * 2 : 64;
                text_word_t* resized = realloc(words, (size_t)next_capacity * sizeof(*words));
                if (!resized) {
                    TessDeleteText(recognized);
                    ocr_free_words(words, *out_count);
                    TessBaseAPIClear(api);
                    return false;
                }
                words = resized;
                capacity = next_capacity;
            }
            char* copy = strdup(recognized);
            if (!copy) {
                TessDeleteText(recognized);
                ocr_free_words(words, *out_count);
                TessBaseAPIClear(api);
                return false;
            }
            const float x_scale = page_width / bitmap->width;
            const float y_scale = page_height / bitmap->height;
            words[*out_count] = (text_word_t){
                .text = copy,
                .left = left * x_scale,
                .right = right * x_scale,
                .bottom = page_height - bottom * y_scale,
                .top = page_height - top * y_scale,
            };
            (*out_count)++;
        }
        if (recognized) TessDeleteText(recognized);
    } while (TessResultIteratorNext(iterator, RIL_WORD));

    *out_words = words;
    TessBaseAPIClear(api);
    return true;
}
