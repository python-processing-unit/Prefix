#include "interpreter.h"
#include "builtins.h"
#include "ns_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

// Forward declarations
static ExecResult exec_stmt(Interpreter* interp, Stmt* stmt, Env* env, LabelMap* labels);
static ExecResult exec_stmt_list(Interpreter* interp, StmtList* list, Env* env, LabelMap* labels);

static void wait_if_paused(Interpreter* interp) {
    if (!interp || !interp->current_thr) return;
    Thr* th = interp->current_thr;
    if (!th) return;
    while (th->paused && !th->finished) {
        thrd_yield();
    }
}

static mtx_t g_tns_lock;
static mtx_t g_parfor_lock;
// Thread worker for THR blocks
typedef struct {
    Interpreter* interp;
    Env* env;
    Stmt* body;
    Value thr_val;
} ThrStart;

static int thr_worker(void* arg) {
    ThrStart* start = (ThrStart*)arg;
    LabelMap labels = {0};
    start->interp->current_thr = start->thr_val.as.thr;
    ExecResult res = exec_stmt(start->interp, start->body, start->env, &labels);

    // Clean up labels
    for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
    free(labels.items);

    if (res.status == EXEC_RETURN || res.status == EXEC_OK || res.status == EXEC_GOTO) {
        value_free(res.value);
    }
    if (res.status == EXEC_ERROR && res.error) {
        free(res.error);
    }

    value_thr_set_finished(start->thr_val, 1);
    value_free(start->thr_val);
    /* `start->env` points at the caller's environment (shared); the
     * worker must not free it. Ownership remains with the parent
     * interpreter which will free the env when appropriate.
     */
    free(start->interp);
    free(start);
    return 0;
}

typedef struct {
    Interpreter* interp;
    Env* env;
    Stmt* body;
    char** errors; // shared array, one slot per iteration
    int index; // iteration index
    Value thr_val;
    int* err_lines;
    int* err_cols;
} ParforStart;

static int parfor_worker(void* arg) {
    ParforStart* start = (ParforStart*)arg;
    LabelMap labels = {0};
    start->interp->current_thr = start->thr_val.as.thr;
    mtx_lock(&g_parfor_lock);
    ExecResult res = exec_stmt(start->interp, start->body, start->env, &labels);
    mtx_unlock(&g_parfor_lock);

    for (size_t i = 0; i < labels.count; i++) value_free(labels.items[i].key);
    free(labels.items);

    if (res.status == EXEC_ERROR && res.error) {
        // Transfer ownership of the error string into shared array
        start->errors[start->index] = res.error;
        /* record original error location so parent can report/handle it */
        if (start->err_lines) start->err_lines[start->index] = res.error_line;
        if (start->err_cols) start->err_cols[start->index] = res.error_column;
    } else {
        if (res.status == EXEC_RETURN || res.status == EXEC_OK || res.status == EXEC_GOTO) {
            value_free(res.value);
        }
        if (res.status == EXEC_ERROR && res.error) free(res.error);
    }

    value_thr_set_finished(start->thr_val, 1);
    /* Null out env on the Thr handle before freeing so the handle
       (which may still be referenced until the join completes) does
       not carry a dangling pointer. */
    if (start->thr_val.as.thr) start->thr_val.as.thr->env = NULL;
    value_free(start->thr_val);
    env_free(start->env);
    free(start->interp);
    free(start);
    return 0;
}

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

