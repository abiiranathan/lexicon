#ifndef CLI_H
#define CLI_H

#include <pgpool.h>
#include <solidc/filepath.h>
#include <solidc/flag.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walk the root_dir and insert all PDF pages into the database.
bool process_pdfs(const char* root_dir);

// Handler for CLI to build PDF index.
void build_pdf_index(Command* cmd);

#ifdef __cplusplus
}
#endif

#endif  // CLI_H
