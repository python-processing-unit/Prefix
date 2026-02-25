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

typedef struct {
    char* name;
    Env* env;
    int call_line;
    int call_col;
    int has_call_location;
    int last_step_index;
    char state_id[24];
    int has_state_entry;
    int last_line;
    int last_col;
    char last_statement[64];
} TraceFrame;

// Interpreter state
typedef struct Interpreter {
    Env* global_env;
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
    // When true, first-declarations/typed first-assignment stay in the current
    // env instead of being redirected to parent env (used by PARFOR workers).
    bool isolate_env_writes;
    // Traceback/logging state
    int verbose;
    int private_mode;
    char* source_path;
    TraceFrame* trace_stack;
    size_t trace_stack_count;
    size_t trace_stack_capacity;
    int trace_next_step_index;
    char trace_last_state_id[24];
    char trace_last_rule[32];
} Interpreter;

// Initialize/destroy a reusable interpreter session.
// `source_path` sets the primary module source label (e.g. script path or "<repl>").
void interpreter_init(Interpreter* interp, const char* source_path, bool verbose, bool private_mode);
void interpreter_destroy(Interpreter* interp);

// Main entry point
ExecResult exec_program(Stmt* program, const char* source_path);

// Execute a parsed program (`Stmt*`) within an existing interpreter
// and environment. This runs the program using the provided `interp`
// state and the supplied `env` (which may be the current frame or
// the global environment). Returns an ExecResult similar to
// `exec_program`.
ExecResult exec_program_in_env(Interpreter* interp, Stmt* program, Env* env);

// Build and return a traceback string for the current interpreter call stack.
// Caller owns the returned string.
char* interpreter_format_traceback(Interpreter* interp, const char* error_msg, int line, int col);

// Reset traceback stack for interactive recovery while preserving the current
// top-level frame.
void interpreter_reset_traceback(Interpreter* interp, Env* top_env);
// Functions needed by builtins.c
Value eval_expr(Interpreter* interp, Expr* expr, Env* env);
int value_truthiness(Value v);
// Expose indexed-assignment helper so builtins can reuse it
ExecResult assign_index_chain(Interpreter* interp, Env* env, Expr* idx_expr, Value rhs, int stmt_line, int stmt_col);
// Restart a finished thread `thr_val` by re-launching its stored body/env.
// Returns 0 on success, -1 on failure. On failure, sets interp->error/message.
int interpreter_restart_thread(Interpreter* interp, Value thr_val, int line, int col);

// Module registry helpers
// Register a module name and create its isolated Env. Returns 0 on success, -1 on error.
int module_register(Interpreter* interp, const char* name);
// Register another module name as an alias to an existing module Env.
// Returns 0 on success, -1 on error.
int module_register_alias(Interpreter* interp, const char* name, Env* env);
// Lookup a module's Env by name; returns NULL if not found.
Env* module_env_lookup(Interpreter* interp, const char* name);

#endif // INTERPRETER_H