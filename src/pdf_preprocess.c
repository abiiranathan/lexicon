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
 * Checks if text appears to be a reference/bibliography/index page.
 * Uses multiple heuristics to detect common patterns.
 * @param text The text to check.
 * @param len Length of the text.
 * @return true if text appears to be a reference page, false otherwise.
 */
static bool is_reference_page(const char* text, size_t len) {
    if (len < 50) {
        return false;  // Too short to judge
    }

    size_t line_count                  = 0;
    size_t lines_with_numbers          = 0;
    size_t lines_with_urls             = 0;
    size_t lines_with_doi              = 0;
    size_t lines_with_et_al            = 0;
    size_t lines_with_year_pattern     = 0;  // e.g., "(2023)" or "2023."
    size_t short_lines                 = 0;  // Lines under 20 chars (common in indexes)
    size_t lines_starting_with_capital = 0;

    size_t line_start            = 0;
    bool has_references_header   = false;
    bool has_bibliography_header = false;
    bool has_index_header        = false;

    // Check for common headers
    if (len > 15) {
        if (strncmp(text, "References", 10) == 0 || strncmp(text, "REFERENCES", 10) == 0) {
            has_references_header = true;
        }
        if (strncmp(text, "Bibliography", 12) == 0 || strncmp(text, "BIBLIOGRAPHY", 12) == 0) {
            has_bibliography_header = true;
        }
        if (strncmp(text, "Index", 5) == 0 || strncmp(text, "INDEX", 5) == 0) {
            has_index_header = true;
        }
    }

    // Analyze line patterns
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t line_len = i - line_start;

            if (line_len > 0) {
                line_count++;

                // Check line characteristics
                bool has_digit = false;
                bool has_url   = false;
                bool has_doi   = false;
                bool has_et_al = false;
                bool has_year  = false;

                if (line_len > 0 && isupper((unsigned char)text[line_start])) {
                    lines_starting_with_capital++;
                }

                // Scan the line
                for (size_t j = line_start; j < i; j++) {
                    if (isdigit((unsigned char)text[j])) {
                        has_digit = true;

                        // Check for year pattern: (YYYY) or YYYY.
                        if (j + 4 < i) {
                            if ((text[j - 1] == '(' || text[j - 1] == ' ') && isdigit((unsigned char)text[j + 1]) &&
                                isdigit((unsigned char)text[j + 2]) && isdigit((unsigned char)text[j + 3]) &&
                                (text[j + 4] == ')' || text[j + 4] == '.')) {
                                // Check if it's a plausible year (1900-2099)
                                if (text[j] == '1' || text[j] == '2') {
                                    has_year = true;
                                }
                            }
                        }
                    }

                    // Check for URL patterns
                    if (j + 7 < i && (strncmp(&text[j], "http://", 7) == 0 || strncmp(&text[j], "https://", 8) == 0 ||
                                      strncmp(&text[j], "www.", 4) == 0)) {
                        has_url = true;
                    }

                    // Check for DOI pattern
                    if (j + 4 < i && strncmp(&text[j], "doi:", 4) == 0) {
                        has_doi = true;
                    }
                    if (j + 8 < i && strncmp(&text[j], "10.", 3) == 0 && isdigit((unsigned char)text[j + 3])) {
                        has_doi = true;  // DOI format: 10.xxxx/yyyy
                    }

                    // Check for "et al."
                    if (j + 6 < i && strncmp(&text[j], "et al.", 6) == 0) {
                        has_et_al = true;
                    }
                }

                // Accumulate line statistics
                if (has_digit) lines_with_numbers++;
                if (has_url) lines_with_urls++;
                if (has_doi) lines_with_doi++;
                if (has_et_al) lines_with_et_al++;
                if (has_year) lines_with_year_pattern++;
                if (line_len < 20) short_lines++;
            }

            line_start = i + 1;
        }
    }

    if (line_count < 3) {
        return false;  // Not enough lines to judge
    }

    // Calculate ratios
    float url_ratio        = (float)lines_with_urls / line_count;
    float doi_ratio        = (float)lines_with_doi / line_count;
    float et_al_ratio      = (float)lines_with_et_al / line_count;
    float year_ratio       = (float)lines_with_year_pattern / line_count;
    float short_line_ratio = (float)short_lines / line_count;
    float capital_ratio    = (float)lines_starting_with_capital / line_count;

    // Reference page heuristics
    bool likely_references = (
        // Strong indicators
        (has_references_header || has_bibliography_header) ||

        // Multiple citation patterns
        (url_ratio > 0.3 || doi_ratio > 0.2) || (et_al_ratio > 0.2) || (year_ratio > 0.4) ||

        // Combined weak indicators
        (url_ratio > 0.15 && year_ratio > 0.25) || (doi_ratio > 0.1 && et_al_ratio > 0.1) ||

        // High percentage of lines with numbers (page refs, years, DOIs)
        (lines_with_numbers > line_count * 0.7));

    // Index page heuristics
    bool likely_index = (has_index_header ||

                         // Many short lines with high capital letter start rate
                         (short_line_ratio > 0.6 && capital_ratio > 0.7) ||

                         // Very short average line length with numbers (page references)
                         (short_line_ratio > 0.5 && lines_with_numbers > line_count * 0.5));

    return likely_references || likely_index;
}

