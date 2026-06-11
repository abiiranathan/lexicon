/*
 * pdf_preprocess.c — PDF text post-processing.
 *
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <assert.h>
#include <ctype.h>    // for isspace, isupper, isdigit
#include <pcre2.h>    // for pcre2_*
#include <stdbool.h>  // for bool
#include <stdio.h>    // for printf
#include <string.h>   // for strncmp

/* ── UTF-8 validation ─────────────────────────────────────────────────────── */

/*
 * Leading-byte → expected sequence length. 0 = invalid start byte.
 * Overlong prefixes (0xC0/0xC1) and values above 0xF4 map to 0.
 */
static const uint8_t utf8_seq_len[256] = {
    [0x00 ... 0x7F] = 1, /* ASCII                  */
    [0x80 ... 0xBF] = 0, /* continuation (invalid) */
    [0xC0 ... 0xC1] = 0, /* overlong               */
    [0xC2 ... 0xDF] = 2, /* 2-byte sequence         */
    [0xE0 ... 0xEF] = 3, /* 3-byte sequence         */
    [0xF0 ... 0xF4] = 4, /* 4-byte sequence         */
    [0xF5 ... 0xFF] = 0, /* invalid (above U+10FFFF) */
};

/**
 * Validates a UTF-8 sequence starting at text[pos].
 * @param text  Input buffer.
 * @param pos   Start offset.
 * @param len   Total buffer length.
 * @param out   Bytes consumed (≥1 even on failure).
 * @return true if the sequence is valid UTF-8.
 */
static bool validate_utf8(const char* text, size_t pos, size_t len, size_t* out) {
    const uint8_t* p = (const uint8_t*)text + pos;
    uint8_t b0 = p[0];
    size_t n = utf8_seq_len[b0];

    if (n == 1) { /* ASCII: reject C0 controls except HT/LF/CR. */
        *out = 1;
        return b0 >= 0x20 || b0 == '\t' || b0 == '\n' || b0 == '\r';
    }
    if (n == 0 || pos + n > len) {
        *out = 1;
        return false;
    }

    for (size_t i = 1; i < n; i++) { /* verify continuations */
        if ((p[i] & 0xC0) != 0x80) {
            *out = 1;
            return false;
        }
    }

    /* Per-leading-byte range checks (overlong / surrogate / > U+10FFFF). */
    bool ok;
    switch (b0) {
        case 0xE0:
            ok = (p[1] >= 0xA0);
            break;
        case 0xED:
            ok = (p[1] <= 0x9F);
            break;
        case 0xF0:
            ok = (p[1] >= 0x90);
            break;
        case 0xF4:
            ok = (p[1] <= 0x8F);
            break;
        default:
            ok = true;
            break;
    }
    *out = ok ? n : 1;
    return ok;
}

/* ── PDF artifact detection ───────────────────────────────────────────────── */

/* Fixed set of zero-width / replacement Unicode sequences (3 bytes each). */
static const uint8_t artifacts[][3] = {
    {0xEF, 0xBF, 0xBD}, /* U+FFFD replacement character */
    {0xE2, 0x80, 0x8B}, /* U+200B zero-width space      */
    {0xE2, 0x80, 0x8C}, /* U+200C ZWNJ                  */
    {0xE2, 0x80, 0x8D}, /* U+200D ZWJ                   */
    {0xE2, 0x81, 0xA0}, /* U+2060 word joiner           */
};
#define ARTIFACT_COUNT (sizeof(artifacts) / sizeof(artifacts[0]))

/** Returns bytes to skip if text[pos] is a known artifact, else 0. */
static size_t artifact_skip(const char* text, size_t pos, size_t len) {
    if (pos + 2 >= len) return 0;
    const uint8_t* p = (const uint8_t*)text + pos;
    for (size_t i = 0; i < ARTIFACT_COUNT; i++) {
        if (p[0] == artifacts[i][0] && p[1] == artifacts[i][1] && p[2] == artifacts[i][2]) return 3;
    }
    return 0;
}

/* ── Boilerplate page detection via PCRE2 ─────────────────────────────────── */

static pcre2_code* pat_header;   /* Section header at line start      */
static pcre2_code* pat_doi;      /* DOI / doi: prefix                 */
static pcre2_code* pat_et_al;    /* "et al."                          */
static pcre2_code* pat_index_ln; /* "Term, 12, 45" index-entry lines  */

static void boilerplate_init(void) {
    int ec;
    PCRE2_SIZE eo;
#define CP(s, f) pcre2_compile((PCRE2_SPTR)(s), PCRE2_ZERO_TERMINATED, (f), &ec, &eo, NULL)
    pat_header = CP("^(?:References|Bibliography|Index|REFERENCES|BIBLIOGRAPHY|INDEX)\\b", PCRE2_MULTILINE);
    pat_doi = CP("\\b(?:doi:|10\\.\\d{4,}/\\S+)", PCRE2_CASELESS);
    pat_et_al = CP("\\bet\\s+al\\.", 0);
    pat_index_ln = CP("^[A-Z][^\\n]{0,38},(?:\\s*\\d+)+\\s*$", PCRE2_MULTILINE);
#undef CP
}

