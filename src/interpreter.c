#include "interpreter.h"
#include "builtins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

// Forward declarations
static ExecResult exec_stmt(Interpreter* interp, Stmt* stmt, Env* env, LabelMap* labels);
static ExecResult exec_stmt_list(Interpreter* interp, StmtList* list, Env* env, LabelMap* labels);

// ============ Helper functions ============

static void* safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memset(ptr, 0, size);
    return ptr;
}

static void label_map_add(LabelMap* map, Value key, int index) {
    if (map->count + 1 > map->capacity) {
        size_t new_cap = map->capacity == 0 ? 8 : map->capacity * 2;
        map->items = realloc(map->items, new_cap * sizeof(LabelEntry));
        if (!map->items) { fprintf(stderr, "Out of memory\n"); exit(1); }
        map->capacity = new_cap;
    }
    map->items[map->count].key = value_copy(key);
    map->items[map->count].index = index;
    map->count++;
}

static int label_map_find(LabelMap* map, Value key) {
    for (size_t i = 0; i < map->count; i++) {
        if (map->items[i].key.type == key.type) {
            if (key.type == VAL_INT && map->items[i].key.as.i == key.as.i) return map->items[i].index;
            if (key.type == VAL_STR && strcmp(map->items[i].key.as.s, key.as.s) == 0) return map->items[i].index;
        }
    }
    return -1;
}

// ============ Function table ============

FuncTable* func_table_create(void) {
    FuncTable* table = safe_malloc(sizeof(FuncTable));
    return table;
}

void func_table_free(FuncTable* table) {
    if (!table) return;
    FuncEntry* entry = table->entries;
    while (entry) {
        FuncEntry* next = entry->next;
        free(entry->name);
        // Don't free func itself as it may be referenced by values
        free(entry);
        entry = next;
    }
    free(table);
}

bool func_table_add(FuncTable* table, const char* name, Func* func) {
    // Check if already exists
    FuncEntry* entry = table->entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return false;  // Already exists
        }
        entry = entry->next;
    }
    
    // Add new entry
    FuncEntry* new_entry = safe_malloc(sizeof(FuncEntry));
    new_entry->name = strdup(name);
    new_entry->func = func;
    new_entry->next = table->entries;
    table->entries = new_entry;
    table->count++;
    return true;
}

Func* func_table_lookup(FuncTable* table, const char* name) {
    FuncEntry* entry = table->entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->func;
        }
        entry = entry->next;
    }
    return NULL;
}

// ============ Value truthiness ============

int value_truthiness(Value v) {
    switch (v.type) {
        case VAL_INT:
            return v.as.i != 0;
        case VAL_FLT:
            return v.as.f != 0.0;
        case VAL_STR:
            return v.as.s != NULL && v.as.s[0] != '\0';
        case VAL_FUNC:
            return 1;  // Functions are always truthy
        default:
            return 0;
    }
}

// ============ Type conversion helpers ============

static DeclType value_type_to_decl(ValueType vt) {
    switch (vt) {
        case VAL_INT: return TYPE_INT;
        case VAL_FLT: return TYPE_FLT;
        case VAL_STR: return TYPE_STR;
        case VAL_FUNC: return TYPE_FUNC;
        default: return TYPE_UNKNOWN;
    }
}

static ValueType decl_type_to_value(DeclType dt) {
    switch (dt) {
        case TYPE_INT: return VAL_INT;
        case TYPE_FLT: return VAL_FLT;
        case TYPE_STR: return VAL_STR;
        case TYPE_FUNC: return VAL_FUNC;
        default: return VAL_NULL;
    }
}

// ============ Error handling ============

static ExecResult make_error(const char* msg, int line, int col) {
    ExecResult res;
    res.status = EXEC_ERROR;
    res.value = value_null();
    res.break_count = 0;
    res.jump_index = -1;
    res.error = strdup(msg);
    res.error_line = line;
    res.error_column = col;
    return res;
}

