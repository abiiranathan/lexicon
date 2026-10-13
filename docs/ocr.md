# OCR and selectable page text

Selectable text is requested on demand with the **T** control in the page
viewer. Pages with embedded PDF text use that text directly. Pages without a
usable text layer are rendered for Tesseract OCR; results are returned as
word-level boxes and cached in PostgreSQL's `ocr_pages` table. Subsequent
requests for that file and page use the cached result.

## Dependencies

The backend build requires the Tesseract and Leptonica development packages
and their `pkg-config` metadata. Runtime installations also need Tesseract
language data. English is the default (`eng`); set `LEXICON_OCR_LANGUAGE` to
another installed Tesseract language code or a `+`-separated combination such
as `eng+deu`. `TESSDATA_PREFIX` may be set when language data is not in the
system default location.

For Debian or Ubuntu, install `libtesseract-dev`, `libleptonica-dev`,
`tesseract-ocr`, and the relevant language package (for example
`tesseract-ocr-eng`).

OCR uses a per-server-worker Tesseract instance, created only when a page
needs OCR. Input rendering is capped at 24 megapixels, with a target scale of
up to 4 PDF pixels per point. Embedded-text pages avoid the OCR render and
recognition costs.
