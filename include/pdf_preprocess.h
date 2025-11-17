#ifndef PDF_PREPROCESS_H
#define PDF_PREPROCESS_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Preprocesses extracted PDF text by removing common artifacts and normalizing whitespace.
 * @param text The input text to clean. Must be null-terminated and modifiable.
 * @param remove_urls If true, removes URLs from the text.
 * @return Pointer to cleaned text (same buffer as input, modified in-place), or nullptr on error.
 * @note Modifies the input buffer in-place for efficiency.
 */
char* pdf_text_clean(char* text, size_t len, bool remove_urls);

#ifdef __cplusplus
}
#endif

#endif  // PDF_PREPROCESS_H
