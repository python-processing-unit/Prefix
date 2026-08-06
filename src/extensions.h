#ifndef EXTENSIONS_H
#define EXTENSIONS_H

#include "common.h"
#include "interpreter.h"

// Configure directories used for extension-path fallback resolution.
// interpreter_dir should be the directory containing the interpreter executable.
// cwd_dir should be the process current working directory.
void extensions_set_runtime_dirs(const char *interpreter_dir, const char *cwd_dir);

// Load one extension library from path. Relative paths resolve against base_dir,
// then cwd, then interpreter_dir/ext.
// Returns 0 on success, -1 on failure. On failure, *error_out is heap-allocated.
int extensions_load_library(const char *path, const char *base_dir, char **error_out);

// Load an extension by logical name/specifier used by extend.
// spec excludes platform filename extension and may use package semantics via "..".
// scope_name is retained for loader bookkeeping; module-restricted exposure uses
// the extension name only.
// If loaded_name_out is non-NULL, it receives the extension namespace name (heap-allocated).
// Returns 0 on success, -1 on failure. On failure, *error_out is heap-allocated.
int extensions_load_named(const char *spec, const char *base_dir, const char *scope_name, char **loaded_name_out,
                          char **error_out);

// Ensure a previously-loaded extension is exposed under the provided
// scope_name (module alias). This creates scope-qualified aliases for any
// operators registered with the module-restriction flag. Returns 0 on
// success, -1 on failure; on failure *error_out is heap-allocated.
int extensions_expose_named(const char *ext_name, const char *scope_name, char **error_out);

// Unload all loaded extension libraries.
void extensions_shutdown(void);

// Fire a named event to all registered event handlers. Returns number of
// handlers invoked (may be 0).
int extensions_fire_event(Interpreter *interp, const char *event_name);

// Evaluate and invoke any periodic hooks that are due at the interpreter's
// current rewrite step. Safe to call after a step has been logged.
void extensions_run_periodic_hooks(Interpreter *interp);

// If a REPL replacement handler has been registered by an extension, call
// it and return its integer return-code. Returns -1 if no repl handler is
// registered.
int extensions_call_repl_handler(void);

#endif // EXTENSIONS_H
