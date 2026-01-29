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

// ============ Module registry ============

typedef struct ModuleEntry {
    char* name;
    Env* env;
    struct ModuleEntry* next;
} ModuleEntry;

int module_register(Interpreter* interp, const char* name) {
    if (!interp || !name) return -1;
    ModuleEntry* e = interp->modules;
    while (e) {
        if (strcmp(e->name, name) == 0) return 0; // already registered
        e = e->next;
    }
    ModuleEntry* me = safe_malloc(sizeof(ModuleEntry));
    me->name = strdup(name);
    me->env = env_create(NULL);
    me->next = interp->modules;
    interp->modules = me;
    return 0;
}

Env* module_env_lookup(Interpreter* interp, const char* name) {
    if (!interp || !name) return NULL;
    ModuleEntry* e = interp->modules;
    while (e) {
        if (strcmp(e->name, name) == 0) return e->env;
        e = e->next;
    }
    return NULL;
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
        case VAL_TNS: return TYPE_TNS;
        case VAL_FUNC: return TYPE_FUNC;
        default: return TYPE_UNKNOWN;
    }
}

static ValueType decl_type_to_value(DeclType dt) {
    switch (dt) {
        case TYPE_INT: return VAL_INT;
        case TYPE_FLT: return VAL_FLT;
        case TYPE_STR: return VAL_STR;
        case TYPE_TNS: return VAL_TNS;
        case TYPE_FUNC: return VAL_FUNC;
        default: return VAL_NULL;
    }
}

