#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void* ast_alloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memset(ptr, 0, size);
    return ptr;
}

Expr* expr_int(int64_t value, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_INT;
    expr->line = line;
    expr->column = column;
    expr->as.int_value = value;
    return expr;
}

Expr* expr_flt(double value, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_FLT;
    expr->line = line;
    expr->column = column;
    expr->as.flt_value = value;
    return expr;
}

Expr* expr_str(char* value, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_STR;
    expr->line = line;
    expr->column = column;
    expr->as.str_value = value;
    return expr;
}

Expr* expr_ident(char* name, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_IDENT;
    expr->line = line;
    expr->column = column;
    expr->as.ident = name;
    return expr;
}

Expr* expr_call(Expr* callee, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_CALL;
    expr->line = line;
    expr->column = column;
    expr->as.call.callee = callee;
    return expr;
}

Expr* expr_tns(int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_TNS;
    expr->line = line;
    expr->column = column;
    expr->as.tns_items.items = NULL;
    expr->as.tns_items.count = 0;
    expr->as.tns_items.capacity = 0;
    return expr;
}

Expr* expr_map(int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_MAP;
    expr->line = line;
    expr->column = column;
    expr->as.map_items.keys.items = NULL;
    expr->as.map_items.keys.count = 0;
    expr->as.map_items.keys.capacity = 0;
    expr->as.map_items.values.items = NULL;
    expr->as.map_items.values.count = 0;
    expr->as.map_items.values.capacity = 0;
    return expr;
}

Expr* expr_index(Expr* target, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_INDEX;
    expr->line = line;
    expr->column = column;
    expr->as.index.target = target;
    expr->as.index.indices.items = NULL;
    expr->as.index.indices.count = 0;
    expr->as.index.indices.capacity = 0;
    return expr;
}

Expr* expr_range(Expr* start, Expr* end, int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_RANGE;
    expr->line = line;
    expr->column = column;
    expr->as.range.start = start;
    expr->as.range.end = end;
    return expr;
}

Expr* expr_wildcard(int line, int column) {
    Expr* expr = ast_alloc(sizeof(Expr));
    expr->type = EXPR_WILDCARD;
    expr->line = line;
    expr->column = column;
    return expr;
}

void expr_list_add(ExprList* list, Expr* expr) {
    if (list->count + 1 > list->capacity) {
        size_t new_cap = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, new_cap * sizeof(Expr*));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        list->capacity = new_cap;
    }
    list->items[list->count++] = expr;
}

Stmt* stmt_block(int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_BLOCK;
    stmt->line = line;
    stmt->column = column;
    return stmt;
}

Stmt* stmt_expr(Expr* expr, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_EXPR;
    stmt->line = line;
    stmt->column = column;
    stmt->as.expr_stmt.expr = expr;
    return stmt;
}

Stmt* stmt_assign(bool has_type, DeclType decl_type, char* name, Expr* target, Expr* value, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_ASSIGN;
    stmt->line = line;
    stmt->column = column;
    stmt->as.assign.has_type = has_type;
    stmt->as.assign.decl_type = decl_type;
    stmt->as.assign.name = name;
    stmt->as.assign.target = target;
    stmt->as.assign.value = value;
    return stmt;
}

Stmt* stmt_decl(DeclType decl_type, char* name, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_DECL;
    stmt->line = line;
    stmt->column = column;
    stmt->as.decl.decl_type = decl_type;
    stmt->as.decl.name = name;
    return stmt;
}

Stmt* stmt_if(Expr* cond, Stmt* then_branch, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_IF;
    stmt->line = line;
    stmt->column = column;
    stmt->as.if_stmt.condition = cond;
    stmt->as.if_stmt.then_branch = then_branch;
    return stmt;
}

Stmt* stmt_while(Expr* cond, Stmt* body, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_WHILE;
    stmt->line = line;
    stmt->column = column;
    stmt->as.while_stmt.condition = cond;
    stmt->as.while_stmt.body = body;
    return stmt;
}

Stmt* stmt_for(char* counter, Expr* target, Stmt* body, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_FOR;
    stmt->line = line;
    stmt->column = column;
    stmt->as.for_stmt.counter = counter;
    stmt->as.for_stmt.target = target;
    stmt->as.for_stmt.body = body;
    return stmt;
}

Stmt* stmt_func(char* name, DeclType ret, Stmt* body, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_FUNC;
    stmt->line = line;
    stmt->column = column;
    stmt->as.func_stmt.name = name;
    stmt->as.func_stmt.return_type = ret;
    stmt->as.func_stmt.body = body;
    return stmt;
}

Stmt* stmt_return(Expr* value, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_RETURN;
    stmt->line = line;
    stmt->column = column;
    stmt->as.return_stmt.value = value;
    return stmt;
}

Stmt* stmt_break(Expr* value, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_BREAK;
    stmt->line = line;
    stmt->column = column;
    stmt->as.break_stmt.value = value;
    return stmt;
}

Stmt* stmt_continue(int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_CONTINUE;
    stmt->line = line;
    stmt->column = column;
    return stmt;
}

