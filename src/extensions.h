#ifndef EXTENSIONS_H
#define EXTENSIONS_H

#include "common.h"

// Configure directories used for extension-path fallback resolution.
// interpreter_dir should be the directory containing the interpreter executable.
// cwd_dir should be the process current working directory.
void extensions_set_runtime_dirs(const char* interpreter_dir, const char* cwd_dir);

// Load one extension library from path. Relative paths resolve against base_dir,
// then cwd, then interpreter_dir/ext.
// Returns 0 on success, -1 on failure. On failure, *error_out is heap-allocated.
int extensions_load_library(const char* path, const char* base_dir, char** error_out);

// Load all extensions listed in a pointer file (.prex).
// Returns 0 on success, -1 on failure. On failure, *error_out is heap-allocated.
int extensions_load_prex_file(const char* prex_path, char** error_out);

// Load all extensions from pointer file if it exists.
// If loaded_any is non-NULL it is set to 1 when file exists and was loaded.
// Returns 0 on success (including file-not-found), -1 on failure.
int extensions_load_prex_if_exists(const char* prex_path, int* loaded_any, char** error_out);

// Unload all loaded extension libraries.
void extensions_shutdown(void);

#endif // EXTENSIONS_H