/**
 * Checks if text appears to be an index page.
 * Indexes have distinctive patterns: short entries, alphabetical ordering,
 * page numbers, and high capital letter density.
 * @param text The text to check.
 * @param len Length of the text.
 * @return true if text appears to be an index page, false otherwise.
 */
static bool is_index_page(const char* text, size_t len) {
    if (len < 50) {
        return false;
    }

    size_t line_count             = 0;
    size_t short_lines            = 0;  // Lines under 40 chars
    size_t very_short_lines       = 0;  // Lines under 20 chars
    size_t lines_with_numbers     = 0;  // Lines with digits (page refs)
    size_t lines_with_commas      = 0;  // "term, 45, 67, 89" pattern
    size_t lines_starting_capital = 0;
    size_t indented_lines         = 0;  // Lines starting with spaces (sub-entries)
    size_t lines_with_dash        = 0;  // "– see also" pattern

    bool has_index_header = false;

    // Check for "Index" or "INDEX" header
    if (len > 5) {
        if (strncmp(text, "Index", 5) == 0 || strncmp(text, "INDEX", 5) == 0) {
            has_index_header = true;
        }
    }

    size_t line_start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t line_len = i - line_start;

            if (line_len > 0) {
                line_count++;

                // Skip leading whitespace to check actual content
                size_t content_start = line_start;
                size_t indent        = 0;
                while (content_start < i && isspace((unsigned char)text[content_start])) {
                    indent++;
                    content_start++;
                }

                size_t content_len = i - content_start;

                if (content_len > 0) {
                    // Check if line starts with capital letter
                    if (isupper((unsigned char)text[content_start])) {
                        lines_starting_capital++;
                    }

                    // Check for indentation (sub-entries in index)
                    if (indent >= 2) {
                        indented_lines++;
                    }

                    // Track line length
                    if (content_len < 40) {
                        short_lines++;
                    }
                    if (content_len < 20) {
                        very_short_lines++;
                    }

                    // Scan line for patterns
                    bool has_digit     = false;
                    bool has_comma     = false;
                    bool has_dash      = false;
                    size_t digit_count = 0;
                    size_t comma_count = 0;

                    for (size_t j = content_start; j < i; j++) {
                        if (isdigit((unsigned char)text[j])) {
                            has_digit = true;
                            digit_count++;
                        }
                        if (text[j] == ',') {
                            has_comma = true;
                            comma_count++;
                        }
                        // Check for em-dash or en-dash (UTF-8 or ASCII)
                        if (text[j] == '-' ||
                            (j + 2 < i && (unsigned char)text[j] == 0xE2 && (unsigned char)text[j + 1] == 0x80 &&
                             ((unsigned char)text[j + 2] == 0x93 || (unsigned char)text[j + 2] == 0x94))) {
                            has_dash = true;
                        }
                    }

                    if (has_digit) {
                        lines_with_numbers++;
                    }

                    // Index pattern: "Term, 12, 45, 67" has multiple commas with digits
                    if (has_comma && comma_count >= 1 && digit_count > 0) {
                        lines_with_commas++;
                    }

                    if (has_dash) {
                        lines_with_dash++;
                    }
                }
            }

            line_start = i + 1;
        }
    }

    if (line_count < 5) {
        return false;
    }

    // Calculate ratios
    float short_ratio      = (float)short_lines / line_count;
    float very_short_ratio = (float)very_short_lines / line_count;
    float number_ratio     = (float)lines_with_numbers / line_count;
    float comma_ratio      = (float)lines_with_commas / line_count;
    float capital_ratio    = (float)lines_starting_capital / line_count;
    float indent_ratio     = (float)indented_lines / line_count;
    float dash_ratio       = (float)lines_with_dash / line_count;

    // Index detection heuristics
    bool likely_index = (
        // Explicit header
        has_index_header ||

        // Strong index patterns:
        // Many short lines with capitals and page numbers
        (short_ratio > 0.7 && capital_ratio > 0.6 && number_ratio > 0.5) ||

        // Comma-separated page numbers pattern
        (comma_ratio > 0.4 && number_ratio > 0.6) ||

        // Sub-entries (indentation) with page numbers
        (indent_ratio > 0.2 && number_ratio > 0.5 && capital_ratio > 0.5) ||

        // Very short lines (typical of index entries) with high structure
        (very_short_ratio > 0.5 && capital_ratio > 0.7 && number_ratio > 0.4) ||

        // Cross-references ("see also", "see") indicated by dashes
        (dash_ratio > 0.15 && capital_ratio > 0.6));

    return likely_index;
}