static ExecResult make_ok(Value v) {
    ExecResult res;
    res.status = EXEC_OK;
    res.value = v;
    res.break_count = 0;
    res.jump_index = -1;
    res.error = NULL;
    res.error_line = 0;
    res.error_column = 0;
    return res;
}

// ============ Expression evaluation ============

Value eval_expr(Interpreter* interp, Expr* expr, Env* env) {
    if (!expr) return value_null();
    
    switch (expr->type) {
        case EXPR_INT:
            return value_int(expr->as.int_value);
            
        case EXPR_FLT:
            return value_flt(expr->as.flt_value);
            
        case EXPR_STR:
            return value_str(expr->as.str_value);
            
        case EXPR_IDENT: {
            Value v;
            DeclType dtype;
            bool initialized;
            if (!env_get(env, expr->as.ident, &v, &dtype, &initialized)) {
                // Check if it's a function name
                Func* func = func_table_lookup(interp->functions, expr->as.ident);
                if (func) {
                    return value_func(func);
                }
                
                char buf[128];
                snprintf(buf, sizeof(buf), "Undefined identifier '%s'", expr->as.ident);
                interp->error = strdup(buf);
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }
            if (!initialized) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Identifier '%s' declared but not initialized", expr->as.ident);
                interp->error = strdup(buf);
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }
            return v;
        }
        
        case EXPR_CALL: {
            // Get the callee
            const char* func_name = NULL;
            Func* user_func = NULL;
            
            if (expr->as.call.callee->type == EXPR_IDENT) {
                func_name = expr->as.call.callee->as.ident;
                
                // Check builtins first
                BuiltinFunction* builtin = builtin_lookup(func_name);
                if (builtin) {
                    // Evaluate arguments
                    int argc = (int)expr->as.call.args.count;
                    Value* args = NULL;
                    Expr** arg_nodes = NULL;
                    
                    if (argc > 0) {
                        args = safe_malloc(sizeof(Value) * argc);
                        arg_nodes = safe_malloc(sizeof(Expr*) * argc);
                        
                        for (int i = 0; i < argc; i++) {
                            arg_nodes[i] = expr->as.call.args.items[i];
                            
                            // For DEL and EXIST, don't evaluate the argument
                            if ((strcmp(func_name, "DEL") == 0 || strcmp(func_name, "EXIST") == 0) && i == 0) {
                                args[i] = value_null();
                            } else {
                                args[i] = eval_expr(interp, expr->as.call.args.items[i], env);
                                if (interp->error) {
                                    // Clean up
                                    for (int j = 0; j < i; j++) value_free(args[j]);
                                    free(args);
                                    free(arg_nodes);
                                    return value_null();
                                }
                            }
                        }
                    }
                    
                    // Check arg count
                    if (argc < builtin->min_args) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s expects at least %d arguments", func_name, builtin->min_args);
                        interp->error = strdup(buf);
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        if (args) {
                            for (int i = 0; i < argc; i++) value_free(args[i]);
                            free(args);
                            free(arg_nodes);
                        }
                        return value_null();
                    }
                    if (builtin->max_args >= 0 && argc > builtin->max_args) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s expects at most %d arguments", func_name, builtin->max_args);
                        interp->error = strdup(buf);
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        if (args) {
                            for (int i = 0; i < argc; i++) value_free(args[i]);
                            free(args);
                            free(arg_nodes);
                        }
                        return value_null();
                    }
                    
                    // Call builtin
                    Value result = builtin->impl(interp, args, argc, arg_nodes, env, expr->line, expr->column);
                    
                    // Clean up
                    if (args) {
                        for (int i = 0; i < argc; i++) value_free(args[i]);
                        free(args);
                        free(arg_nodes);
                    }
                    
                    return result;
                }
                
                // Check user-defined functions
                user_func = func_table_lookup(interp->functions, func_name);
                if (!user_func) {
                    // Check if it's a variable holding a function
                    Value v;
                    DeclType dtype;
                    bool initialized;
                    if (env_get(env, func_name, &v, &dtype, &initialized) && initialized && v.type == VAL_FUNC) {
                        user_func = v.as.func;
                    }
                }
            } else {
                // Callee is an expression (like tns[1]())
                Value callee_val = eval_expr(interp, expr->as.call.callee, env);
                if (interp->error) return value_null();
                
                if (callee_val.type != VAL_FUNC) {
                    interp->error = strdup("Cannot call non-function value");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    value_free(callee_val);
                    return value_null();
                }
                user_func = callee_val.as.func;
            }
            
            if (!user_func) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Unknown function '%s'", func_name ? func_name : "<expr>");
                interp->error = strdup(buf);
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }
            
            // Call user-defined function
            int argc = (int)expr->as.call.args.count;
            
            // Create new environment for function call
            Env* call_env = env_create(user_func->closure);
            
            // Bind parameters
            for (size_t i = 0; i < user_func->params.count; i++) {
                Param* param = &user_func->params.items[i];
                Value arg_val;
                
                if ((int)i < argc) {
                    arg_val = eval_expr(interp, expr->as.call.args.items[i], env);
                    if (interp->error) {
                        env_free(call_env);
                        return value_null();
                    }
                } else if (param->default_value) {
                    arg_val = eval_expr(interp, param->default_value, call_env);
                    if (interp->error) {
                        env_free(call_env);
                        return value_null();
                    }
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Missing argument for parameter '%s'", param->name);
                    interp->error = strdup(buf);
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    env_free(call_env);
                    return value_null();
                }
                
                // Type check
                if (value_type_to_decl(arg_val.type) != param->type) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Type mismatch for parameter '%s'", param->name);
                    interp->error = strdup(buf);
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    value_free(arg_val);
                    env_free(call_env);
                    return value_null();
                }
                
                env_define(call_env, param->name, param->type);
                env_assign(call_env, param->name, arg_val, param->type, true);
                value_free(arg_val);
            }
            
            // Execute function body
            LabelMap labels = {0};
            ExecResult res = exec_stmt(interp, user_func->body, call_env, &labels);
            
            // Clean up labels
            for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
            free(labels.items);
            
            env_free(call_env);
            
            if (res.status == EXEC_ERROR) {
                interp->error = res.error;
                interp->error_line = res.error_line;
                interp->error_col = res.error_column;
                return value_null();
            }
            
            if (res.status == EXEC_RETURN) {
                // Type check return value
                if (value_type_to_decl(res.value.type) != user_func->return_type) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Return type mismatch in function '%s'", user_func->name ? user_func->name : "<lambda>");
                    interp->error = strdup(buf);
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    value_free(res.value);
                    return value_null();
                }
                return res.value;
            }
            
            // No explicit return - return default value
            switch (user_func->return_type) {
                case TYPE_INT: return value_int(0);
                case TYPE_FLT: return value_flt(0.0);
                case TYPE_STR: return value_str("");
                case TYPE_FUNC:
                    interp->error = strdup("FUNC-returning function must return a value");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    return value_null();
                default:
                    return value_null();
            }
        }
        
        default:
            interp->error = strdup("Unknown expression type");
            interp->error_line = expr->line;
            interp->error_col = expr->column;
            return value_null();
    }
}