static int builtin_param_index(BuiltinFunction* builtin, const char* kw) {
    if (!builtin || !kw) return -1;
    if (!builtin->param_names || builtin->param_count <= 0) return -1;
    for (int i = 0; i < builtin->param_count; i++) {
        const char* pn = builtin->param_names[i];
        if (pn && strcmp(pn, kw) == 0) return i;
    }
    return -1;
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

// ============ Module registry ============

typedef struct ModuleEntry {
    char* name;
    Env* env;
    int owns_env;
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
    me->owns_env = 1;
    me->next = interp->modules;
    interp->modules = me;
    return 0;
}

int module_register_alias(Interpreter* interp, const char* name, Env* env) {
    if (!interp || !name || !env) return -1;

    ModuleEntry* e = interp->modules;
    while (e) {
        if (strcmp(e->name, name) == 0) {
            return e->env == env ? 0 : -1;
        }
        e = e->next;
    }

    ModuleEntry* me = safe_malloc(sizeof(ModuleEntry));
    me->name = strdup(name);
    me->env = env;
    me->owns_env = 0;
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
        case VAL_THR:
            return value_thr_is_running(v);
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
        case VAL_THR: return TYPE_THR;
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
        case TYPE_THR: return VAL_THR;
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

        case EXPR_PTR: {
            // Evaluate pointer literal by returning the pointed-to binding's value (dereference-on-read)
            const char* name = expr->as.ptr_name;
            Value v; DeclType dt; bool initialized;
            if (!env_get(env, name, &v, &dt, &initialized)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Undefined identifier '%s'", name);
                interp->error = strdup(buf);
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                return value_null();
            }
            if (!initialized) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Identifier '%s' declared but not initialized", name);
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
                    int pos_argc = (int)expr->as.call.args.count;
                    int kwc = (int)expr->as.call.kw_count;
                    Value* args = NULL;
                    Expr** arg_nodes = NULL;

                    // For builtins, keywords are supported only if the builtin declares param names.
                    if (kwc > 0 && (!builtin->param_names || builtin->param_count <= 0)) {
                        interp->error = strdup("Keyword arguments not supported for builtin function");
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        return value_null();
                    }

                    // Reject duplicate keyword names (order-independent)
                    if (kwc > 0) {
                        for (int k = 0; k < kwc; k++) {
                            for (int m = 0; m < k; m++) {
                                if (strcmp(expr->as.call.kw_names[m], expr->as.call.kw_names[k]) == 0) {
                                    interp->error = strdup("Duplicate keyword argument");
                                    interp->error_line = expr->line;
                                    interp->error_col = expr->column;
                                    return value_null();
                                }
                            }
                        }
                    }

                    // Determine required slots for args (positional plus any kw slot indices)
                    int max_slot = pos_argc;
                    if (kwc > 0) {
                        for (int i = 0; i < kwc; i++) {
                            char* k = expr->as.call.kw_names[i];
                            int idx = builtin_param_index(builtin, k);
                            if (idx < 0) {
                                interp->error = strdup("Unknown keyword argument");
                                interp->error_line = expr->line;
                                interp->error_col = expr->column;
                                return value_null();
                            }
                            if (idx + 1 > max_slot) max_slot = idx + 1;
                        }
                    }

                    if (max_slot > 0) {
                        args = safe_malloc(sizeof(Value) * max_slot);
                        arg_nodes = safe_malloc(sizeof(Expr*) * max_slot);
                        // initialize to nulls
                        for (int i = 0; i < max_slot; i++) { args[i] = value_null(); arg_nodes[i] = NULL; }

                        // Evaluate positional args
                        for (int i = 0; i < pos_argc; i++) {
                            arg_nodes[i] = expr->as.call.args.items[i];
                            if (((strcmp(func_name, "DEL") == 0 || strcmp(func_name, "EXIST") == 0 || strcmp(func_name, "IMPORT") == 0 || strcmp(func_name, "ASSIGN") == 0) && i == 0)
                                || ((strcmp(func_name, "IMPORT") == 0 || strcmp(func_name, "IMPORT_PATH") == 0) && i == 1)) {
                                // leave as null placeholder
                                continue;
                            }
                            args[i] = eval_expr(interp, expr->as.call.args.items[i], env);
                            if (interp->error) {
                                for (int j = 0; j <= i; j++) value_free(args[j]);
                                free(args);
                                free(arg_nodes);
                                return value_null();
                            }
                        }

                        // Evaluate keyword args and place into appropriate slots
                        for (int k = 0; k < kwc; k++) {
                            char* name = expr->as.call.kw_names[k];
                            Expr* valnode = expr->as.call.kw_args.items[k];
                            int idx = builtin_param_index(builtin, name);
                            if (idx < 0) {
                                interp->error = strdup("Unknown keyword argument");
                                interp->error_line = expr->line;
                                interp->error_col = expr->column;
                                for (int j = 0; j < max_slot; j++) value_free(args[j]);
                                free(args);
                                free(arg_nodes);
                                return value_null();
                            }
                            // Duplicate positional/keyword or duplicate keyword->slot
                            if (idx < max_slot && arg_nodes[idx] != NULL) {
                                interp->error = strdup("Duplicate argument for parameter");
                                interp->error_line = expr->line;
                                interp->error_col = expr->column;
                                for (int j = 0; j < max_slot; j++) value_free(args[j]);
                                free(args);
                                free(arg_nodes);
                                return value_null();
                            }
                            // Evaluate kw expr in caller env (left-to-right preserved)
                            Value v = eval_expr(interp, valnode, env);
                            if (interp->error) {
                                for (int j = 0; j < max_slot; j++) value_free(args[j]);
                                free(args);
                                free(arg_nodes);
                                return value_null();
                            }
                            // assign into slot
                            if (idx >= max_slot) {
                                // should not happen
                                for (int j = 0; j < max_slot; j++) value_free(args[j]);
                                value_free(v);
                                free(args);
                                free(arg_nodes);
                                interp->error = strdup("Internal error mapping keyword arg");
                                interp->error_line = expr->line;
                                interp->error_col = expr->column;
                                return value_null();
                            }
                            // free placeholder null
                            value_free(args[idx]);
                            args[idx] = v;
                            arg_nodes[idx] = valnode;
                        }
                    }

                    // effective_argc should count the original positional arguments
                    // and extend if any keyword maps beyond them. Do NOT trim placeholder
                    // NULLs for intentionally-unevaluated positional args (e.g. DEL).
                    int effective_argc = pos_argc;
                    if (max_slot > effective_argc) effective_argc = max_slot;

                    // Check arg count against builtin limits
                    if (effective_argc < builtin->min_args) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s expects at least %d arguments", func_name, builtin->min_args);
                        interp->error = strdup(buf);
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        if (args) {
                            for (int i = 0; i < max_slot; i++) value_free(args[i]);
                            free(args);
                            free(arg_nodes);
                        }
                        return value_null();
                    }
                    if (builtin->max_args >= 0 && effective_argc > builtin->max_args) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%s expects at most %d arguments", func_name, builtin->max_args);
                        interp->error = strdup(buf);
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        if (args) {
                            for (int i = 0; i < max_slot; i++) value_free(args[i]);
                            free(args);
                            free(arg_nodes);
                        }
                        return value_null();
                    }

                    // Call builtin
                    Value result = builtin->impl(interp, args, effective_argc, arg_nodes, env, expr->line, expr->column);

                    // Clean up
                    if (args) {
                        for (int i = 0; i < max_slot; i++) value_free(args[i]);
                        free(args);
                        free(arg_nodes);
                    }

                    return result;
                }
                
                // Check user-defined functions in the shared namespace
                Value v;
                DeclType dtype;
                bool initialized;
                if (env_get(env, func_name, &v, &dtype, &initialized)) {
                    if (initialized && v.type == VAL_FUNC) {
                        user_func = v.as.func;
                    }
                    value_free(v);
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
            int pos_argc = (int)expr->as.call.args.count;
            int kwc = (int)expr->as.call.kw_count;

            // Evaluate positional arguments first (left-to-right)
            Value* pos_vals = NULL;
            if (pos_argc > 0) {
                pos_vals = safe_malloc(sizeof(Value) * pos_argc);
                for (int i = 0; i < pos_argc; i++) {
                    pos_vals[i] = eval_expr(interp, expr->as.call.args.items[i], env);
                    if (interp->error) {
                        for (int j = 0; j <= i; j++) value_free(pos_vals[j]);
                        free(pos_vals);
                        return value_null();
                    }
                }
            }

            // Evaluate keyword argument expressions in source order
            Value* kw_vals = NULL;
            int* kw_used = NULL;
            if (kwc > 0) {
                kw_vals = safe_malloc(sizeof(Value) * kwc);
                kw_used = calloc(kwc, sizeof(int));
                for (int k = 0; k < kwc; k++) {
                    // detect duplicate keyword names in source (runtime error)
                    for (int m = 0; m < k; m++) {
                        if (strcmp(expr->as.call.kw_names[m], expr->as.call.kw_names[k]) == 0) {
                            interp->error = strdup("Duplicate keyword argument");
                            interp->error_line = expr->line;
                            interp->error_col = expr->column;
                            for (int t = 0; t < (pos_argc); t++) value_free(pos_vals[t]);
                            free(pos_vals);
                            for (int t = 0; t < k; t++) value_free(kw_vals[t]);
                            free(kw_vals);
                            return value_null();
                        }
                    }
                    kw_vals[k] = eval_expr(interp, expr->as.call.kw_args.items[k], env);
                    if (interp->error) {
                        for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                        free(pos_vals);
                        for (int t = 0; t < k; t++) value_free(kw_vals[t]);
                        free(kw_vals);
                        return value_null();
                    }
                }
            }

            // Count positional-only parameters (those without a default value).
            // Per spec: "A parameter without a default is positional; a parameter
            // with a default is keyword-capable." Positional arguments may only
            // bind to positional parameters.
            int num_pos_params = 0;
            for (size_t pi = 0; pi < user_func->params.count; pi++) {
                if (!user_func->params.items[pi].default_value) num_pos_params++;
                else break; // positional params must precede keyword-capable ones
            }

            if (pos_argc > num_pos_params) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Too many positional arguments for '%s'",
                         user_func->name ? user_func->name : "<lambda>");
                interp->error = strdup(buf);
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                free(pos_vals);
                for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                free(kw_vals);
                if (kw_used) free(kw_used);
                return value_null();
            }

            // Create new environment for function call
            Env* call_env = env_create(user_func->closure);

            // Bind parameters in order, evaluating defaults in call_env after earlier params are bound
            for (size_t i = 0; i < user_func->params.count; i++) {
                Param* param = &user_func->params.items[i];
                Value arg_val = value_null();

                bool provided = false;
                // positional provided?
                if ((int)i < pos_argc) {
                    arg_val = pos_vals[i];
                    provided = true;
                    // check if a keyword also provided for same name -> error
                    for (int k = 0; k < kwc; k++) {
                        if (strcmp(expr->as.call.kw_names[k], param->name) == 0) {
                            interp->error = strdup("Duplicate argument for parameter");
                            interp->error_line = expr->line;
                            interp->error_col = expr->column;
                            // cleanup
                            for (int t = 0; t < pos_argc; t++) if (t != i) value_free(pos_vals[t]);
                            free(pos_vals);
                            for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                            free(kw_vals);
                            env_free(call_env);
                            return value_null();
                        }
                    }
                } else {
                    // check if provided as keyword
                    int found_kw = -1;
                    for (int k = 0; k < kwc; k++) {
                        if (strcmp(expr->as.call.kw_names[k], param->name) == 0) { found_kw = k; break; }
                    }
                    if (found_kw >= 0) {
                        // parameter must declare a default to be keyword-capable
                        if (!param->default_value) {
                            interp->error = strdup("Parameter is not keyword-capable");
                            interp->error_line = expr->line;
                            interp->error_col = expr->column;
                            for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                            free(pos_vals);
                            for (int t = 0; t < kwc; t++) if (t != found_kw) value_free(kw_vals[t]);
                            free(kw_vals);
                            free(kw_used);
                            env_free(call_env);
                            return value_null();
                        }
                        arg_val = kw_vals[found_kw];
                        kw_used[found_kw] = 1;
                        provided = true;
                    } else if (param->default_value) {
                        // evaluate default in call_env (after earlier params bound)
                        arg_val = eval_expr(interp, param->default_value, call_env);
                        if (interp->error) {
                            for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                            free(pos_vals);
                            for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                            free(kw_vals);
                            env_free(call_env);
                            return value_null();
                        }
                        provided = true;
                    }
                }

                if (!provided) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Missing argument for parameter '%s'", param->name);
                    interp->error = strdup(buf);
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                    free(pos_vals);
                    for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                    free(kw_vals);
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
                    // free val resources
                    if ((int)i < pos_argc) {
                        // pos_vals[i] will be freed below when cleaning pos_vals array
                    } else {
                        // arg_val came from kw_vals or default; if from kw_vals we will free kw_vals array later
                    }
                    for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                    free(pos_vals);
                    for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                    free(kw_vals);
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
                    for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                    free(pos_vals);
                    for (int t = 0; t < kwc; t++) value_free(kw_vals[t]);
                    free(kw_vals);
                    env_free(call_env);
                    return value_null();
                }
                // Free the temporary argument value now that it's been copied into the callee env
                value_free(arg_val);
            }

            // Check for any unmatched keyword args
            if (kwc > 0) {
                for (int k = 0; k < kwc; k++) {
                    if (!kw_used[k]) {
                        interp->error = strdup("Unknown keyword argument");
                        interp->error_line = expr->line;
                        interp->error_col = expr->column;
                        // cleanup
                        for (int t = 0; t < pos_argc; t++) value_free(pos_vals[t]);
                        free(pos_vals);
                        for (int t = 0; t < kwc; t++) if (!kw_used[t]) value_free(kw_vals[t]);
                        free(kw_vals);
                        free(kw_used);
                        env_free(call_env);
                        return value_null();
                    }
                }
            }

            // free temporary evaluated argument arrays (their Value contents are now copied into env or freed by env_assign)
            if (pos_vals) free(pos_vals);
            if (kw_vals) free(kw_vals);
            if (kw_used) free(kw_used);
            
            // Execute function body
            LabelMap local_labels = {0};
            ExecResult res = exec_stmt(interp, user_func->body, call_env, &local_labels);
            
            // Clean up labels
            for (size_t i = 0; i < local_labels.count; i++) value_free(local_labels.items[i].key);
            free(local_labels.items);
            
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
                case TYPE_TNS:
                    interp->error = strdup("TNS-returning function must return a value");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    return value_null();
                case TYPE_FUNC:
                    interp->error = strdup("FUNC-returning function must return a value");
                    interp->error_line = expr->line;
                    interp->error_col = expr->column;
                    return value_null();
                case TYPE_THR:
                    interp->error = strdup("THR-returning function must return a value");
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
                    elem_decl = TYPE_UNKNOWN;
                    break;
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
        case EXPR_ASYNC: {
            // Expression form: start executing block asynchronously and return THR handle
            Value thr_val = value_thr_new();
            Value thr_for_worker = value_copy(thr_val);

            ThrStart* start = safe_malloc(sizeof(ThrStart));
            Interpreter* thr_interp = safe_malloc(sizeof(Interpreter));
            *thr_interp = (Interpreter){0};
            thr_interp->global_env = interp->global_env;
            thr_interp->loop_depth = 0;
            thr_interp->error = NULL;
            thr_interp->error_line = 0;
            thr_interp->error_col = 0;
            thr_interp->in_try_block = false;
            thr_interp->modules = interp->modules;
            thr_interp->shushed = interp->shushed;

            start->interp = thr_interp;
            start->env = env;
            start->body = expr->as.async.block;
            start->thr_val = thr_for_worker;

            /* record body/env on the Thr so restart is possible */
            thr_for_worker.as.thr->body = start->body;
            thr_for_worker.as.thr->env = start->env;
            thr_for_worker.as.thr->started = 1;

            if (thrd_create(&thr_for_worker.as.thr->thread, thr_worker, start) != thrd_success) {
                value_thr_set_finished(thr_for_worker, 1);
                value_free(thr_for_worker);
                free(thr_interp);
                free(start);
                interp->error = strdup("Failed to start ASYNC");
                interp->error_line = expr->line;
                interp->error_col = expr->column;
                value_free(thr_val);
                return value_null();
            }
            return thr_val;
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

ExecResult assign_index_chain(Interpreter* interp, Env* env, Expr* idx_expr, Value rhs, int stmt_line, int stmt_col) {
    // Collect index nodes from outermost -> innermost, and require base to be an identifier.
    size_t chain_len = 0;
    Expr* walker = idx_expr;
    while (walker && walker->type == EXPR_INDEX) {
        chain_len++;
        walker = walker->as.index.target;
    }

    if (!walker || walker->type != EXPR_IDENT) {
        return make_error("Indexed assignment base must be an identifier", stmt_line, stmt_col);
    }

    const char* base_name = walker->as.ident;
    Value base_val = value_null();
    DeclType base_type = TYPE_UNKNOWN;
    bool base_initialized = false;

    if (!env_get(env, base_name, &base_val, &base_type, &base_initialized)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot assign to undeclared identifier '%s'", base_name);
        return make_error(buf, stmt_line, stmt_col);
    }

    // If uninitialized (or NULL), default to MAP (matches previous behaviour),
    // and persist that into the environment via env_assign.
    if (!base_initialized || base_val.type == VAL_NULL) {
        Value nm = value_map_new();
        if (!env_assign(env, base_name, nm, TYPE_UNKNOWN, false)) {
            value_free(nm);
            value_free(base_val);
            return make_error("Cannot assign to identifier (frozen?)", stmt_line, stmt_col);
        }
        value_free(base_val);
        base_val = nm;
        base_initialized = true;
    }

    Expr** nodes = malloc(sizeof(Expr*) * (chain_len ? chain_len : 1));
    if (!nodes) {
        value_free(base_val);
        return make_error("Out of memory", stmt_line, stmt_col);
    }

    walker = idx_expr;
    for (size_t i = 0; i < chain_len; i++) {
        nodes[i] = walker;
        walker = walker->as.index.target;
    }

    ExecResult out;
    Value* cur = &base_val;

    // Process from innermost -> outermost
    for (int ni = (int)chain_len - 1; ni >= 0; ni--) {
        Expr* node = nodes[ni];
        ExprList* indices = &node->as.index.indices;
        if (indices->count == 0) {
            out = make_error("Empty index list", node->line, node->column);
            goto cleanup;
        }

        // Auto-promote NULL to MAP when assigning through indexes.
        if (cur->type == VAL_NULL) {
            *cur = value_map_new();
        }

        if (cur->type == VAL_TNS) {
            Tensor* t = cur->as.tns;
            // Lvalue assignment only supports full integer indexing (no ranges/wildcards).
            if (indices->count != t->ndim) {
                out = make_error("Cannot assign through tensor slice", node->line, node->column);
                goto cleanup;
            }

            size_t* idxs0 = malloc(sizeof(size_t) * indices->count);
            if (!idxs0) {
                out = make_error("Out of memory", stmt_line, stmt_col);
                goto cleanup;
            }

            for (size_t i = 0; i < indices->count; i++) {
                Expr* it = indices->items[i];
                if (it->type == EXPR_WILDCARD || it->type == EXPR_RANGE) {
                    free(idxs0);
                    out = make_error("Cannot assign using ranges or wildcards", it->line, it->column);
                    goto cleanup;
                }

                Value vi = eval_expr(interp, it, env);
                if (interp->error) {
                    ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                    clear_error(interp);
                    free(idxs0);
                    out = err;
                    goto cleanup;
                }
                if (vi.type != VAL_INT) {
                    value_free(vi);
                    free(idxs0);
                    out = make_error("Index expression must evaluate to INT", it->line, it->column);
                    goto cleanup;
                }

                int64_t v = vi.as.i;
                int64_t dim = (int64_t)t->shape[i];
                if (v < 0) v = dim + v + 1;
                if (v < 1 || v > dim) {
                    value_free(vi);
                    free(idxs0);
                    out = make_error("Index out of range", it->line, it->column);
                    goto cleanup;
                }
                idxs0[i] = (size_t)(v - 1);
                value_free(vi);
            }

            Value* elem = value_tns_get_ptr(*cur, idxs0, indices->count);
            free(idxs0);
            if (!elem) {
                out = make_error("Index out of range", node->line, node->column);
                goto cleanup;
            }
            cur = elem;
            continue;
        }

        if (cur->type == VAL_MAP) {
            for (size_t i = 0; i < indices->count; i++) {
                Expr* it = indices->items[i];
                Value key = eval_expr(interp, it, env);
                if (interp->error) {
                    ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                    clear_error(interp);
                    out = err;
                    goto cleanup;
                }
                if (!(key.type == VAL_INT || key.type == VAL_STR || key.type == VAL_FLT)) {
                    value_free(key);
                    out = make_error("Map index must be INT, FLT or STR", it->line, it->column);
                    goto cleanup;
                }

                bool last_key_in_node = (i + 1 == indices->count);
                bool last_node_in_chain = (ni == 0);

                if (last_node_in_chain && last_key_in_node) {
                    // Final destination: assign rhs here.
                    Value* slot = value_map_get_ptr(cur, key, true);
                    value_free(key);
                    if (!slot) {
                        out = make_error("Internal error assigning to map", stmt_line, stmt_col);
                        goto cleanup;
                    }
                    if (slot->type != VAL_NULL && value_type_to_decl(slot->type) != value_type_to_decl(rhs.type)) {
                        out = make_error("Map entry type mismatch", stmt_line, stmt_col);
                        goto cleanup;
                    }
                    value_free(*slot);
                    *slot = value_copy(rhs);
                    out = make_ok(value_null());
                    goto cleanup;
                }

                // Descend into slot.
                Value* slot = value_map_get_ptr(cur, key, true);
                value_free(key);
                if (!slot) {
                    out = make_error("Internal error indexing map", stmt_line, stmt_col);
                    goto cleanup;
                }

                if (slot->type == VAL_NULL) {
                    *slot = value_map_new();
                }

                cur = slot;
            }

            continue;
        }

        // Unsupported type for indexed assignment
        out = make_error("Indexing assignment is supported only on tensors and maps", node->line, node->column);
        goto cleanup;
    }

    // If we get here, the chain ended after resolving to a tensor element (e.g. a<1> = rhs)
    if (cur->type != VAL_NULL && value_type_to_decl(cur->type) != value_type_to_decl(rhs.type)) {
        out = make_error("Element type mismatch", stmt_line, stmt_col);
        goto cleanup;
    }
    mtx_lock(&g_tns_lock);
    value_free(*cur);
    *cur = value_copy(rhs);
    mtx_unlock(&g_tns_lock);

    out = make_ok(value_null());

cleanup:
    free(nodes);
    value_free(base_val);
    return out;
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
            EnvEntry* existing = env_get_entry(env, stmt->as.decl.name);
            if (!existing) {
                Env* decl_env = env->parent ? env->parent : env;
                env_define(decl_env, stmt->as.decl.name, stmt->as.decl.decl_type);
            }
            return make_ok(value_null());
        }

        case STMT_ASSIGN: {
            // Special-case: RHS is a pointer literal -> create alias binding on LHS (no PTR Value type)
            if (stmt->as.assign.value && stmt->as.assign.value->type == EXPR_PTR && stmt->as.assign.target == NULL) {
                const char* tgt = stmt->as.assign.value->as.ptr_name;
                if (!tgt) return make_error("Invalid pointer literal", stmt->line, stmt->column);
                if (stmt->as.assign.has_type) {
                    DeclType expected = stmt->as.assign.decl_type;
                    if (!env_set_alias(env, stmt->as.assign.name, tgt, expected, true)) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Cannot create alias '%s' -> '%s'", stmt->as.assign.name, tgt);
                        return make_error(buf, stmt->line, stmt->column);
                    }
                } else {
                    if (!env_set_alias(env, stmt->as.assign.name, tgt, TYPE_UNKNOWN, false)) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Cannot create alias '%s' -> '%s'", stmt->as.assign.name, tgt);
                        return make_error(buf, stmt->line, stmt->column);
                    }
                }
                return make_ok(value_null());
            }

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

                ExecResult ar = assign_index_chain(interp, env, stmt->as.assign.target, v, stmt->line, stmt->column);
                if (ar.status == EXEC_ERROR) {
                    value_free(v);
                    return ar;
                }

                value_free(v);
                return make_ok(value_null());
            }

            if (stmt->as.assign.has_type) {
                DeclType expected = stmt->as.assign.decl_type;
                DeclType actual = value_type_to_decl(v.type);

                if (expected != actual) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Type mismatch: expected %s but got %s",
                             expected == TYPE_INT ? "INT" :
                             expected == TYPE_FLT ? "FLT" :
                             expected == TYPE_STR ? "STR" :
                             expected == TYPE_TNS ? "TNS" :
                             expected == TYPE_FUNC ? "FUNC" :
                             expected == TYPE_THR ? "THR" : "UNKNOWN",
                             value_type_name(v));
                    value_free(v);
                    return make_error(buf, stmt->line, stmt->column);
                }

                EnvEntry* existing = env_get_entry(env, stmt->as.assign.name);
                Env* assign_env = env;
                if (!existing && env->parent) {
                    assign_env = env->parent;
                }
                if (!existing) {
                    env_define(assign_env, stmt->as.assign.name, expected);
                }
                if (!env_assign(assign_env, stmt->as.assign.name, v, expected, true)) {
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

            if (builtin_lookup(f->name)) {
                free(f->name);
                for (size_t i = 0; i < f->params.count; i++) free(f->params.items[i].name);
                free(f->params.items);
                free(f);
                return make_error("Function name conflicts with built-in", stmt->line, stmt->column);
            }

            EnvEntry* prior = env_get_entry(env, f->name);
            if (prior && prior->decl_type != TYPE_FUNC) {
                free(f->name);
                for (size_t i = 0; i < f->params.count; i++) free(f->params.items[i].name);
                free(f->params.items);
                free(f);
                return make_error("Function name conflicts with existing symbol", stmt->line, stmt->column);
            }

            // Also expose the function as a binding in the current environment
            // so that builtins which operate on identifiers (DEL, EXIST, etc.)
            // can find and manipulate the function by name.
            Value fv = value_func(f);
            EnvEntry* existing = env_get_entry(env, f->name);
            Env* bind_env = env;
            if (!existing && env->parent) {
                bind_env = env->parent;
            }
            if (!env_assign(bind_env, f->name, fv, TYPE_FUNC, true)) {
                // If we cannot assign the function into the environment, treat as error
                return make_error("Failed to bind function name in environment", stmt->line, stmt->column);
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

        case STMT_THR: {
            Value thr_val = value_thr_new();
            Value thr_for_worker = value_copy(thr_val);
            if (!env_assign(env, stmt->as.thr_stmt.name, thr_val, TYPE_THR, true)) {
                value_free(thr_for_worker);
                value_free(thr_val);
                return make_error("Cannot assign to THR identifier", stmt->line, stmt->column);
            }
            value_free(thr_val);

            ThrStart* start = safe_malloc(sizeof(ThrStart));
            Interpreter* thr_interp = safe_malloc(sizeof(Interpreter));
            *thr_interp = (Interpreter){0};
            thr_interp->global_env = interp->global_env;
            thr_interp->loop_depth = 0;
            thr_interp->error = NULL;
            thr_interp->error_line = 0;
            thr_interp->error_col = 0;
            thr_interp->in_try_block = false;
            thr_interp->modules = interp->modules;
            thr_interp->shushed = interp->shushed;

            start->interp = thr_interp;
            start->env = env;
            start->body = stmt->as.thr_stmt.body;
            start->thr_val = thr_for_worker;

            /* record body/env on the Thr so restart is possible */
            thr_for_worker.as.thr->body = start->body;
            thr_for_worker.as.thr->env = start->env;
            thr_for_worker.as.thr->started = 1;

            if (thrd_create(&thr_for_worker.as.thr->thread, thr_worker, start) != thrd_success) {
                value_thr_set_finished(thr_for_worker, 1);
                value_free(thr_for_worker);
                free(thr_interp);
                free(start);
                return make_error("Failed to start THR", stmt->line, stmt->column);
            }
            return make_ok(value_null());
        }

        case STMT_ASYNC: {
            Value thr_val = value_thr_new();
            Value thr_for_worker = value_copy(thr_val);

            ThrStart* start = safe_malloc(sizeof(ThrStart));
            Interpreter* thr_interp = safe_malloc(sizeof(Interpreter));
            *thr_interp = (Interpreter){0};
            thr_interp->global_env = interp->global_env;
            thr_interp->loop_depth = 0;
            thr_interp->error = NULL;
            thr_interp->error_line = 0;
            thr_interp->error_col = 0;
            thr_interp->in_try_block = false;
            thr_interp->modules = interp->modules;
            thr_interp->shushed = interp->shushed;

            start->interp = thr_interp;
            start->env = env;
            start->body = stmt->as.async_stmt.body;
            start->thr_val = thr_for_worker;

            /* record body/env on the Thr so restart is possible */
            thr_for_worker.as.thr->body = start->body;
            thr_for_worker.as.thr->env = start->env;
            thr_for_worker.as.thr->started = 1;

            if (thrd_create(&thr_for_worker.as.thr->thread, thr_worker, start) != thrd_success) {
                value_thr_set_finished(thr_for_worker, 1);
                value_free(thr_for_worker);
                free(thr_interp);
                free(start);
                return make_error("Failed to start ASYNC", stmt->line, stmt->column);
            }
            return make_ok(value_null());
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
            const unsigned long long max_iterations = 18446744073709551615; // Prevent infinite loops

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

            for (int64_t idx = 1; idx <= limit; idx++) {
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

        case STMT_PARFOR: {
            interp->loop_depth++;
            int iteration_count = 0;
            const int max_iterations = 100000;

            Value target = eval_expr(interp, stmt->as.parfor_stmt.target, env);
            if (interp->error) {
                interp->loop_depth--;
                ExecResult err = make_error(interp->error, interp->error_line, interp->error_col);
                clear_error(interp);
                return err;
            }

            if (target.type != VAL_INT) {
                value_free(target);
                interp->loop_depth--;
                return make_error("PARFOR target must be INT", stmt->line, stmt->column);
            }

            int64_t limit = target.as.i;
            value_free(target);

            if (limit < 0) {
                interp->loop_depth--;
                return make_error("PARFOR target must be non-negative", stmt->line, stmt->column);
            }

            // Spawn worker threads for each iteration
            size_t n = (size_t)limit;
            char** errors = calloc(n, sizeof(char*));
            int* err_lines = calloc(n, sizeof(int));
            int* err_cols = calloc(n, sizeof(int));
            Value* thr_vals = malloc(sizeof(Value) * n);
            ParforStart** starts = malloc(sizeof(ParforStart*) * n);

            for (size_t i = 0; i < n; i++) {
                if (++iteration_count > max_iterations) {
                    interp->loop_depth--;
                    // cleanup
                    for (size_t j = 0; j < i; j++) value_free(thr_vals[j]);
                    free(thr_vals);
                    free(starts);
                    for (size_t j = 0; j < n; j++) free(errors[j]);
                    free(errors);
                    free(err_lines);
                    free(err_cols);
                    return make_error("Infinite loop detected", stmt->line, stmt->column);
                }

                thr_vals[i] = value_thr_new();

                ParforStart* start = safe_malloc(sizeof(ParforStart));
                Interpreter* thr_interp = safe_malloc(sizeof(Interpreter));
                *thr_interp = (Interpreter){0};
                thr_interp->global_env = interp->global_env;
                thr_interp->loop_depth = 0;
                thr_interp->error = NULL;
                thr_interp->error_line = 0;
                thr_interp->error_col = 0;
                thr_interp->in_try_block = interp->in_try_block;
                thr_interp->modules = interp->modules;
                thr_interp->shushed = interp->shushed;

                start->interp = thr_interp;
                /* Create a per-iteration child env so each PARFOR iteration
                   gets its own counter binding and does not race with others. */
                Env* thread_env = env_create(env);
                int64_t idx = (int64_t)i + 1; /* iterations are 1-based */
                /* Always define the counter locally so it shadows any
                   same-named binding in a parent env. */
                env_define(thread_env, stmt->as.parfor_stmt.counter, TYPE_INT);
                if (!env_assign(thread_env, stmt->as.parfor_stmt.counter, value_int(idx), TYPE_INT, false)) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", stmt->as.parfor_stmt.counter);
                    errors[i] = strdup(buf);
                    env_free(thread_env);
                    free(thr_interp);
                    free(start);
                    /* Mark this iteration as finished and continue launching others */
                    value_thr_set_finished(thr_vals[i], 1);
                    continue;
                }
                start->env = thread_env;
                start->body = stmt->as.parfor_stmt.body;
                start->errors = errors;
                start->err_lines = err_lines;
                start->err_cols = err_cols;
                start->index = (int)i;
                start->thr_val = value_copy(thr_vals[i]);
                starts[i] = start;

                /* record body/env on Thr so restart is possible */
                thr_vals[i].as.thr->body = start->body;
                thr_vals[i].as.thr->env = start->env; /* points to per-iteration env */

                if (thrd_create(&thr_vals[i].as.thr->thread, parfor_worker, start) != thrd_success) {
                    /* mark finished and leave thr_vals[i] intact so later cleanup is safe */
                    value_thr_set_finished(thr_vals[i], 1);
                    free(thr_interp);
                    free(start);
                    errors[i] = strdup("Failed to start PARFOR iteration");
                    /* continue launching others */
                } else {
                    /* only mark started after successful thread creation */
                    thr_vals[i].as.thr->started = 1;
                }
            }

            // Join only threads that were actually started
            for (size_t i = 0; i < n; i++) {
                if (thr_vals[i].type == VAL_THR && thr_vals[i].as.thr && thr_vals[i].as.thr->started) {
                    if (thrd_join(thr_vals[i].as.thr->thread, NULL) != thrd_success) {
                        // ignore join failures but mark finished
                    }
                }
            }

            // Collect first error if any (and its original location)
            char* first_err = NULL;
            int first_err_line = 0;
            int first_err_col = 0;
            for (size_t i = 0; i < n; i++) {
                if (errors[i]) { first_err = errors[i]; first_err_line = err_lines[i]; first_err_col = err_cols[i]; break; }
            }

            // Cleanup thr values
            for (size_t i = 0; i < n; i++) value_free(thr_vals[i]);
            free(thr_vals);
            free(starts);
            for (size_t i = 0; i < n; i++) if (errors[i] && errors[i] != first_err) free(errors[i]);
            free(errors);
            free(err_lines);
            free(err_cols);

            interp->loop_depth--;

            if (first_err) {
                /* Propagate iteration error into parent interpreter state
                 * so surrounding TRY blocks reliably observe it. */
                if (interp->error) free(interp->error);
                interp->error = strdup(first_err);
                /* Prefer the original iteration error location when available */
                interp->error_line = first_err_line ? first_err_line : stmt->line;
                interp->error_col = first_err_col ? first_err_col : stmt->column;
                ExecResult err = make_error(first_err, interp->error_line, interp->error_col);
                free(first_err);
                return err;
            }

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
        wait_if_paused(interp);
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
    interp.loop_depth = 0;
    interp.error = NULL;
    interp.in_try_block = false;
    interp.modules = NULL;
    interp.current_thr = NULL;

    builtins_init();
    mtx_init(&g_tns_lock, 0);
    mtx_init(&g_parfor_lock, 0);
    ns_buffer_init();
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
    // Free modules
    ModuleEntry* me = interp.modules;
    while (me) {
        ModuleEntry* next = me->next;
        free(me->name);
        if (me->owns_env) env_free(me->env);
        free(me);
        me = next;
    }
    
    ns_buffer_shutdown();
    mtx_destroy(&g_tns_lock);
    mtx_destroy(&g_parfor_lock);
    return res;
}

// helper used by builtins to restart threads
int interpreter_restart_thread(Interpreter* interp, Value thr_val, int line, int col) {
    (void)line; (void)col;
    if (thr_val.type != VAL_THR || !thr_val.as.thr) {
        if (interp) interp->error = strdup("RESTART expects THR argument");
        return -1;
    }
    Thr* th = thr_val.as.thr;
    if (!th->body || !th->env) {
        if (interp) interp->error = strdup("Cannot restart: no stored thread body/env");
        return -1;
    }
    if (!th->finished) {
        if (interp) interp->error = strdup("Cannot restart running thread");
        return -1;
    }
    ThrStart* start = safe_malloc(sizeof(ThrStart));
    Interpreter* thr_interp = safe_malloc(sizeof(Interpreter));
    *thr_interp = (Interpreter){0};
    thr_interp->global_env = interp->global_env;
    thr_interp->loop_depth = 0;
    thr_interp->error = NULL;
    thr_interp->error_line = 0;
    thr_interp->error_col = 0;
    thr_interp->in_try_block = interp->in_try_block;
    thr_interp->modules = interp->modules;
    thr_interp->shushed = interp->shushed;

    start->interp = thr_interp;
    start->env = th->env;
    start->body = th->body;
    start->thr_val = value_copy(thr_val);

    th->finished = 0;
    th->paused = 0;
    th->started = 1;
    if (thrd_create(&th->thread, thr_worker, start) != thrd_success) {
        th->finished = 1;
        value_free(start->thr_val);
        free(thr_interp);
        free(start);
        if (interp) interp->error = strdup("Failed to restart thread");
        return -1;
    }
    return 0;
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
