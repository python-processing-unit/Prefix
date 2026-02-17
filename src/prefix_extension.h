#ifndef PREFIX_EXTENSION_H
#define PREFIX_EXTENSION_H

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PREFIX_EXTENSION_API_VERSION 1
#define PREFIX_EXTENSION_ASMODULE 1

struct Interpreter;
struct Env;
struct Expr;

typedef struct Interpreter Interpreter;
typedef struct Env Env;
typedef struct Expr Expr;
typedef struct prefix_ext_context prefix_ext_context;

typedef Value (*prefix_operator_fn)(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);
typedef void (*prefix_event_fn)(Interpreter* interp, const char* event_name);
typedef int (*prefix_repl_fn)(void);

struct prefix_ext_context {
    int api_version;
    const char* extension_name;

    int (*register_operator)(const char* name, prefix_operator_fn fn, int asmodule);
    int (*register_periodic_hook)(int n, prefix_event_fn fn);
    int (*register_event_handler)(const char* event_name, prefix_event_fn fn);
    int (*register_repl_handler)(prefix_repl_fn repl_fn);
};

typedef void (*prefix_extension_init_fn)(prefix_ext_context* ctx);

#ifdef __cplusplus
}
#endif

#endif // PREFIX_EXTENSION_H
