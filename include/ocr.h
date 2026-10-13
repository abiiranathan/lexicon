#ifndef LEXICON_OCR_H
#define LEXICON_OCR_H

#include <stdbool.h>

#include "json_response.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns true when the worker-thread Tesseract engine is usable.
 *
 * Lazily creates and initializes one engine per thread on first use.
 * The recognition language comes from LEXICON_OCR_LANGUAGE (default "eng").
 */
bool ocr_available(void);

/**
 * Recognizes words on a rendered PDF page bitmap.
 *
 * Boxes are rescaled from pixels to PDF points using the page dimensions.
 * On success stores a heap-allocated word array in out_words/out_count,
 * owned by the caller (release with ocr_free_words). Returns false on any
 * failure, including an unavailable engine; out_words/out_count are then
 * left as NULL/0.
 */
bool ocr_page_words(const pdf_bitmap_t* bitmap, float page_width, float page_height,
                    text_word_t** out_words, int* out_count);

/** Frees a word array produced by ocr_page_words. */
void ocr_free_words(text_word_t* words, int count);

#ifdef __cplusplus
}
#endif

#endif  // LEXICON_OCR_H
