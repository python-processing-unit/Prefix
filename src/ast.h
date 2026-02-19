#ifndef AST_H
#define AST_H

#include "common.h"

typedef enum {
    TYPE_INT,
    TYPE_FLT,
    TYPE_STR,
    TYPE_TNS,
    TYPE_MAP,
    TYPE_FUNC,
    TYPE_THR,
    TYPE_UNKNOWN
} DeclType;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef enum {
    EXPR_INT,
    EXPR_FLT,
    EXPR_STR,
    EXPR_PTR,
    EXPR_IDENT,
    EXPR_CALL,
    EXPR_ASYNC,
    EXPR_TNS,
    EXPR_MAP,
    EXPR_INDEX,
    EXPR_RANGE,
    EXPR_WILDCARD
} ExprType;

typedef struct {
    Expr** items;
    size_t count;
    size_t capacity;
} ExprList;

struct Expr {
    ExprType type;
    int line;
    int column;
    union {
        int64_t int_value;
        double flt_value;
        char* str_value;
        char* ident;
        char* ptr_name;
            struct { Stmt* block; } async;
        struct {
            Expr* callee;
            ExprList args;
            char** kw_names;
            ExprList kw_args;
            size_t kw_count;
            size_t kw_capacity;
        } call;
        struct {
            Expr* target;
            ExprList indices;
        } index;
        struct {
            Expr* start;
            Expr* end;
        } range;
        struct {
            ExprList keys;
            ExprList values;
        } map_items;
        ExprList tns_items;
    } as;
};

typedef enum {
    STMT_BLOCK,
    STMT_ASYNC,
    STMT_EXPR,
    STMT_ASSIGN,
    STMT_DECL,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_PARFOR,
    STMT_FUNC,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_THR,
    STMT_POP,
    STMT_TRY,
    STMT_GOTO,
    STMT_GOTOPOINT
} StmtType;

typedef struct {
    Stmt** items;
    size_t count;
    size_t capacity;
} StmtList;

typedef struct {
    DeclType type;
    char* name;
    Expr* default_value; // optional
} Param;

typedef struct {
    Param* items;
    size_t count;
    size_t capacity;
} ParamList;

struct Stmt {
    StmtType type;
    int line;
    int column;
    char* src_text;
    union {
        StmtList block;
        struct { Expr* expr; } expr_stmt;
        struct { bool has_type; DeclType decl_type; char* name; Expr* target; Expr* value; } assign;
        struct { DeclType decl_type; char* name; } decl;
        struct {
            Expr* condition;
            Stmt* then_branch;
            ExprList elif_conditions;
            StmtList elif_blocks;
            Stmt* else_branch; // optional
        } if_stmt;
        struct { Expr* condition; Stmt* body; } while_stmt;
        struct { char* counter; Expr* target; Stmt* body; } for_stmt;
        struct { char* counter; Expr* target; Stmt* body; } parfor_stmt;
        struct { char* name; ParamList params; DeclType return_type; Stmt* body; } func_stmt;
        struct { Expr* value; } return_stmt;
        struct { Expr* value; } break_stmt;
        struct { Stmt* body; } async_stmt;
        struct { char* name; Stmt* body; } thr_stmt;
        struct { Stmt* try_block; char* catch_name; Stmt* catch_block; } try_stmt;
        struct { Expr* target; } goto_stmt;
        struct { char* name; } pop_stmt;
        struct { Expr* target; } gotopoint_stmt;
    } as;
};

Expr* expr_int(int64_t value, int line, int column);
Expr* expr_flt(double value, int line, int column);
Expr* expr_str(char* value, int line, int column);
Expr* expr_ptr(char* name, int line, int column);
Expr* expr_ident(char* name, int line, int column);
Expr* expr_call(Expr* callee, int line, int column);
void call_kw_add(Expr* call, char* name, Expr* value);
Expr* expr_tns(int line, int column);
Expr* expr_async(Stmt* block, int line, int column);
Expr* expr_map(int line, int column);
Expr* expr_index(Expr* target, int line, int column);
Expr* expr_range(Expr* start, Expr* end, int line, int column);
Expr* expr_wildcard(int line, int column);
void expr_list_add(ExprList* list, Expr* expr);

Stmt* stmt_block(int line, int column);
Stmt* stmt_async(Stmt* body, int line, int column);
Stmt* stmt_expr(Expr* expr, int line, int column);
Stmt* stmt_assign(bool has_type, DeclType decl_type, char* name, Expr* target, Expr* value, int line, int column);
Stmt* stmt_decl(DeclType decl_type, char* name, int line, int column);
Stmt* stmt_if(Expr* cond, Stmt* then_branch, int line, int column);
Stmt* stmt_while(Expr* cond, Stmt* body, int line, int column);
Stmt* stmt_for(char* counter, Expr* target, Stmt* body, int line, int column);
Stmt* stmt_parfor(char* counter, Expr* target, Stmt* body, int line, int column);
Stmt* stmt_func(char* name, DeclType ret, Stmt* body, int line, int column);
Stmt* stmt_return(Expr* value, int line, int column);
Stmt* stmt_pop(char* name, int line, int column);
Stmt* stmt_break(Expr* value, int line, int column);
Stmt* stmt_continue(int line, int column);
Stmt* stmt_thr(char* name, Stmt* body, int line, int column);
Stmt* stmt_try(Stmt* try_block, char* catch_name, Stmt* catch_block, int line, int column);
Stmt* stmt_goto(Expr* target, int line, int column);
Stmt* stmt_gotopoint(Expr* target, int line, int column);

void stmt_list_add(StmtList* list, Stmt* stmt);
void param_list_add(ParamList* list, Param param);

// Attach original source text (single line) to a statement node.
void stmt_set_src(Stmt* stmt, const char* src);

void free_expr(Expr* expr);
void free_stmt(Stmt* stmt);

#endif // AST_H