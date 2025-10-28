#ifndef CLI_H
#define CLI_H

#include <pgconn/pgconn.h>
#include <solidc/filepath.h>
#include <solidc/flag.h>

#ifdef __cplusplus
extern "C" {
#endif

// Walk the root_dir and insert all PDF pages into the database.
bool process_pdfs(const char* root_dir, pgconn_t* conn);

#ifdef __cplusplus
}
#endif

#endif  // CLI_H
