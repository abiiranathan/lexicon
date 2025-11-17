#include <assert.h>
#include <ctype.h>    // for isspace, isprint, isalnum
#include <stdbool.h>  // for bool
#include <stdio.h>    // for printf
#include <stdlib.h>   // for malloc, free
#include <string.h>   // for strlen, memmove, strdup, strncmp

/**
 * Additional filter to remove common PDF header/footer artifacts.
 * @param text The cleaned text buffer.
 * @return The same buffer, potentially further modified.
 * @note Should be called after pdf_text_preprocess().
 */
static char* pdf_remove_headers_footers(char* text, size_t len) {
    assert(text && len > 0);

    // Remove common page number patterns at start/end
    // Pattern: "Page N of M" or just "N" at the beginning

    // Check for page numbers at the start
    if (len > 0 && isdigit((unsigned char)text[0])) {
        size_t i = 0;
        while (i < len && (isdigit((unsigned char)text[i]) || isspace((unsigned char)text[i]))) {
            i++;
        }

        // If we found only digits/spaces in first 10 chars, it's likely a page number
        if (i > 0 && i < 10) {
            memmove(text, text + i, len - i + 1);
            len = strlen(text);
        }
    }

    return text;
}

/**
 * Validates and sanitizes text to ensure it contains only valid UTF-8.
 * Replaces invalid sequences with the Unicode replacement character (�).
 *
 * @param text The text buffer to sanitize (modified in-place).
 * @param len Length of the text buffer.
 * @return Length of the sanitized text.
 */
static size_t sanitize_utf8(char* text, size_t len) {
    if (!text || len == 0) {
        return 0;
    }

    size_t write_pos = 0;
    size_t read_pos  = 0;

    while (read_pos < len && text[read_pos] != '\0') {
        unsigned char byte = (unsigned char)text[read_pos];

        // ASCII character (0x00-0x7F)
        if (byte <= 0x7F) {
            // Skip control characters except tab, newline, carriage return
            if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') {
                read_pos++;
                continue;
            }
            text[write_pos++] = text[read_pos++];
        }
        // 2-byte UTF-8 sequence (0xC2-0xDF)
        else if (byte >= 0xC2 && byte <= 0xDF) {
            if (read_pos + 1 < len) {
                unsigned char byte2 = (unsigned char)text[read_pos + 1];
                // Valid continuation byte (0x80-0xBF)
                if (byte2 >= 0x80 && byte2 <= 0xBF) {
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    continue;
                }
            }
            // Invalid sequence - skip it
            read_pos++;
        }
        // 3-byte UTF-8 sequence (0xE0-0xEF)
        else if (byte >= 0xE0 && byte <= 0xEF) {
            if (read_pos + 2 < len) {
                unsigned char byte2 = (unsigned char)text[read_pos + 1];
                unsigned char byte3 = (unsigned char)text[read_pos + 2];

                bool valid = false;
                if (byte == 0xE0) {
                    // Overlong encoding check
                    valid = (byte2 >= 0xA0 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF);
                } else if (byte == 0xED) {
                    // Surrogate pairs check
                    valid = (byte2 >= 0x80 && byte2 <= 0x9F) && (byte3 >= 0x80 && byte3 <= 0xBF);
                } else {
                    valid = (byte2 >= 0x80 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF);
                }

                if (valid) {
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    continue;
                }
            }
            // Invalid sequence - skip it
            read_pos++;
        }
        // 4-byte UTF-8 sequence (0xF0-0xF4)
        else if (byte >= 0xF0 && byte <= 0xF4) {
            if (read_pos + 3 < len) {
                unsigned char byte2 = (unsigned char)text[read_pos + 1];
                unsigned char byte3 = (unsigned char)text[read_pos + 2];
                unsigned char byte4 = (unsigned char)text[read_pos + 3];

                bool valid = false;
                if (byte == 0xF0) {
                    valid = (byte2 >= 0x90 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF) &&
                            (byte4 >= 0x80 && byte4 <= 0xBF);
                } else if (byte == 0xF4) {
                    valid = (byte2 >= 0x80 && byte2 <= 0x8F) && (byte3 >= 0x80 && byte3 <= 0xBF) &&
                            (byte4 >= 0x80 && byte4 <= 0xBF);
                } else {
                    valid = (byte2 >= 0x80 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF) &&
                            (byte4 >= 0x80 && byte4 <= 0xBF);
                }

                if (valid) {
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    text[write_pos++] = text[read_pos++];
                    continue;
                }
            }
            // Invalid sequence - skip it
            read_pos++;
        }
        // Invalid UTF-8 start byte - skip it
        else {
            read_pos++;
        }
    }

    // Null-terminate the sanitized string
    text[write_pos] = '\0';
    return write_pos;
}

