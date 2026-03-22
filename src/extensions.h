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

// Load an extension by logical name/specifier used by EXTEND.
// spec excludes platform filename extension and may use package semantics via "..".
// scope_name is retained for loader bookkeeping; PREFIX_EXTENSION_ASMODULE exposure
// uses the extension name only.
// If loaded_name_out is non-NULL, it receives the extension namespace name (heap-allocated).
// Returns 0 on success, -1 on failure. On failure, *error_out is heap-allocated.
int extensions_load_named(const char* spec,
                          const char* base_dir,
                          const char* scope_name,
                          char** loaded_name_out,
                          char** error_out);

// Unload all loaded extension libraries.
void extensions_shutdown(void);

#endif // EXTENSIONS_H
