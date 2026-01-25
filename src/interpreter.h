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
} Interpreter;

// Main entry point
ExecResult exec_program(Stmt* program);

// Functions needed by builtins.c
Value eval_expr(Interpreter* interp, Expr* expr, Env* env);
int value_truthiness(Value v);

// Function table operations
FuncTable* func_table_create(void);
void func_table_free(FuncTable* table);
bool func_table_add(FuncTable* table, const char* name, Func* func);
Func* func_table_lookup(FuncTable* table, const char* name);

#endif // INTERPRETER_H