// ============ Statement execution ============

static ExecResult exec_stmt(Interpreter* interp, Stmt* stmt, Env* env, LabelMap* labels) {
    if (!stmt) return make_ok(value_null());
    
    switch (stmt->type) {
        case STMT_BLOCK:
            return exec_stmt_list(interp, &stmt->as.block, env, labels);
            
        case STMT_EXPR: {
            Value v = eval_expr(interp, stmt->as.expr_stmt.expr, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            value_free(v);
            return make_ok(value_null());
        }
        
        case STMT_DECL: {
            env_define(env, stmt->as.decl.name, stmt->as.decl.decl_type);
            return make_ok(value_null());
        }
        
        case STMT_ASSIGN: {
            Value v = eval_expr(interp, stmt->as.assign.value, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            if (stmt->as.assign.has_type) {
                // Declaration with assignment
                DeclType expected = stmt->as.assign.decl_type;
                DeclType actual = value_type_to_decl(v.type);
                
                if (expected != actual) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Type mismatch: expected %s but got %s",
                             expected == TYPE_INT ? "INT" : 
                             expected == TYPE_FLT ? "FLT" :
                             expected == TYPE_STR ? "STR" : "FUNC",
                             value_type_name(v));
                    value_free(v);
                    return make_error(buf, stmt->line, stmt->column);
                }
                
                env_define(env, stmt->as.assign.name, expected);
                env_assign(env, stmt->as.assign.name, v, expected, true);
            } else {
                // Assignment without type - must be previously declared
                if (!env_assign(env, stmt->as.assign.name, v, TYPE_UNKNOWN, false)) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Cannot assign to undeclared identifier '%s'", stmt->as.assign.name);
                    value_free(v);
                    return make_error(buf, stmt->line, stmt->column);
                }
            }
            value_free(v);
            return make_ok(value_null());
        }
        
        case STMT_IF: {
            // Evaluate condition
            Value cond = eval_expr(interp, stmt->as.if_stmt.condition, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            if (value_truthiness(cond)) {
                value_free(cond);
                return exec_stmt(interp, stmt->as.if_stmt.then_branch, env, labels);
            }
            value_free(cond);
            
            // Check elif branches
            for (size_t i = 0; i < stmt->as.if_stmt.elif_conditions.count; i++) {
                Value elif_cond = eval_expr(interp, stmt->as.if_stmt.elif_conditions.items[i], env);
                if (interp->error) {
                    return make_error(interp->error, interp->error_line, interp->error_col);
                }
                
                if (value_truthiness(elif_cond)) {
                    value_free(elif_cond);
                    return exec_stmt(interp, stmt->as.if_stmt.elif_blocks.items[i], env, labels);
                }
                value_free(elif_cond);
            }
            
            // Check else branch
            if (stmt->as.if_stmt.else_branch) {
                return exec_stmt(interp, stmt->as.if_stmt.else_branch, env, labels);
            }
            
            return make_ok(value_null());
        }
        
        case STMT_WHILE: {
            interp->loop_depth++;
            
            while (1) {
                Value cond = eval_expr(interp, stmt->as.while_stmt.condition, env);
                if (interp->error) {
                    interp->loop_depth--;
                    return make_error(interp->error, interp->error_line, interp->error_col);
                }
                
                if (!value_truthiness(cond)) {
                    value_free(cond);
                    break;
                }
                value_free(cond);
                
                ExecResult res = exec_stmt(interp, stmt->as.while_stmt.body, env, labels);
                
                if (res.status == EXEC_ERROR || res.status == EXEC_RETURN || res.status == EXEC_GOTO) {
                    interp->loop_depth--;
                    return res;
                }
                
                if (res.status == EXEC_BREAK) {
                    if (res.break_count > 1) {
                        res.break_count--;
                        interp->loop_depth--;
                        return res;
                    }
                    break;
                }
                
                if (res.status == EXEC_CONTINUE) {
                    continue;
                }
            }
            
            interp->loop_depth--;
            return make_ok(value_null());
        }
        
        case STMT_FOR: {
            // Evaluate target once
            Value target = eval_expr(interp, stmt->as.for_stmt.target, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            if (target.type != VAL_INT) {
                value_free(target);
                return make_error("FOR target must be INT", stmt->line, stmt->column);
            }
            
            int64_t limit = target.as.i;
            value_free(target);
            
            // Save any existing binding for counter
            Value old_val;
            DeclType old_type;
            bool old_init;
            bool had_counter = env_get(env, stmt->as.for_stmt.counter, &old_val, &old_type, &old_init);
            
            // Create counter
            env_define(env, stmt->as.for_stmt.counter, TYPE_INT);
            
            interp->loop_depth++;
            
            for (int64_t i = 0; i < limit; i++) {
                env_assign(env, stmt->as.for_stmt.counter, value_int(i), TYPE_INT, true);
                
                ExecResult res = exec_stmt(interp, stmt->as.for_stmt.body, env, labels);
                
                if (res.status == EXEC_ERROR || res.status == EXEC_RETURN || res.status == EXEC_GOTO) {
                    interp->loop_depth--;
                    // Restore counter
                    env_delete(env, stmt->as.for_stmt.counter);
                    if (had_counter && old_init) {
                        env_define(env, stmt->as.for_stmt.counter, old_type);
                        env_assign(env, stmt->as.for_stmt.counter, old_val, old_type, true);
                    }
                    if (had_counter) value_free(old_val);
                    return res;
                }
                
                if (res.status == EXEC_BREAK) {
                    if (res.break_count > 1) {
                        res.break_count--;
                        interp->loop_depth--;
                        env_delete(env, stmt->as.for_stmt.counter);
                        if (had_counter && old_init) {
                            env_define(env, stmt->as.for_stmt.counter, old_type);
                            env_assign(env, stmt->as.for_stmt.counter, old_val, old_type, true);
                        }
                        if (had_counter) value_free(old_val);
                        return res;
                    }
                    break;
                }
                
                if (res.status == EXEC_CONTINUE) {
                    continue;
                }
            }
            
            interp->loop_depth--;
            
            // Restore counter
            env_delete(env, stmt->as.for_stmt.counter);
            if (had_counter && old_init) {
                env_define(env, stmt->as.for_stmt.counter, old_type);
                env_assign(env, stmt->as.for_stmt.counter, old_val, old_type, true);
            }
            if (had_counter) value_free(old_val);
            
            return make_ok(value_null());
        }
        
        case STMT_FUNC: {
            // Check if name conflicts with builtin
            if (is_builtin(stmt->as.func_stmt.name)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Cannot define function with builtin name '%s'", stmt->as.func_stmt.name);
                return make_error(buf, stmt->line, stmt->column);
            }
            
            // Create function object
            Func* func = safe_malloc(sizeof(Func));
            func->name = strdup(stmt->as.func_stmt.name);
            func->return_type = stmt->as.func_stmt.return_type;
            func->params = stmt->as.func_stmt.params;
            func->body = stmt->as.func_stmt.body;
            func->closure = env;
            
            // Add to function table
            func_table_add(interp->functions, func->name, func);
            
            // Also store as a FUNC value in environment
            env_define(env, func->name, TYPE_FUNC);
            env_assign(env, func->name, value_func(func), TYPE_FUNC, true);
            
            return make_ok(value_null());
        }
        
        case STMT_RETURN: {
            Value v = eval_expr(interp, stmt->as.return_stmt.value, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            ExecResult res;
            res.status = EXEC_RETURN;
            res.value = v;
            res.break_count = 0;
            res.jump_index = -1;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            return res;
        }
        
        case STMT_BREAK: {
            Value v = eval_expr(interp, stmt->as.break_stmt.value, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            if (v.type != VAL_INT) {
                value_free(v);
                return make_error("BREAK argument must be INT", stmt->line, stmt->column);
            }
            
            if (v.as.i <= 0) {
                value_free(v);
                return make_error("BREAK argument must be positive", stmt->line, stmt->column);
            }
            
            if (v.as.i > interp->loop_depth) {
                value_free(v);
                return make_error("BREAK count exceeds loop nesting depth", stmt->line, stmt->column);
            }
            
            ExecResult res;
            res.status = EXEC_BREAK;
            res.value = value_null();
            res.break_count = (int)v.as.i;
            res.jump_index = -1;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            value_free(v);
            return res;
        }
        
        case STMT_CONTINUE: {
            if (interp->loop_depth <= 0) {
                return make_error("CONTINUE outside of loop", stmt->line, stmt->column);
            }
            
            ExecResult res;
            res.status = EXEC_CONTINUE;
            res.value = value_null();
            res.break_count = 0;
            res.jump_index = -1;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            return res;
        }
        
        case STMT_TRY: {
            bool old_in_try = interp->in_try_block;
            interp->in_try_block = true;
            
            // Execute try block
            ExecResult res = exec_stmt(interp, stmt->as.try_stmt.try_block, env, labels);
            
            interp->in_try_block = old_in_try;
            
            if (res.status == EXEC_ERROR) {
                // Create catch environment
                Env* catch_env = env;
                
                // If there's a catch name, bind the error message
                if (stmt->as.try_stmt.catch_name) {
                    env_define(catch_env, stmt->as.try_stmt.catch_name, TYPE_STR);
                    env_assign(catch_env, stmt->as.try_stmt.catch_name, 
                              value_str(res.error ? res.error : "Error"), TYPE_STR, true);
                }
                
                free(res.error);
                free(interp->error);
                interp->error = NULL;
                
                // Execute catch block
                ExecResult catch_res = exec_stmt(interp, stmt->as.try_stmt.catch_block, catch_env, labels);
                
                // Clean up catch binding
                if (stmt->as.try_stmt.catch_name) {
                    env_delete(catch_env, stmt->as.try_stmt.catch_name);
                }
                
                return catch_res;
            }
            
            return res;
        }
        
        case STMT_GOTO: {
            Value target = eval_expr(interp, stmt->as.goto_stmt.target, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            int idx = label_map_find(labels, target);
            value_free(target);
            
            if (idx < 0) {
                return make_error("GOTO target not found", stmt->line, stmt->column);
            }
            
            ExecResult res;
            res.status = EXEC_GOTO;
            res.value = value_null();
            res.break_count = 0;
            res.jump_index = idx;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            return res;
        }
        
        case STMT_GOTOPOINT: {
            Value target = eval_expr(interp, stmt->as.gotopoint_stmt.target, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            
            // Register this gotopoint - the index will be set by exec_stmt_list
            // For now, just return OK
            value_free(target);
            return make_ok(value_null());
        }
        
        default:
            return make_error("Unknown statement type", stmt->line, stmt->column);
    }
}

static ExecResult exec_stmt_list(Interpreter* interp, StmtList* list, Env* env, LabelMap* labels) {
    // First pass: collect gotopoints
    for (size_t i = 0; i < list->count; i++) {
        Stmt* stmt = list->items[i];
        if (stmt->type == STMT_GOTOPOINT) {
            Value target = eval_expr(interp, stmt->as.gotopoint_stmt.target, env);
            if (interp->error) {
                return make_error(interp->error, interp->error_line, interp->error_col);
            }
            label_map_add(labels, target, (int)i);
            value_free(target);
        }
    }
    
    // Second pass: execute statements
    size_t i = 0;
    while (i < list->count) {
        ExecResult res = exec_stmt(interp, list->items[i], env, labels);
        
        if (res.status == EXEC_ERROR || res.status == EXEC_RETURN || 
            res.status == EXEC_BREAK || res.status == EXEC_CONTINUE) {
            return res;
        }
        
        if (res.status == EXEC_GOTO) {
            if (res.jump_index >= 0 && res.jump_index < (int)list->count) {
                i = (size_t)res.jump_index;
                continue;
            }
            return res;  // Propagate upward if target not in this block
        }
        
        i++;
    }
    
    return make_ok(value_null());
}

// ============ Main entry point ============

ExecResult exec_program(Stmt* program) {
    Interpreter interp = {0};
    interp.global_env = env_create(NULL);
    interp.functions = func_table_create();
    interp.loop_depth = 0;
    interp.error = NULL;
    interp.in_try_block = false;
    
    builtins_init();
    
    LabelMap labels = {0};
    ExecResult res = exec_stmt_list(&interp, &program->as.block, interp.global_env, &labels);
    
    // Clean up
    for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
    free(labels.items);
    
    env_free(interp.global_env);
    func_table_free(interp.functions);
    
    return res;
}