/**
 * Returns true if text looks like a reference, bibliography, or index page.
 * Uses four pre-compiled PCRE2 patterns; compiled once on first call.
 * @param text  Text to inspect (need not be null-terminated within len bytes).
 * @param len   Number of bytes to inspect.
 */
static bool is_boilerplate_page(const char* text, size_t len) {
    static bool inited = false;
    if (!inited) {
        boilerplate_init();
        inited = true;
    }

    pcre2_match_data* md = pcre2_match_data_create(4, NULL);
    if (!md) return false;

    const PCRE2_SPTR s = (const PCRE2_SPTR)text;

    if (pcre2_match(pat_header, s, len, 0, 0, md, NULL) >= 0) {
        pcre2_match_data_free(md);
        return true;
    }

    /* Count hits; discard once any threshold is reached. */
    struct {
        pcre2_code* pat;
        size_t threshold;
        size_t count;
    } checks[] = {
        {pat_doi, 3, 0},
        {pat_et_al, 3, 0},
        {pat_index_ln, 5, 0},
    };
    for (size_t c = 0; c < 3; c++) {
        size_t off = 0;
        while (off < len && pcre2_match(checks[c].pat, s, len, off, 0, md, NULL) >= 0) {
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
            off = ov[1];
            if (++checks[c].count >= checks[c].threshold) {
                pcre2_match_data_free(md);
                return true;
            }
        }
    }

    pcre2_match_data_free(md);
    return false;
}

/* ── Main entry point ─────────────────────────────────────────────────────── */

/**
 * Cleans extracted PDF text in-place: removes Unicode artifacts, normalises
 * whitespace, drops repetitive dash/dot runs, optionally strips URLs,
 * validates UTF-8, and discards reference/index pages.
 *
 * @param text        Modifiable, null-terminated input buffer.
 * @param len         Length of @p text (excluding the null terminator).
 * @param remove_urls Strip http(s):// URLs when true.
 * @return @p text, always. On discarded pages write_pos is 0 and text[0] == '\0'.
 * @note Operates in-place; caller retains ownership of the buffer.
 */
char* pdf_text_clean(char* text, size_t len, bool remove_urls) {
    assert(text != NULL && len > 0);

    size_t write = 0, read = 0;
    bool prev_space = true; /* true → collapse leading whitespace */
    bool in_url = false;

    while (read < len && text[read] != '\0') {
        unsigned char c = (unsigned char)text[read];

        /* Known Unicode artifact → drop. */
        size_t art = artifact_skip(text, read, len);
        if (art) {
            read += art;
            continue;
        }

        /* URL stripping. */
        if (remove_urls) {
            if (!in_url && c == 'h' && read + 7 < len &&
                (strncmp(&text[read], "http://", 7) == 0 || strncmp(&text[read], "https://", 8) == 0))
                in_url = true;
            if (in_url) {
                if (isspace(c) || c == ')' || c == ']' || c == '>') {
                    in_url = false;
                    if (!prev_space) {
                        text[write++] = ' ';
                        prev_space = true;
                    }
                }
                read++;
                continue;
            }
        }

        /* Long run of dashes or dots (≥10) → single space. */
        if ((c == '-' || c == '.') && read + 9 < len) {
            size_t ahead = read, run = 0;
            while (ahead < len) {
                unsigned char a = (unsigned char)text[ahead];
                if (a == '-' || a == '.') {
                    run++;
                    ahead++;
                } else if (isspace(a)) {
                    ahead++;
                } else {
                    break;
                }
            }
            if (run >= 10) {
                read = ahead;
                if (!prev_space) {
                    text[write++] = ' ';
                    prev_space = true;
                }
                continue;
            }
        }

        /* UTF-8 validation — skip invalid bytes. */
        size_t n = 0;
        if (!validate_utf8(text, read, len, &n)) {
            read += n;
            continue;
        }

        /* Whitespace normalisation. */
        if (isspace(c)) {
            if (!prev_space) {
                if (c == '\n' && read + 1 < len && text[read + 1] == '\n') {
                    text[write++] = '\n'; /* preserve paragraph break */
                    text[write++] = '\n';
                    read += 2;
                } else {
                    text[write++] = ' ';
                    read++;
                }
                prev_space = true;
            } else {
                read++;
            }
            continue;
        }

        /* Drop isolated visual-noise ASCII (|, ~, ^, `) surrounded by spaces. */
        if (n == 1 && !prev_space) {
            bool next_sp = (read + 1 >= len || isspace((unsigned char)text[read + 1]));
            if (next_sp && (c == '|' || c == '~' || c == '^' || c == '`')) {
                read++;
                continue;
            }
        }

        for (size_t i = 0; i < n; i++)
            text[write++] = text[read++];
        prev_space = false;
    }

    /* Discard reference / bibliography / index pages. */
    if (write > 100 && is_boilerplate_page(text, write)) {
        text[0] = '\0';
        return text;
    }

    /* Trim trailing whitespace then stray dashes/dots. */
    while (write > 0 && isspace((unsigned char)text[write - 1]))
        write--;
    while (write > 0 && (text[write - 1] == '-' || text[write - 1] == '.'))
        write--;

    text[write] = '\0';
    if (write < 3) text[0] = '\0'; /* too short to be meaningful */
    return text;
}
