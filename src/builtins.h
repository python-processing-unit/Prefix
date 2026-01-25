#ifndef BUILTINS_H
#define BUILTINS_H

#include "value.h"
#include "env.h"
#include "ast.h"

// Forward declaration
typedef struct Interpreter Interpreter;

typedef struct {
    const char* name;
    int min_args;
    int max_args;  // -1 for variadic
    Value (*impl)(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);
} BuiltinFunction;

// Initialize the builtins table
void builtins_init(void);

// Lookup a builtin by name, returns NULL if not found
BuiltinFunction* builtin_lookup(const char* name);

// Check if a name is a builtin
bool is_builtin(const char* name);

#endif // BUILTINS_H
