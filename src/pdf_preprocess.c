#include <assert.h>
#include <ctype.h>    // for isspace, isprint, isalnum, isdigit
#include <stdbool.h>  // for bool
#include <stdio.h>    // for printf
#include <stdlib.h>   // for malloc, free
#include <string.h>   // for strlen, strncmp

/**
 * Validates a UTF-8 sequence starting at the given position.
 * @param text The text buffer.
 * @param pos Current read position.
 * @param len Total buffer length.
 * @param bytes_consumed Output parameter for number of bytes in this sequence.
 * @return true if the sequence is valid UTF-8, false otherwise.
 */
static bool validate_utf8_sequence(const char* text, size_t pos, size_t len, size_t* bytes_consumed) {
    unsigned char byte = (unsigned char)text[pos];
    *bytes_consumed    = 0;

    // ASCII character (0x00-0x7F)
    if (byte <= 0x7F) {
        // Skip control characters except tab, newline, carriage return
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') {
            *bytes_consumed = 1;
            return false;
        }
        *bytes_consumed = 1;
        return true;
    }

    // 2-byte UTF-8 sequence (0xC2-0xDF)
    if (byte >= 0xC2 && byte <= 0xDF) {
        if (pos + 1 < len) {
            unsigned char byte2 = (unsigned char)text[pos + 1];
            if (byte2 >= 0x80 && byte2 <= 0xBF) {
                *bytes_consumed = 2;
                return true;
            }
        }
        *bytes_consumed = 1;
        return false;
    }

    // 3-byte UTF-8 sequence (0xE0-0xEF)
    if (byte >= 0xE0 && byte <= 0xEF) {
        if (pos + 2 < len) {
            unsigned char byte2 = (unsigned char)text[pos + 1];
            unsigned char byte3 = (unsigned char)text[pos + 2];

            bool valid = false;
            if (byte == 0xE0) {
                valid = (byte2 >= 0xA0 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF);
            } else if (byte == 0xED) {
                valid = (byte2 >= 0x80 && byte2 <= 0x9F) && (byte3 >= 0x80 && byte3 <= 0xBF);
            } else {
                valid = (byte2 >= 0x80 && byte2 <= 0xBF) && (byte3 >= 0x80 && byte3 <= 0xBF);
            }

            if (valid) {
                *bytes_consumed = 3;
                return true;
            }
        }
        *bytes_consumed = 1;
        return false;
    }

    // 4-byte UTF-8 sequence (0xF0-0xF4)
    if (byte >= 0xF0 && byte <= 0xF4) {
        if (pos + 3 < len) {
            unsigned char byte2 = (unsigned char)text[pos + 1];
            unsigned char byte3 = (unsigned char)text[pos + 2];
            unsigned char byte4 = (unsigned char)text[pos + 3];

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
                *bytes_consumed = 4;
                return true;
            }
        }
        *bytes_consumed = 1;
        return false;
    }

    // Invalid UTF-8 start byte
    *bytes_consumed = 1;
    return false;
}

/**
 * Checks if a UTF-8 sequence is a known artifact that should be removed.
 * @param text The text buffer.
 * @param pos Current read position.
 * @param len Total buffer length.
 * @param skip_bytes Output parameter for number of bytes to skip if artifact found.
 * @return true if this is an artifact to remove, false otherwise.
 */
static bool is_pdf_artifact(const char* text, size_t pos, size_t len, size_t* skip_bytes) {
    unsigned char c = (unsigned char)text[pos];
    *skip_bytes     = 0;

    // Unicode replacement character (U+FFFD): EF BF BD
    if (c == 0xEF && pos + 2 < len && (unsigned char)text[pos + 1] == 0xBF && (unsigned char)text[pos + 2] == 0xBD) {
        *skip_bytes = 3;
        return true;
    }

    // Zero-width characters and word joiners
    if (c == 0xE2 && pos + 2 < len) {
        unsigned char c1 = (unsigned char)text[pos + 1];
        unsigned char c2 = (unsigned char)text[pos + 2];

        // Zero-width space (U+200B), ZWNJ (U+200C), ZWJ (U+200D), or word joiner (U+2060)
        if ((c1 == 0x80 && (c2 == 0x8B || c2 == 0x8C || c2 == 0x8D)) || (c1 == 0x81 && c2 == 0xA0)) {
            *skip_bytes = 3;
            return true;
        }
    }

    return false;
}