// Compute tensor shape from AST tensor literal. Returns true on success
// and allocates *out_shape (caller must free). On failure sets *err.
static bool ast_tns_compute_shape(Expr* expr, size_t** out_shape, size_t* out_ndim, char** err) {
    if (!expr || expr->type != EXPR_TNS) {
        *err = strdup("Internal: expected tensor AST node");
        return false;
    }
    size_t count = expr->as.tns_items.count;
    if (count == 0) {
        *err = strdup("Tensor literal must be non-empty");
        return false;
    }

    Expr* first = expr->as.tns_items.items[0];
    if (first->type == EXPR_TNS) {
        // All items must be EXPR_TNS and have identical shape
        size_t* child_shape = NULL;
        size_t child_ndim = 0;
        for (size_t i = 0; i < count; i++) {
            Expr* it = expr->as.tns_items.items[i];
            if (it->type != EXPR_TNS) {
                *err = strdup("Mixed nested and non-nested tensor elements");
                return false;
            }
            size_t* s = NULL; size_t nd = 0;
            if (!ast_tns_compute_shape(it, &s, &nd, err)) {
                return false;
            }
            if (i == 0) {
                child_shape = s; child_ndim = nd;
            } else {
                if (nd != child_ndim) {
                    free(s);
                    *err = strdup("Inconsistent tensor shapes in nested literal");
                    free(child_shape);
                    return false;
                }
                for (size_t k = 0; k < nd; k++) {
                    if (s[k] != child_shape[k]) {
                        free(s);
                        *err = strdup("Inconsistent tensor shapes in nested literal");
                        free(child_shape);
                        return false;
                    }
                }
                free(s);
            }
        }

        // Build shape = [count] + child_shape
        *out_ndim = child_ndim + 1;
        *out_shape = malloc(sizeof(size_t) * (*out_ndim));
        (*out_shape)[0] = count;
        for (size_t k = 0; k < child_ndim; k++) (*out_shape)[k+1] = child_shape[k];
        free(child_shape);
        return true;
    } else {
        // Leaf level
        *out_ndim = 1;
        *out_shape = malloc(sizeof(size_t));
        (*out_shape)[0] = count;
        return true;
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

// Clear the interpreter error after handling it
static void clear_error(Interpreter* interp) {
    if (interp->error) {
        free(interp->error);
        interp->error = NULL;
        interp->error_line = 0;
        interp->error_col = 0;
    }
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
                            
                            // For DEL, EXIST, and IMPORT, don't evaluate the first argument.
                            // For IMPORT_PATH, don't evaluate the second (alias) argument.
                            if (((strcmp(func_name, "DEL") == 0 || strcmp(func_name, "EXIST") == 0 || strcmp(func_name, "IMPORT") == 0) && i == 0)
                                || (strcmp(func_name, "IMPORT_PATH") == 0 && i == 1)) {
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
                if (!env_assign(call_env, param->name, arg_val, param->type, true)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", param->name);
                    interp->error = strdup(buf);
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    value_free(arg_val);
                    env_free(call_env);
                    return value_null();
                }
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
                /* Copy the error message into the interpreter-owned slot and
                   free the ExecResult-owned string to avoid ambiguous ownership
                   (which previously could lead to double-free or use-after-free
                   and cause the interpreter to hang). */
                interp->error = res.error ? strdup(res.error) : strdup("Error");
                interp->error_line = res.error_line;
                interp->error_col = res.error_column;
                free(res.error);
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
        case EXPR_TNS: {
            // Compute shape
            size_t* shape = NULL;
            size_t ndim = 0;
            char* serr = NULL;
            if (!ast_tns_compute_shape(expr, &shape, &ndim, &serr)) {
                interp->error = serr ? serr : strdup("Invalid tensor literal");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }

            size_t total = 1;
            for (size_t d = 0; d < ndim; d++) total *= shape[d];

            Value* items = malloc(sizeof(Value) * (total ? total : 1));
            size_t pos = 0;

            for (size_t i = 0; i < expr->as.tns_items.count; i++) {
                Expr* it = expr->as.tns_items.items[i];
                if (it->type == EXPR_TNS) {
                    Value cv = eval_expr(interp, it, env);
                    if (interp->error) { goto tns_eval_fail; }
                    if (cv.type != VAL_TNS) {
                        interp->error = strdup("Nested tensor literal did not evaluate to tensor");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(cv);
                        goto tns_eval_fail;
                    }
                    Tensor* ct = cv.as.tns;
                    if (ct->ndim != (ndim ? ndim - 1 : 0)) {
                        interp->error = strdup("Nested tensor shape mismatch");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(cv);
                        goto tns_eval_fail;
                    }
                    for (size_t d = 0; d < ct->ndim; d++) {
                        if (ct->shape[d] != shape[d+1]) {
                            interp->error = strdup("Nested tensor shape mismatch");
                            interp->error_line = it->line;
                            interp->error_col = it->column;
                            value_free(cv);
                            goto tns_eval_fail;
                        }
                    }
                    for (size_t k = 0; k < ct->length; k++) items[pos++] = value_copy(ct->data[k]);
                    value_free(cv);
                } else {
                    Value v = eval_expr(interp, it, env);
                    if (interp->error) { goto tns_eval_fail; }
                    items[pos++] = value_copy(v);
                    value_free(v);
                }
            }

            if (pos != total) {
                interp->error = strdup("Internal: tensor flatten length mismatch");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                goto tns_eval_fail;
            }

            if (total == 0) {
                interp->error = strdup("Empty tensor literal");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                goto tns_eval_fail;
            }

            DeclType elem_decl = value_type_to_decl(items[0].type);
            for (size_t j = 1; j < total; j++) {
                if (value_type_to_decl(items[j].type) != elem_decl) {
                    interp->error = strdup("Tensor literal elements must have uniform type");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    goto tns_eval_fail;
                }
            }

            Value out = value_tns_from_values(elem_decl, ndim, shape, items, total);
            for (size_t j = 0; j < total; j++) value_free(items[j]);
            free(items);
            free(shape);
            return out;

tns_eval_fail:
            for (size_t j = 0; j < pos; j++) value_free(items[j]);
            free(items);
            free(shape);
            return value_null();
        }
        case EXPR_MAP: {
            // Evaluate map literal: keys and values
            Expr* mp = expr;
            Value mv = value_map_new();
            size_t count = mp->as.map_items.keys.count;
            for (size_t i = 0; i < count; i++) {
                Expr* kexpr = mp->as.map_items.keys.items[i];
                Expr* vexpr = mp->as.map_items.values.items[i];
                Value k = eval_expr(interp, kexpr, env);
                if (interp->error) { value_free(k); value_free(mv); return value_null(); }
                if (!(k.type == VAL_INT || k.type == VAL_STR || k.type == VAL_FLT)) {
                    value_free(k);
                    value_free(mv);
                    interp->error = strdup("Map keys must be INT, FLT or STR");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    return value_null();
                }
                Value v = eval_expr(interp, vexpr, env);
                if (interp->error) { value_free(k); value_free(mv); return value_null(); }
                value_map_set(&mv, k, v);
                value_free(k);
                value_free(v);
            }
            return mv;
        }
        case EXPR_INDEX: {
            // Evaluate target
            Expr* target = expr->as.index.target;
            Value tval = eval_expr(interp, target, env);
            if (interp->error) return value_null();
            size_t nidx = expr->as.index.indices.count;
            if (nidx == 0) {
                value_free(tval);
                interp->error = strdup("Empty index list");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }

            if (tval.type == VAL_MAP) {
                // map indexing: support nested lookups m<k1,k2>
                Value cur = tval;
                for (size_t i = 0; i < nidx; i++) {
                    Expr* it = expr->as.index.indices.items[i];
                    Value key = eval_expr(interp, it, env);
                    if (interp->error) { value_free(cur); return value_null(); }
                    if (!(key.type == VAL_INT || key.type == VAL_STR || key.type == VAL_FLT)) {
                        value_free(key); value_free(cur);
                        interp->error = strdup("Map index must be INT, FLT or STR");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        return value_null();
                    }
                    int found = 0;
                    Value got = value_map_get(cur, key, &found);
                    value_free(key);
                    // If not found, return null (missing key)
                    if (!found) { value_free(cur); return value_null(); }
                    // If this is last index, return the found value
                    if (i + 1 == nidx) {
                        // free original map container if it was a copy
                        if (cur.type == VAL_MAP) value_free(cur);
                        return got;
                    }
                    // Otherwise, continue descending; found must be a map
                    if (got.type != VAL_MAP) {
                        value_free(got);
                        value_free(cur);
                        interp->error = strdup("Attempted nested map indexing on non-map value");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        return value_null();
                    }
                    // replace cur with got and continue
                    if (cur.type == VAL_MAP) value_free(cur);
                    cur = got;
                }
                // Shouldn't reach here
                value_free(cur);
                return value_null();
            }

            if (tval.type != VAL_TNS) {
                interp->error = strdup("Indexing is supported only on tensors and maps");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                value_free(tval);
                return value_null();
            }
            Tensor* t = tval.as.tns;

            // Check whether all indices are simple integer indexes
            bool all_int = true;
            for (size_t i = 0; i < nidx; i++) {
                Expr* it = expr->as.index.indices.items[i];
                if (it->type != EXPR_INT) {
                    all_int = false;
                    break;
                }
            }

            // If all are ints and count <= ndim, perform direct get
            if (all_int) {
                // Build 0-based idx array
                size_t* idxs = malloc(sizeof(size_t) * nidx);
                for (size_t i = 0; i < nidx; i++) {
                    Expr* it = expr->as.index.indices.items[i];
                    Value vi = eval_expr(interp, it, env);
                    if (interp->error) {
                        free(idxs);
                        value_free(tval);
                        return value_null();
                    }
                    if (vi.type != VAL_INT) {
                        interp->error = strdup("Index expression must evaluate to INT");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(vi);
                        free(idxs);
                        value_free(tval);
                        return value_null();
                    }
                    int64_t v = vi.as.i;
                    // convert 1-based/negative to 0-based
                    int64_t dim = (int64_t)t->shape[i];
                    if (v < 0) v = dim + v + 1;
                    if (v < 1 || v > dim) {
                        interp->error = strdup("Index out of range");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(vi);
                        free(idxs);
                        value_free(tval);
                        return value_null();
                    }
                    idxs[i] = (size_t)(v - 1);
                    value_free(vi);
                }
                Value out = value_tns_get(tval, idxs, nidx);
                free(idxs);
                value_free(tval);
                return out;
            }

            // Mixed case: build starts/ends arrays (1-based inclusive per value_tns_slice)
            int64_t* starts = malloc(sizeof(int64_t) * nidx);
            int64_t* ends = malloc(sizeof(int64_t) * nidx);
            for (size_t i = 0; i < nidx; i++) {
                Expr* it = expr->as.index.indices.items[i];
                if (it->type == EXPR_WILDCARD) {
                    starts[i] = 1;
                    ends[i] = (int64_t)t->shape[i];
                } else if (it->type == EXPR_RANGE) {
                    // evaluate start and end
                    Value vs = eval_expr(interp, it->as.range.start, env);
                    if (interp->error) { free(starts); free(ends); value_free(tval); return value_null(); }
                    Value ve = eval_expr(interp, it->as.range.end, env);
                    if (interp->error) { value_free(vs); free(starts); free(ends); value_free(tval); return value_null(); }
                    if (vs.type != VAL_INT || ve.type != VAL_INT) {
                        interp->error = strdup("Range bounds must be INT");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(vs); value_free(ve); free(starts); free(ends); value_free(tval); return value_null();
                    }
                    starts[i] = vs.as.i;
                    ends[i] = ve.as.i;
                    value_free(vs); value_free(ve);
                } else {
                    // general expression: expect INT
                    Value vi = eval_expr(interp, it, env);
                    if (interp->error) { free(starts); free(ends); value_free(tval); return value_null(); }
                    if (vi.type != VAL_INT) {
                        interp->error = strdup("Index expression must evaluate to INT");
                        interp->error_line = it->line;
                        interp->error_col = it->column;
                        value_free(vi); free(starts); free(ends); value_free(tval); return value_null();
                    }
                    starts[i] = vi.as.i;
                    ends[i] = vi.as.i;
                    value_free(vi);
                }
            }

            Value out = value_tns_slice(tval, starts, ends, nidx);
            free(starts); free(ends);
            value_free(tval);
            return out;
        }
        
        default:
            interp->error = strdup("Unknown expression type");
            interp->error_line = expr->line;
            interp->error_col = expr->column;
            return value_null();
    }
}

// ============ Statement execution ============

// Recursive helper to assign a value into a nested map path.
// map_ptr: pointer to the Value (must be VAL_MAP) to operate on.
// keys: list of key Expr nodes
// idx: current index into keys
// rhs: value to assign (ownership: caller retains ownership; this function MUST NOT free rhs)
static ExecResult assign_map_nested(Interpreter* interp, Env* env, Value* map_ptr, ExprList* keys, size_t idx, Value rhs, int stmt_line, int stmt_col) {
    if (idx >= keys->count) {
        return make_error("Internal: missing key in nested assignment", stmt_line, stmt_col);
    }
    // evaluate key
    Expr* kexpr = keys->items[idx];
    Value key = eval_expr(interp, kexpr, env);
    if (interp->error) {
        ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
        clear_error(interp);
        return err;
    }
    if (!(key.type == VAL_INT || key.type == VAL_STR || key.type == VAL_FLT)) {
        value_free(key);
        return make_error("Map index must be INT, FLT or STR", kexpr->line, kexpr->column);
    }

    if (idx + 1 == keys->count) {
        // final key: set rhs here (value_map_set copies rhs)
        value_map_set(map_ptr, key, rhs);
        value_free(key);
        return make_ok(value_null());
    }

    // intermediate: ensure child map exists
    int found = 0;
    Value child = value_map_get(*map_ptr, key, &found);
    if (!found) {
        Value nm = value_map_new();
        value_map_set(map_ptr, key, nm);
        value_free(nm);
        child = value_map_get(*map_ptr, key, &found);
        if (!found) {
            value_free(key);
            return make_error("Internal error creating nested map", stmt_line, stmt_col);
        }
    }

    if (child.type != VAL_MAP) {
        value_free(child);
        value_free(key);
        return make_error("Attempted nested map indexing on non-map value", kexpr->line, kexpr->column);
    }

    // Recurse into child (child is a copy). Mutate child, then write it back into parent.
    ExecResult res = assign_map_nested(interp, env, &child, keys, idx + 1, rhs, stmt_line, stmt_col);
    if (res.status == EXEC_ERROR) {
        value_free(child);
        value_free(key);
        return res;
    }

    // write back modified child into parent map under key
    value_map_set(map_ptr, key, child);
    value_free(child);
    value_free(key);
    return make_ok(value_null());
}

static ExecResult exec_stmt(Interpreter* interp, Stmt* stmt, Env* env, LabelMap* labels) {
    if (!stmt) return make_ok(value_null());

    switch (stmt->type) {
        case STMT_BLOCK:
            return exec_stmt_list(interp, &stmt->as.block, env, labels);

        case STMT_EXPR: {
            Value v = eval_expr(interp, stmt->as.expr_stmt.expr, env);
            if (interp->error) {
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
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
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }

            // If this assignment has an expression target (indexed assignment), handle specially
            if (stmt->as.assign.target) {
                if (stmt->as.assign.target->type != EXPR_INDEX) {
                    value_free(v);
                    return make_error("Can only assign to indexed targets or identifiers", stmt->line, stmt->column);
                }
                Expr* idx = stmt->as.assign.target;
                Expr* base = idx->as.index.target;

                // Two supported forms:
                // 1) base is an identifier: treat indices on this node as a multi-key assignment
                // 2) base is a nested index chain whose innermost target is an identifier:
                //    flatten the chain's index lists into a single key list and perform assignment
                if (base && base->type == EXPR_IDENT) {
                    EnvEntry* entry = env_get_entry(env, base->as.ident);
                    if (!entry) {
                        value_free(v);
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Cannot assign to undeclared identifier '%s'", base->as.ident);
                        return make_error(buf, stmt->line, stmt->column);
                    }
                    if (!entry->initialized) {
                        entry->value = value_map_new();
                        entry->initialized = true;
                    }
                    if (entry->value.type != VAL_MAP) {
                        value_free(v);
                        return make_error("Attempting indexed assignment on non-map value", stmt->line, stmt->column);
                    }

                    // Delegate to recursive helper using this node's indices
                    ExecResult ar = assign_map_nested(interp, env, &entry->value, &idx->as.index.indices, 0, v, stmt->line, stmt->column);
                    if (ar.status == EXEC_ERROR) {
                        value_free(v);
                        return ar;
                    }
                    // value_map_set copies rhs; free local rhs
                    value_free(v);
                    return make_ok(value_null());
                }

                // Handle chained indexing e.g. outer<"b"><"x">: collect chain nodes
                if (base && base->type == EXPR_INDEX) {
                    // Walk the chain, collecting nodes
                    Expr* node = stmt->as.assign.target;
                    // Determine chain length
                    size_t chain_len = 0;
                    Expr* walker = node;
                    while (walker && walker->type == EXPR_INDEX) {
                        chain_len++;
                        walker = walker->as.index.target;
                    }
                    // walker should now point to innermost target
                    if (!walker || walker->type != EXPR_IDENT) {
                        value_free(v);
                        return make_error("Indexed assignment base must be an identifier", stmt->line, stmt->column);
                    }

                    // Lookup the environment entry for the innermost identifier
                    EnvEntry* entry = env_get_entry(env, walker->as.ident);
                    if (!entry) {
                        value_free(v);
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Cannot assign to undeclared identifier '%s'", walker->as.ident);
                        return make_error(buf, stmt->line, stmt->column);
                    }
                    if (!entry->initialized) {
                        entry->value = value_map_new();
                        entry->initialized = true;
                    }
                    if (entry->value.type != VAL_MAP) {
                        value_free(v);
                        return make_error("Attempting indexed assignment on non-map value", stmt->line, stmt->column);
                    }

                    // Build combined ExprList of all keys from innermost -> outermost
                    ExprList combined = {0};
                    // allocate by iterating again and summing counts
                    size_t total_keys = 0;
                    walker = stmt->as.assign.target;
                    while (walker && walker->type == EXPR_INDEX) {
                        total_keys += walker->as.index.indices.count;
                        walker = walker->as.index.target;
                    }
                    // Reserve
                    if (total_keys) {
                        combined.items = malloc(sizeof(Expr*) * total_keys);
                        combined.capacity = total_keys;
                    } else {
                        combined.items = NULL;
                        combined.capacity = 0;
                    }
                    combined.count = 0;

                    // collect in reverse (innermost first)
                    Expr** nodes = malloc(sizeof(Expr*) * chain_len);
                    size_t ni = 0;
                    walker = stmt->as.assign.target;
                    while (walker && walker->type == EXPR_INDEX) {
                        nodes[ni++] = walker;
                        walker = walker->as.index.target;
                    }
                    // nodes[0] = outermost, nodes[ni-1] = innermost
                    for (int k = (int)ni - 1; k >= 0; k--) {
                        Expr* n = nodes[k];
                        for (size_t j = 0; j < n->as.index.indices.count; j++) {
                            combined.items[combined.count++] = n->as.index.indices.items[j];
                        }
                    }
                    free(nodes);

                    // Delegate to recursive helper with combined list
                    ExecResult ar = assign_map_nested(interp, env, &entry->value, &combined, 0, v, stmt->line, stmt->column);
                    if (ar.status == EXEC_ERROR) {
                        value_free(v);
                        if (combined.items) free(combined.items);
                        return ar;
                    }

                    if (combined.items) free(combined.items);
                    value_free(v);
                    return make_ok(value_null());
                }

                // Unsupported base expression
                value_free(v);
                return make_error("Indexed assignment base must be an identifier or an index chain", stmt->line, stmt->column);
            }

            if (stmt->as.assign.has_type) {
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
                if (!env_assign(env, stmt->as.assign.name, v, expected, true)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", stmt->as.assign.name);
                    value_free(v);
                    return make_error(buf, stmt->line, stmt->column);
                }
            } else {
                if (!env_assign(env, stmt->as.assign.name, v, TYPE_UNKNOWN, false)) {
                    EnvEntry* echeck = env_get_entry(env, stmt->as.assign.name);
                    if (echeck) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", stmt->as.assign.name);
                        value_free(v);
                        return make_error(buf, stmt->line, stmt->column);
                    }
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Cannot assign to undeclared identifier '%s'", stmt->as.assign.name);
                    value_free(v);
                    return make_error(buf, stmt->line, stmt->column);
                }
            }
            value_free(v);
            return make_ok(value_null());
        }

        case STMT_FUNC: {
            // Register user-defined function in the interpreter
            Func* f = safe_malloc(sizeof(Func));
            f->name = strdup(stmt->as.func_stmt.name);
            f->return_type = stmt->as.func_stmt.return_type;
            f->body = stmt->as.func_stmt.body;
            // Copy parameters
            ParamList* src = &stmt->as.func_stmt.params;
            f->params.count = src->count;
            f->params.items = NULL;
            f->params.capacity = src->capacity;
            if (src->count > 0) {
                f->params.items = safe_malloc(sizeof(Param) * src->count);
                for (size_t i = 0; i < src->count; i++) {
                    f->params.items[i].type = src->items[i].type;
                    f->params.items[i].name = strdup(src->items[i].name);
                    f->params.items[i].default_value = src->items[i].default_value; // share AST node
                }
            }
            // Closure is current environment
            f->closure = env;

            if (!func_table_add(interp->functions, f->name, f)) {
                // Name conflict
                free(f->name);
                // free params names
                for (size_t i = 0; i < f->params.count; i++) free(f->params.items[i].name);
                free(f->params.items);
                free(f);
                return make_error("Function name already defined", stmt->line, stmt->column);
            }

            return make_ok(value_null());
        }

        case STMT_RETURN: {
            // Evaluate return expression and propagate as EXEC_RETURN
            Value v = eval_expr(interp, stmt->as.return_stmt.value, env);
            if (interp->error) {
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }
            ExecResult res;
            res.status = EXEC_RETURN;
            res.value = v; // caller will own and free
            res.break_count = 0;
            res.jump_index = -1;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            return res;
        }

        case STMT_POP: {
            // POP: retrieve identifier value, delete binding, and return the value
            const char* name = stmt->as.pop_stmt.name;
            // POP is only valid inside a function (env != global_env)
            if (env == interp->global_env) {
                return make_error("POP used outside function", stmt->line, stmt->column);
            }

            Value v;
            DeclType dtype;
            bool initialized = false;
            if (!env_get(env, name, &v, &dtype, &initialized) || !initialized) {
                return make_error("Cannot POP undefined or uninitialized identifier", stmt->line, stmt->column);
            }

            if (!env_delete(env, name)) {
                value_free(v);
                return make_error("Failed to delete identifier during POP", stmt->line, stmt->column);
            }

            ExecResult res;
            res.status = EXEC_RETURN;
            res.value = v; // transfer ownership to caller
            res.break_count = 0;
            res.jump_index = -1;
            res.error = NULL;
            res.error_line = 0;
            res.error_column = 0;
            return res;
        }

        case STMT_TRY: {
            // Execute try block and, on error, run catch block if present
            bool prev_in_try = interp->in_try_block;
            interp->in_try_block = true;
            ExecResult tres = exec_stmt(interp, stmt->as.try_stmt.try_block, env, labels);
            interp->in_try_block = prev_in_try;

            if (tres.status == EXEC_ERROR) {
                // Determine error message
                char* msg = NULL;
                if (tres.error) {
                    msg = strdup(tres.error);
                } else if (interp->error) {
                    msg = strdup(interp->error);
                } else {
                    msg = strdup("Error");
                }

                // Clear interpreter error state
                clear_error(interp);

                // If a catch block exists, bind the catch name (if any) and execute it
                if (stmt->as.try_stmt.catch_block) {
                    if (stmt->as.try_stmt.catch_name) {
                        env_define(env, stmt->as.try_stmt.catch_name, TYPE_STR);
                        if (!env_assign(env, stmt->as.try_stmt.catch_name, value_str(msg), TYPE_STR, true)) {
                            free(msg);
                            return make_error("Cannot bind catch name (frozen)", stmt->line, stmt->column);
                        }
                    }
                    free(msg);
                    ExecResult cres = exec_stmt(interp, stmt->as.try_stmt.catch_block, env, labels);
                    if (cres.status == EXEC_ERROR) return cres;
                    return cres;
                }

                free(msg);
                // No catch -> propagate error upward
                return tres;
            }

            // No error in try block -> normal completion
            return tres;
        }

        case STMT_BREAK: {
            // Evaluate break count expression
            Value v = eval_expr(interp, stmt->as.break_stmt.value, env);
            if (interp->error) {
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }
            if (v.type != VAL_INT) {
                value_free(v);
                return make_error("BREAK requires INT argument", stmt->line, stmt->column);
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

        case STMT_IF: {
            Value cond = eval_expr(interp, stmt->as.if_stmt.condition, env);
            if (interp->error) {
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }

            if (value_truthiness(cond)) {
                value_free(cond);
                return exec_stmt(interp, stmt->as.if_stmt.then_branch, env, labels);
            }
            value_free(cond);

            for (size_t i = 0; i < stmt->as.if_stmt.elif_conditions.count; i++) {
                Value elif_cond = eval_expr(interp, stmt->as.if_stmt.elif_conditions.items[i], env);
                if (interp->error) {
                    ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                    clear_error(interp);
                    return err;
                }

                if (value_truthiness(elif_cond)) {
                    value_free(elif_cond);
                    return exec_stmt(interp, stmt->as.if_stmt.elif_blocks.items[i], env, labels);
                }
                value_free(elif_cond);
            }

            if (stmt->as.if_stmt.else_branch) {
                return exec_stmt(interp, stmt->as.if_stmt.else_branch, env, labels);
            }

            return make_ok(value_null());
        }

        case STMT_WHILE: {
            interp->loop_depth++;
            int iteration_count = 0;
            const int max_iterations = 100000; // Prevent infinite loops

            while (1) {
                if (++iteration_count > max_iterations) {
                    interp->loop_depth--;
                    return make_error("Infinite loop detected", stmt->line, stmt->column);
                }

                Value cond = eval_expr(interp, stmt->as.while_stmt.condition, env);
                if (interp->error) {
                    interp->loop_depth--;
                    ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                    clear_error(interp);
                    return err;
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
            }

            interp->loop_depth--;
            return make_ok(value_null());
        }

        case STMT_FOR: {
            interp->loop_depth++;
            int iteration_count = 0;
            const int max_iterations = 100000; // Prevent infinite loops

            Value target = eval_expr(interp, stmt->as.for_stmt.target, env);
            if (interp->error) {
                interp->loop_depth--;
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }

            if (target.type != VAL_INT) {
                value_free(target);
                interp->loop_depth--;
                return make_error("FOR target must be INT", stmt->line, stmt->column);
            }

            int64_t limit = target.as.i;
            value_free(target);

            for (int64_t idx = 0; idx < limit; idx++) {
                if (++iteration_count > max_iterations) {
                    interp->loop_depth--;
                    return make_error("Infinite loop detected", stmt->line, stmt->column);
                }

                // Bind or assign the loop counter in the current environment
                if (!env_assign(env, stmt->as.for_stmt.counter, value_int(idx), TYPE_INT, true)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", stmt->as.for_stmt.counter);
                    return make_error(buf, stmt->line, stmt->column);
                }

                ExecResult res = exec_stmt(interp, stmt->as.for_stmt.body, env, labels);

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

                // EXEC_CONTINUE is treated as a normal completion (loop continues)
            }

            interp->loop_depth--;
            return make_ok(value_null());
        }

        default:
            return make_ok(value_null());
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

ExecResult exec_program(Stmt* program, const char* source_path) {
    Interpreter interp = {0};
    interp.global_env = env_create(NULL);
    interp.functions = func_table_create();
    interp.loop_depth = 0;
    interp.error = NULL;
    interp.in_try_block = false;
    interp.modules = NULL;

    builtins_init();
    // Record source path of the primary program so imports and MAIN() work
    if (source_path && source_path[0] != '\0') {
        env_assign(interp.global_env, "__MODULE_SOURCE__", value_str(source_path), TYPE_STR, true);
    }
    
    LabelMap labels = {0};
    ExecResult res = exec_stmt_list(&interp, &program->as.block, interp.global_env, &labels);
    
    // Clean up
    for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
    free(labels.items);
    
    env_free(interp.global_env);
    func_table_free(interp.functions);
    // Free modules
    ModuleEntry* me = interp.modules;
    while (me) {
        ModuleEntry* next = me->next;
        free(me->name);
        env_free(me->env);
        free(me);
        me = next;
    }
    
    return res;
}

// Execute a parsed program within an existing Interpreter and Env.
// This runs `program` using the provided `interp` state and the
// supplied `env` as the execution environment. It returns an
// ExecResult similar to `exec_program`.
ExecResult exec_program_in_env(Interpreter* interp, Stmt* program, Env* env) {
    if (!interp || !program || !env) {
        ExecResult r = make_error("Internal: invalid args to exec_program_in_env", 0, 0);
        return r;
    }

    LabelMap labels = {0};
    ExecResult res = exec_stmt_list(interp, &program->as.block, env, &labels);

    for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
    free(labels.items);

    return res;
}
