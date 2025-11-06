#ifndef CLI_H
#define CLI_H

#include <pgconn/pgconn.h>
#include <solidc/filepath.h>
#include <solidc/flag.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walk the root_dir and insert all PDF pages into the database.
// If a page is empty or has fewer pages than min_pages, it is skipped.
// This can be used to filter out small PDF that are not books.
bool process_pdfs(pgconn_config_t* config, const char* root_dir, int min_pages, bool dryrun);

// Function to clean PDF text before being saved.
// Provided by pdf_preprocess.c
extern char* pdf_text_clean(char* text, size_t len, bool remove_urls);

#ifdef __cplusplus
}
#endif

#endif  // CLI_H