/**
 * Checks if text has abnormally long lines (missing newlines).
 * @param text The text to check.
 * @param len Length of the text.
 * @param max_line_length Maximum acceptable line length (e.g., 500-1000 chars).
 * @return true if text has lines exceeding the threshold, false otherwise.
 */
static bool has_malformed_lines(const char* text, size_t len, size_t max_line_length) {
    size_t current_line_len = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            current_line_len = 0;  // Reset on newline
        } else {
            current_line_len++;
            if (current_line_len > max_line_length) {
                return true;  // Found a line that's too long
            }
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

    // Skip any whitespace after leading dashes
    while (read_pos < len && isspace((unsigned char)text[read_pos])) {
        read_pos++;
    }

    // Skip leading dashes (common PDF artifact)
    while (read_pos < len && (text[read_pos] == '-' || text[read_pos] == '.')) {
        read_pos++;
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

        // Detect and skip repetitive dash sequences (common PDF artifacts)
        if ((c == '-' || c == '.') && read_pos + 10 < len) {
            // Look ahead to see if this is a long sequence of dashes
            size_t dash_count = 0;
            size_t lookahead  = read_pos;
            while (lookahead < len && lookahead < read_pos + 100) {
                unsigned char ahead = (unsigned char)text[lookahead];
                if (ahead == '-' || c == '.') {
                    dash_count++;
                } else if (!isspace(ahead)) {
                    break;  // Found non-dash, non-whitespace
                }
                lookahead++;
            }

            // If we found 10+ dashes with only whitespace between, skip the entire sequence
            if (dash_count >= 10) {
                read_pos = lookahead;
                // Preserve a single space separator if previous wasn't already a space
                if (!prev_was_space) {
                    text[write_pos++] = ' ';
                    prev_was_space    = true;
                }
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

    // Check if this appears to be a reference or index page
    if (write_pos > 100) {
        if (is_reference_page(text, write_pos) || is_index_page(text, write_pos)) {
            printf("Skipping Reference/index page\n");

            text[0] = '\0';  // Reject reference/index pages
            return text;
        }
    }

    // Trim trailing whitespace
    while (write_pos > 0 && isspace((unsigned char)text[write_pos - 1])) {
        write_pos--;
    }

    // Trim trailing dashes (handles end-of-document artifacts)
    while (write_pos > 0 && (text[write_pos - 1] == '-' || text[write_pos - 1] == '.')) {
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
