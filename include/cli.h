#ifndef CLI_H
#define CLI_H

#include <solidc/filepath.h>
#include <solidc/flags.h>
#include "database.h"

#ifdef __cplusplus
extern "C" {
#endif

// Walk the root_dir and insert all PDF pages into the database.
// If a page is empty or has fewer pages than min_pages, it is skipped.
// This can be used to filter out small PDF that are not books.
bool process_pdfs(const char* root_dir, int min_pages, bool dryrun);

#ifdef __cplusplus
}
#endif

#endif  // CLI_H