/**
 * Preprocesses extracted PDF text by removing common artifacts and normalizing whitespace.
 * @param text The input text to clean. Must be null-terminated and modifiable.
 * @param remove_urls If true, removes URLs from the text.
 * @return Pointer to cleaned text (same buffer as input, modified in-place), or nullptr on error.
 * @note Modifies the input buffer in-place for efficiency.
 */
char* pdf_text_clean(char* text, size_t len, bool remove_urls) {
    assert(text && len > 0);

    size_t write_pos    = 0;
    bool prev_was_space = true;  // Start true to trim leading spaces
    bool in_url         = false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        // Remove null bytes and other control characters except newline/tab
        if (c == '\0' || (c < 32 && c != '\n' && c != '\t')) {
            continue;
        }

        // Detect and skip URLs (optional)
        if (remove_urls) {
            if (c == 'h' && i + 6 < len &&
                (strncmp(&text[i], "http://", 7) == 0 || strncmp(&text[i], "https://", 8) == 0)) {
                in_url = true;
            }
            if (in_url) {
                if (isspace(c) || c == ')' || c == ']' || c == '>') {
                    in_url = false;
                    // Add a space to separate from next content
                    if (!prev_was_space) {
                        text[write_pos++] = ' ';
                        prev_was_space    = true;
                    }
                }
                continue;
            }
        }

        // Remove common PDF encoding artifacts
        // Unicode replacement character (often from failed encoding): U+FFFD (EF BF BD)
        if (c == 0xEF && i + 2 < len && (unsigned char)text[i + 1] == 0xBF && (unsigned char)text[i + 2] == 0xBD) {
            i += 2;
            continue;
        }

        // Remove zero-width spaces and similar
        if (c == 0xE2 && i + 2 < len) {
            unsigned char c1 = (unsigned char)text[i + 1];
            unsigned char c2 = (unsigned char)text[i + 2];

            // Zero-width space (U+200B), zero-width non-joiner (U+200C), zero-width joiner (U+200D)
            if ((c1 == 0x80 && (c2 == 0x8B || c2 == 0x8C || c2 == 0x8D)) ||
                // Word joiner (U+2060)
                (c1 == 0x81 && c2 == 0xA0)) {
                i += 2;
                continue;
            }
        }

        // Normalize whitespace (spaces, tabs, newlines) to single space
        if (isspace(c)) {
            if (!prev_was_space) {
                // Preserve paragraph breaks (multiple newlines become double space)
                if (c == '\n' && i + 1 < len && text[i + 1] == '\n') {
                    text[write_pos++] = '\n';
                    text[write_pos++] = '\n';
                    prev_was_space    = true;
                    i++;  // Skip the second newline
                } else {
                    text[write_pos++] = ' ';
                    prev_was_space    = true;
                }
            }
            continue;
        }

        // Remove standalone single characters that are likely artifacts
        // (common in scanned PDFs with OCR errors)
        if (!isalnum(c) && !prev_was_space) {
            // Check if it's a lone punctuation between spaces
            bool next_is_space = (i + 1 >= len || isspace(text[i + 1]));
            if (next_is_space && (c == '|' || c == '~' || c == '^' || c == '`')) {
                continue;
            }
        }

        // Keep printable ASCII and common UTF-8 characters
        if (isprint(c) || c >= 0x80) {
            text[write_pos++] = (char)c;
            prev_was_space    = false;
        }
    }

    // Trim trailing whitespace
    while (write_pos > 0 && isspace((unsigned char)text[write_pos - 1])) {
        write_pos--;
    }

    // Null terminate
    text[write_pos] = '\0';

    // Remove the text if it's too short to be meaningful (likely all artifacts)
    if (write_pos < 3) {
        text[0] = '\0';
    }

    pdf_remove_headers_footers(text, len);
    sanitize_utf8(text, len);
    return text;
}