/**
 * Preprocesses extracted PDF text by removing artifacts, normalizing whitespace,
 * and validating UTF-8 encoding.
 * @param text The input text to clean. Must be null-terminated and modifiable.
 * @param len Length of the input text.
 * @param remove_urls If true, removes URLs from the text.
 * @return Pointer to cleaned text (same buffer as input, modified in-place), or nullptr on error.
 * @note Modifies the input buffer in-place for efficiency.
 */
char* pdf_text_clean(char* text, size_t len, bool remove_urls) {
    assert(text && len > 0);

    size_t write_pos    = 0;
    size_t read_pos     = 0;
    bool prev_was_space = true;  // Start true to trim leading spaces
    bool in_url         = false;

    // Skip leading page numbers (first pass)
    if (len > 0 && isdigit((unsigned char)text[0])) {
        size_t skip = 0;
        while (skip < len && skip < 10 && (isdigit((unsigned char)text[skip]) || isspace((unsigned char)text[skip]))) {
            skip++;
        }
        if (skip > 0 && skip < 10) {
            read_pos = skip;
        }
    }

    while (read_pos < len && text[read_pos] != '\0') {
        unsigned char c = (unsigned char)text[read_pos];

        // Check for PDF-specific artifacts first
        size_t skip_bytes = 0;
        if (is_pdf_artifact(text, read_pos, len, &skip_bytes)) {
            read_pos += skip_bytes;
            continue;
        }

        // Detect and skip URLs (optional)
        if (remove_urls) {
            if (c == 'h' && read_pos + 6 < len &&
                (strncmp(&text[read_pos], "http://", 7) == 0 || strncmp(&text[read_pos], "https://", 8) == 0)) {
                in_url = true;
            }
            if (in_url) {
                if (isspace(c) || c == ')' || c == ']' || c == '>') {
                    in_url = false;
                    if (!prev_was_space) {
                        text[write_pos++] = ' ';
                        prev_was_space    = true;
                    }
                }
                read_pos++;
                continue;
            }
        }

        // Validate UTF-8 and copy valid sequences
        size_t utf8_bytes = 0;
        if (validate_utf8_sequence(text, read_pos, len, &utf8_bytes)) {
            // Handle whitespace normalization
            if (isspace(c)) {
                if (!prev_was_space) {
                    // Preserve paragraph breaks (multiple newlines)
                    if (c == '\n' && read_pos + 1 < len && text[read_pos + 1] == '\n') {
                        text[write_pos++] = '\n';
                        text[write_pos++] = '\n';
                        prev_was_space    = true;
                        read_pos += 2;
                        continue;
                    }
                    text[write_pos++] = ' ';
                    prev_was_space    = true;
                }
                read_pos++;
                continue;
            }

            // Skip standalone artifact punctuation between spaces
            if (!isalnum(c) && !prev_was_space && utf8_bytes == 1) {
                bool next_is_space = (read_pos + 1 >= len || isspace(text[read_pos + 1]));
                if (next_is_space && (c == '|' || c == '~' || c == '^' || c == '`')) {
                    read_pos++;
                    continue;
                }
            }

            // Copy valid UTF-8 sequence
            for (size_t i = 0; i < utf8_bytes; i++) {
                text[write_pos++] = text[read_pos++];
            }
            prev_was_space = false;
        } else {
            // Invalid UTF-8 sequence - skip it
            read_pos += utf8_bytes > 0 ? utf8_bytes : 1;
        }
    }

    // Trim trailing whitespace
    while (write_pos > 0 && isspace((unsigned char)text[write_pos - 1])) {
        write_pos--;
    }

    // Null terminate
    text[write_pos] = '\0';

    // Remove text if it's too short to be meaningful
    if (write_pos < 3) {
        text[0] = '\0';
    }

    return text;
}
