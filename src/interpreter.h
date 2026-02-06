#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "ast.h"
#include "env.h"

typedef struct {
    DeclType type;
    char* name;
    Expr* default_value;
} RuntimeParam;

typedef struct {
    RuntimeParam* items;
    size_t count;
} RuntimeParamList;

// Func is the runtime representation of a function
struct Func {
    char* name;
    DeclType return_type;
    ParamList params;
    Stmt* body;
    Env* closure;
};

typedef struct Func Func;

typedef enum {
    EXEC_OK,
    EXEC_RETURN,
    EXEC_BREAK,
    EXEC_CONTINUE,
    EXEC_ERROR,
    EXEC_GOTO
} ExecStatus;

typedef struct {
    ExecStatus status;
    Value value;
    int break_count;
    int jump_index;
    char* error;
    int error_line;
    int error_column;
} ExecResult;

typedef struct {
    Value key;
    int index;
} LabelEntry;

typedef struct {
    LabelEntry* items;
    size_t count;
    size_t capacity;
} LabelMap;

typedef struct {
    Env* env;
    LabelMap labels;
} Frame;

// Function table entry
typedef struct FuncEntry {
    char* name;
    Func* func;
    struct FuncEntry* next;
} FuncEntry;

// Function table
typedef struct {
    FuncEntry* entries;
    size_t count;
} FuncTable;

// Interpreter state
typedef struct Interpreter {
    Env* global_env;
    FuncTable* functions;
    int loop_depth;
    char* error;
    int error_line;
    int error_col;
    bool in_try_block;
    // Module registry: linked list of imported modules
    struct ModuleEntry* modules;
    // When non-zero, forwarding of console output (PRINT/CL) is suppressed
    int shushed;
    // Current thread handle when executing in a background thread (NULL in main thread)
    struct Thr* current_thr;
} Interpreter;

// Main entry point
ExecResult exec_program(Stmt* program, const char* source_path);

// Execute a parsed program (`Stmt*`) within an existing interpreter
// and environment. This runs the program using the provided `interp`
// state and the supplied `env` (which may be the current frame or
// the global environment). Returns an ExecResult similar to
// `exec_program`.
ExecResult exec_program_in_env(Interpreter* interp, Stmt* program, Env* env);
// Functions needed by builtins.c
Value eval_expr(Interpreter* interp, Expr* expr, Env* env);
int value_truthiness(Value v);
// Expose indexed-assignment helper so builtins can reuse it
ExecResult assign_index_chain(Interpreter* interp, Env* env, Expr* idx_expr, Value rhs, int stmt_line, int stmt_col);
// Restart a finished thread `thr_val` by re-launching its stored body/env.
// Returns 0 on success, -1 on failure. On failure, sets interp->error/message.
int interpreter_restart_thread(Interpreter* interp, Value thr_val, int line, int col);

// Function table operations
FuncTable* func_table_create(void);
void func_table_free(FuncTable* table);
bool func_table_add(FuncTable* table, const char* name, Func* func);
Func* func_table_lookup(FuncTable* table, const char* name);

// Module registry helpers
// Register a module name and create its isolated Env. Returns 0 on success, -1 on error.
int module_register(Interpreter* interp, const char* name);
// Lookup a module's Env by name; returns NULL if not found.
Env* module_env_lookup(Interpreter* interp, const char* name);

#endif // INTERPRETER_H