Stmt* stmt_try(Stmt* try_block, char* catch_name, Stmt* catch_block, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_TRY;
    stmt->line = line;
    stmt->column = column;
    stmt->as.try_stmt.try_block = try_block;
    stmt->as.try_stmt.catch_name = catch_name;
    stmt->as.try_stmt.catch_block = catch_block;
    return stmt;
}

Stmt* stmt_goto(Expr* target, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_GOTO;
    stmt->line = line;
    stmt->column = column;
    stmt->as.goto_stmt.target = target;
    return stmt;
}

Stmt* stmt_gotopoint(Expr* target, int line, int column) {
    Stmt* stmt = ast_alloc(sizeof(Stmt));
    stmt->type = STMT_GOTOPOINT;
    stmt->line = line;
    stmt->column = column;
    stmt->as.gotopoint_stmt.target = target;
    return stmt;
}

void stmt_list_add(StmtList* list, Stmt* stmt) {
    if (list->count + 1 > list->capacity) {
        size_t new_cap = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, new_cap * sizeof(Stmt*));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        list->capacity = new_cap;
    }
    list->items[list->count++] = stmt;
}

void param_list_add(ParamList* list, Param param) {
    if (list->count + 1 > list->capacity) {
        size_t new_cap = list->capacity == 0 ? 4 : list->capacity * 2;
        list->items = realloc(list->items, new_cap * sizeof(Param));
        if (!list->items) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        list->capacity = new_cap;
    }
    list->items[list->count++] = param;
}

static void free_expr_list(ExprList* list) {
    for (size_t i = 0; i < list->count; i++) {
        free_expr(list->items[i]);
    }
    free(list->items);
}

static void free_stmt_list(StmtList* list) {
    for (size_t i = 0; i < list->count; i++) {
        free_stmt(list->items[i]);
    }
    free(list->items);
}

void free_expr(Expr* expr) {
    if (!expr) return;
    switch (expr->type) {
        case EXPR_STR:
            free(expr->as.str_value);
            break;
        case EXPR_TNS:
            free_expr_list(&expr->as.tns_items);
            break;
            case EXPR_MAP:
                free_expr_list(&expr->as.map_items.keys);
                free_expr_list(&expr->as.map_items.values);
                break;
        case EXPR_INDEX:
            free_expr(expr->as.index.target);
            free_expr_list(&expr->as.index.indices);
            break;
        case EXPR_RANGE:
            free_expr(expr->as.range.start);
            free_expr(expr->as.range.end);
            break;
        case EXPR_IDENT:
            free(expr->as.ident);
            break;
        case EXPR_CALL:
            free_expr(expr->as.call.callee);
            free_expr_list(&expr->as.call.args);
            break;
        default:
            break;
    }
    free(expr);
}

void free_stmt(Stmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case STMT_BLOCK:
            free_stmt_list(&stmt->as.block);
            break;
        case STMT_EXPR:
            free_expr(stmt->as.expr_stmt.expr);
            break;
        case STMT_ASSIGN:
            if (stmt->as.assign.name) free(stmt->as.assign.name);
            if (stmt->as.assign.target) free_expr(stmt->as.assign.target);
            free_expr(stmt->as.assign.value);
            break;
        case STMT_DECL:
            free(stmt->as.decl.name);
            break;
        case STMT_IF:
            free_expr(stmt->as.if_stmt.condition);
            free_stmt(stmt->as.if_stmt.then_branch);
            free_expr_list(&stmt->as.if_stmt.elif_conditions);
            free_stmt_list(&stmt->as.if_stmt.elif_blocks);
            free_stmt(stmt->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            free_expr(stmt->as.while_stmt.condition);
            free_stmt(stmt->as.while_stmt.body);
            break;
        case STMT_FOR:
            free(stmt->as.for_stmt.counter);
            free_expr(stmt->as.for_stmt.target);
            free_stmt(stmt->as.for_stmt.body);
            break;
        case STMT_FUNC:
            free(stmt->as.func_stmt.name);
            for (size_t i = 0; i < stmt->as.func_stmt.params.count; i++) {
                free(stmt->as.func_stmt.params.items[i].name);
                free_expr(stmt->as.func_stmt.params.items[i].default_value);
            }
            free(stmt->as.func_stmt.params.items);
            free_stmt(stmt->as.func_stmt.body);
            break;
        case STMT_RETURN:
            free_expr(stmt->as.return_stmt.value);
            break;
        case STMT_BREAK:
            free_expr(stmt->as.break_stmt.value);
            break;
        case STMT_TRY:
            free_stmt(stmt->as.try_stmt.try_block);
            free(stmt->as.try_stmt.catch_name);
            free_stmt(stmt->as.try_stmt.catch_block);
            break;
        case STMT_GOTO:
            free_expr(stmt->as.goto_stmt.target);
            break;
        case STMT_GOTOPOINT:
            free_expr(stmt->as.gotopoint_stmt.target);
            break;
        default:
            break;
    }
    free(stmt);
}