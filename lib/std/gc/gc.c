#include "../../../src/interpreter.h"
#include "../../../src/lexer.h"
#include "../../../src/parser.h"
#include "../../../src/prefix_extension.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GcSymbol {
    Interpreter *interp;
    Env *env;
    char *name;
    bool ignored;
    struct GcSymbol *next;
} GcSymbol;

typedef struct ModuleEntry {
    char *name;
    Env *env;
    int owns_env;
    struct ModuleEntry *next;
} ModuleEntry;

typedef struct GcName {
    char *name;
    struct GcName *next;
} GcName;

typedef struct GcSource {
    char *path;
    struct GcSource *next;
} GcSource;

typedef struct GcFunction {
    Stmt *stmt;
    bool scanned;
    struct GcFunction *next;
} GcFunction;

typedef struct {
    Interpreter *interp;
    const char *current_source;
    const char *active_source;
    int current_line;
    GcName *references;
    GcSource *sources;
    GcFunction *functions;
    bool failed;
} GcScan;

static GcSymbol *gc_symbols = NULL;

static void gc_scan_stmt(GcScan *scan, Stmt *stmt, bool all_lines);
static void gc_scan_expr(GcScan *scan, Expr *expr, bool all_lines);
static bool gc_file_exists(const char *path);

static int gc_stmt_last_line(Stmt *stmt);

static bool gc_name_seen(GcName *names, const char *name) {
    for (GcName *item = names; item; item = item->next) {
        if (strcmp(item->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void gc_add_reference(GcScan *scan, const char *name) {
    if (!scan || !name || name[0] == '\0' || gc_name_seen(scan->references, name)) {
        return;
    }
    GcName *item = malloc(sizeof(*item));
    if (!item) {
        scan->failed = true;
        return;
    }
    item->name = strdup(name);
    if (!item->name) {
        free(item);
        scan->failed = true;
        return;
    }
    item->next = scan->references;
    scan->references = item;
}

static GcFunction *gc_add_function(GcScan *scan, Stmt *stmt) {
    GcFunction *function = malloc(sizeof(*function));
    if (!function) {
        scan->failed = true;
        return NULL;
    }
    function->stmt = stmt;
    function->scanned = false;
    function->next = scan->functions;
    scan->functions = function;
    return function;
}

static bool gc_source_seen(GcSource *sources, const char *path) {
    for (GcSource *item = sources; item; item = item->next) {
        if (strcmp(item->path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool gc_mark_source(GcScan *scan, const char *path) {
    if (!scan || !path || path[0] == '\0' || gc_source_seen(scan->sources, path)) {
        return false;
    }
    GcSource *item = malloc(sizeof(*item));
    if (!item) {
        scan->failed = true;
        return false;
    }
    item->path = strdup(path);
    if (!item->path) {
        free(item);
        scan->failed = true;
        return false;
    }
    item->next = scan->sources;
    scan->sources = item;
    return true;
}

static void gc_free_scan(GcScan *scan) {
    if (!scan) {
        return;
    }
    while (scan->references) {
        GcName *next = scan->references->next;
        free(scan->references->name);
        free(scan->references);
        scan->references = next;
    }
    while (scan->sources) {
        GcSource *next = scan->sources->next;
        free(scan->sources->path);
        free(scan->sources);
        scan->sources = next;
    }
    while (scan->functions) {
        GcFunction *next = scan->functions->next;
        free(scan->functions);
        scan->functions = next;
    }
}

static char *gc_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *source = malloc((size_t)length + 1);
    if (!source) {
        fclose(file);
        return NULL;
    }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        free(source);
        fclose(file);
        return NULL;
    }
    source[length] = '\0';
    fclose(file);
    return source;
}

static char *gc_path_dir(const char *path) {
    if (!path) {
        return NULL;
    }
    const char *last = NULL;
    for (const char *cursor = path; *cursor; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            last = cursor;
        }
    }
    if (!last) {
        return strdup(".");
    }
    size_t length = (size_t)(last - path);
    if (length == 0) {
        length = 1;
    }
    char *dir = malloc(length + 1);
    if (!dir) {
        return NULL;
    }
    memcpy(dir, path, length);
    dir[length] = '\0';
    return dir;
}

static char *gc_module_base(const char *module) {
    if (!module || module[0] == '\0') {
        return NULL;
    }
    size_t length = strlen(module);
    char *base = malloc(length + 1);
    if (!base) {
        return NULL;
    }
    size_t output = 0;
    for (size_t i = 0; i < length; i++) {
        if (module[i] == '.' && i + 1 < length && module[i + 1] == '.') {
#ifdef _WIN32
            base[output++] = '\\';
#else
            base[output++] = '/';
#endif
            i++;
        } else {
            base[output++] = module[i];
        }
    }
    base[output] = '\0';
    return base;
}

static char *gc_join_path(const char *left, const char *right) {
    if (!left || !right) {
        return NULL;
    }
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left_length > 0 && left[left_length - 1] != '/' && left[left_length - 1] != '\\';
    char *path = malloc(left_length + (separator ? 1 : 0) + right_length + 1);
    if (!path) {
        return NULL;
    }
    memcpy(path, left, left_length);
    size_t offset = left_length;
    if (separator) {
#ifdef _WIN32
        path[offset++] = '\\';
#else
        path[offset++] = '/';
#endif
    }
    memcpy(path + offset, right, right_length);
    path[offset + right_length] = '\0';
    return path;
}

static char *gc_existing_path(const char *directory, const char *base) {
    char *file = gc_join_path(directory, base);
    if (!file) {
        return NULL;
    }
    size_t length = strlen(file);
    char *candidate = malloc(length + 5);
    if (!candidate) {
        free(file);
        return NULL;
    }
    snprintf(candidate, length + 5, "%s.pre", file);
    free(file);
    if (gc_file_exists(candidate)) {
        return candidate;
    }
    free(candidate);

    file = gc_join_path(directory, base);
    if (!file) {
        return NULL;
    }
    length = strlen(file);
    candidate = malloc(length + 10);
    if (!candidate) {
        free(file);
        return NULL;
    }
#ifdef _WIN32
    snprintf(candidate, length + 10, "%s\\init.pre", file);
#else
    snprintf(candidate, length + 10, "%s/init.pre", file);
#endif
    free(file);
    if (gc_file_exists(candidate)) {
        return candidate;
    }
    free(candidate);
    return NULL;
}

static char *gc_find_module_source(const char *module, const char *referer) {
    char *base = gc_module_base(module);
    if (!base) {
        return NULL;
    }
    char *referer_dir = gc_path_dir(referer);
    char *path = referer_dir ? gc_existing_path(referer_dir, base) : NULL;
    free(referer_dir);
    if (!path) {
        path = gc_existing_path("lib/std", base);
    }
    if (!path) {
        path = gc_existing_path("lib/usr", base);
    }
    if (!path) {
        path = gc_existing_path(".", base);
    }
    free(base);
    return path;
}

static bool gc_file_exists(const char *path) {
    if (!path) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

static void gc_scan_source(GcScan *scan, const char *path, bool all_lines);

static bool gc_expr_named(Expr *expr, const char *name) {
    return expr && expr->type == EXPR_IDENT && expr->as.ident && strcmp(expr->as.ident, name) == 0;
}

static void gc_scan_import(GcScan *scan, Expr *expr) {
    if (!scan || !expr || expr->type != EXPR_CALL || expr->as.call.args.count < 1 ||
        (!gc_expr_named(expr->as.call.callee, "import") && !gc_expr_named(expr->as.call.callee, "import_path") &&
         !gc_expr_named(expr->as.call.callee, "include"))) {
        return;
    }
    Expr *module = expr->as.call.args.items[0];
    if (!module || module->type != EXPR_STR || !module->as.str_value) {
        return;
    }
    char *path = NULL;
    if (gc_expr_named(expr->as.call.callee, "import_path")) {
        path = strdup(module->as.str_value);
    } else {
        path = gc_find_module_source(module->as.str_value, scan->active_source);
    }
    if (path) {
        gc_scan_source(scan, path, true);
        free(path);
    }
}

static void gc_scan_expr(GcScan *scan, Expr *expr, bool all_lines) {
    if (!scan || !expr || scan->failed || (!all_lines && expr->line <= scan->current_line)) {
        return;
    }
    switch (expr->type) {
    case EXPR_IDENT:
        gc_add_reference(scan, expr->as.ident);
        break;
    case EXPR_PTR:
        gc_add_reference(scan, expr->as.ptr_name);
        break;
    case EXPR_TYPED_IDENT:
        gc_add_reference(scan, expr->as.typed_ident.name);
        gc_scan_expr(scan, expr->as.typed_ident.schema_expr, all_lines);
        break;
    case EXPR_CALL:
        if (gc_expr_named(expr->as.call.callee, "run")) {
            return;
        }
        gc_scan_import(scan, expr);
        gc_scan_expr(scan, expr->as.call.callee, all_lines);
        for (size_t i = 0; i < expr->as.call.args.count; i++) {
            gc_scan_expr(scan, expr->as.call.args.items[i], all_lines);
        }
        for (size_t i = 0; i < expr->as.call.kw_count; i++) {
            gc_scan_expr(scan, expr->as.call.kw_args.items[i], all_lines);
        }
        break;
    case EXPR_ASYNC:
        gc_scan_stmt(scan, expr->as.async.block, all_lines);
        break;
    case EXPR_TENSOR:
        for (size_t i = 0; i < expr->as.tns_items.count; i++) {
            gc_scan_expr(scan, expr->as.tns_items.items[i], all_lines);
        }
        break;
    case EXPR_MAP:
        for (size_t i = 0; i < expr->as.map_items.keys.count; i++) {
            gc_scan_expr(scan, expr->as.map_items.keys.items[i], all_lines);
            gc_scan_expr(scan, expr->as.map_items.values.items[i], all_lines);
        }
        break;
    case EXPR_INDEX:
        gc_scan_expr(scan, expr->as.index.target, all_lines);
        for (size_t i = 0; i < expr->as.index.indices.count; i++) {
            gc_scan_expr(scan, expr->as.index.indices.items[i], all_lines);
        }
        break;
    case EXPR_RANGE:
        gc_scan_expr(scan, expr->as.range.start, all_lines);
        gc_scan_expr(scan, expr->as.range.end, all_lines);
        break;
    case EXPR_LAMBDA:
        for (size_t i = 0; i < expr->as.lambda.params.count; i++) {
            gc_scan_expr(scan, expr->as.lambda.params.items[i].default_value, all_lines);
            gc_scan_expr(scan, expr->as.lambda.params.items[i].schema_expr, all_lines);
        }
        gc_scan_expr(scan, expr->as.lambda.return_schema_expr, all_lines);
        gc_scan_stmt(scan, expr->as.lambda.body, all_lines);
        break;
    case EXPR_FMT_STR:
        for (size_t i = 0; i < expr->as.fmt_str.parts.count; i++) {
            gc_scan_expr(scan, expr->as.fmt_str.parts.items[i], all_lines);
        }
        break;
    default:
        break;
    }
}

static int gc_stmt_last_line(Stmt *stmt) {
    if (!stmt) {
        return 0;
    }
    int last = stmt->line;
    switch (stmt->type) {
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            int child_last = gc_stmt_last_line(stmt->as.block.items[i]);
            if (child_last > last) {
                last = child_last;
            }
        }
        break;
    case STMT_IF: {
        int child_last = gc_stmt_last_line(stmt->as.if_stmt.then_branch);
        if (child_last > last) {
            last = child_last;
        }
        for (size_t i = 0; i < stmt->as.if_stmt.elif_blocks.count; i++) {
            child_last = gc_stmt_last_line(stmt->as.if_stmt.elif_blocks.items[i]);
            if (child_last > last) {
                last = child_last;
            }
        }
        child_last = gc_stmt_last_line(stmt->as.if_stmt.else_branch);
        if (child_last > last) {
            last = child_last;
        }
        break;
    }
    case STMT_WHILE:
        last = gc_stmt_last_line(stmt->as.while_stmt.body);
        break;
    case STMT_FOR:
        last = gc_stmt_last_line(stmt->as.for_stmt.body);
        break;
    case STMT_PARFOR:
        last = gc_stmt_last_line(stmt->as.parfor_stmt.body);
        break;
    case STMT_FUNC:
        last = gc_stmt_last_line(stmt->as.func_stmt.body);
        break;
    case STMT_ASYNC:
        last = gc_stmt_last_line(stmt->as.async_stmt.body);
        break;
    case STMT_THREAD:
        last = gc_stmt_last_line(stmt->as.thread_stmt.body);
        break;
    case STMT_TRY: {
        int child_last = gc_stmt_last_line(stmt->as.try_stmt.try_block);
        if (child_last > last) {
            last = child_last;
        }
        child_last = gc_stmt_last_line(stmt->as.try_stmt.catch_block);
        if (child_last > last) {
            last = child_last;
        }
        break;
    }
    default:
        break;
    }
    return last;
}

static void gc_scan_called_functions(GcScan *scan) {
    bool changed;
    do {
        changed = false;
        for (GcFunction *function = scan->functions; function; function = function->next) {
            if (function->scanned || !function->stmt->as.func_stmt.name ||
                !gc_name_seen(scan->references, function->stmt->as.func_stmt.name)) {
                continue;
            }
            function->scanned = true;
            gc_scan_stmt(scan, function->stmt->as.func_stmt.body, true);
            changed = true;
        }
    } while (changed && !scan->failed);
}

static void gc_scan_stmt(GcScan *scan, Stmt *stmt, bool all_lines) {
    if (!scan || !stmt || scan->failed) {
        return;
    }
    switch (stmt->type) {
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->as.block.count; i++) {
            gc_scan_stmt(scan, stmt->as.block.items[i], all_lines);
        }
        break;
    case STMT_EXPR:
        gc_scan_expr(scan, stmt->as.expr_stmt.expr, all_lines);
        break;
    case STMT_ASSIGN:
        if (all_lines || stmt->line > scan->current_line) {
            if (!stmt->as.assign.has_type) {
                gc_add_reference(scan, stmt->as.assign.name);
            }
            gc_scan_expr(scan, stmt->as.assign.target, all_lines);
            gc_scan_expr(scan, stmt->as.assign.value, all_lines);
            gc_scan_expr(scan, stmt->as.assign.schema_expr, all_lines);
        }
        break;
    case STMT_DECL:
        if (all_lines || stmt->line > scan->current_line) {
            gc_scan_expr(scan, stmt->as.decl.schema_expr, all_lines);
        }
        break;
    case STMT_IF:
        gc_scan_expr(scan, stmt->as.if_stmt.condition, all_lines);
        gc_scan_stmt(scan, stmt->as.if_stmt.then_branch, all_lines);
        for (size_t i = 0; i < stmt->as.if_stmt.elif_conditions.count; i++) {
            gc_scan_expr(scan, stmt->as.if_stmt.elif_conditions.items[i], all_lines);
            gc_scan_stmt(scan, stmt->as.if_stmt.elif_blocks.items[i], all_lines);
        }
        gc_scan_stmt(scan, stmt->as.if_stmt.else_branch, all_lines);
        break;
    case STMT_WHILE:
        if (all_lines || gc_stmt_last_line(stmt) > scan->current_line) {
            bool active = !all_lines && stmt->line <= scan->current_line;
            gc_scan_expr(scan, stmt->as.while_stmt.condition, all_lines || active);
            gc_scan_stmt(scan, stmt->as.while_stmt.body, all_lines || active);
        }
        break;
    case STMT_FOR:
        if (all_lines || gc_stmt_last_line(stmt) > scan->current_line) {
            bool active = !all_lines && stmt->line <= scan->current_line;
            gc_scan_expr(scan, stmt->as.for_stmt.target, all_lines || active);
            gc_scan_stmt(scan, stmt->as.for_stmt.body, all_lines || active);
        }
        break;
    case STMT_PARFOR:
        if (all_lines || gc_stmt_last_line(stmt) > scan->current_line) {
            bool active = !all_lines && stmt->line <= scan->current_line;
            gc_scan_expr(scan, stmt->as.parfor_stmt.target, all_lines || active);
            gc_scan_stmt(scan, stmt->as.parfor_stmt.body, all_lines || active);
        }
        break;
    case STMT_FUNC: {
        GcFunction *function = gc_add_function(scan, stmt);
        bool active = !all_lines && stmt->line <= scan->current_line &&
                      gc_stmt_last_line(stmt->as.func_stmt.body) >= scan->current_line;
        if (active) {
            gc_add_reference(scan, stmt->as.func_stmt.name);
        }
        for (size_t i = 0; i < stmt->as.func_stmt.params.count; i++) {
            gc_scan_expr(scan, stmt->as.func_stmt.params.items[i].default_value, all_lines);
            gc_scan_expr(scan, stmt->as.func_stmt.params.items[i].schema_expr, all_lines);
        }
        gc_scan_expr(scan, stmt->as.func_stmt.return_schema_expr, all_lines);
        if (function && (all_lines || active)) {
            function->scanned = true;
            gc_scan_stmt(scan, stmt->as.func_stmt.body, all_lines || active);
        }
        break;
    }
    case STMT_RETURN:
        gc_scan_expr(scan, stmt->as.return_stmt.value, all_lines);
        break;
    case STMT_BREAK:
        gc_scan_expr(scan, stmt->as.break_stmt.value, all_lines);
        break;
    case STMT_ASYNC:
        gc_scan_stmt(scan, stmt->as.async_stmt.body, all_lines);
        break;
    case STMT_THREAD:
        gc_scan_stmt(scan, stmt->as.thread_stmt.body, all_lines);
        break;
    case STMT_TRY:
        gc_scan_stmt(scan, stmt->as.try_stmt.try_block, all_lines);
        gc_scan_stmt(scan, stmt->as.try_stmt.catch_block, all_lines);
        break;
    case STMT_GOTO:
        gc_scan_expr(scan, stmt->as.goto_stmt.target, all_lines);
        break;
    case STMT_POP:
        gc_scan_expr(scan, stmt->as.pop_stmt.expr, all_lines);
        break;
    case STMT_GOTOPOINT:
        gc_scan_expr(scan, stmt->as.gotopoint_stmt.target, all_lines);
        break;
    default:
        break;
    }
}

static void gc_scan_source(GcScan *scan, const char *path, bool all_lines) {
    if (!scan || !path || scan->failed || !gc_mark_source(scan, path)) {
        return;
    }
    char *source = gc_read_file(path);
    if (!source) {
        scan->failed = true;
        return;
    }
    Lexer lexer;
    lexer_init(&lexer, source, path);
    Parser parser;
    parser_init(&parser, &lexer);
    Stmt *program = parser_parse(&parser);
    if (parser.had_error || !program) {
        scan->failed = true;
        free(parser.error_msg);
        free(source);
        free_stmt(program);
        return;
    }
    const char *previous_source = scan->active_source;
    scan->active_source = path;
    gc_scan_stmt(scan, program, all_lines);
    if (!all_lines && !scan->failed) {
        gc_scan_called_functions(scan);
    }
    scan->active_source = previous_source;
    free(parser.error_msg);
    free_stmt(program);
    free(source);
}

static char *gc_env_source(Env *env) {
    Value value = value_null();
    bool initialized = false;
    if (!env || !env_get(env, "__MODULE_SOURCE__", &value, NULL, NULL, &initialized) || !initialized ||
        value.type != VAL_STR || !value.as.s || value.as.s[0] == '\0') {
        value_free(value);
        return NULL;
    }
    char *source = strdup(value.as.s);
    value_free(value);
    return source;
}

static int gc_current_line(Interpreter *interp) {
    if (!interp || interp->trace_stack_count == 0) {
        return 0;
    }
    return interp->trace_stack[interp->trace_stack_count - 1].last_line;
}

static bool gc_scan_references(Interpreter *interp, Env *env, GcScan *scan) {
    if (!interp || !env || !scan) {
        return false;
    }
    scan->interp = interp;
    scan->current_line = gc_current_line(interp);
    char *current_source = gc_env_source(env);
    if (!current_source || current_source[0] == '<') {
        free(current_source);
        scan->failed = true;
        return false;
    }
    scan->current_source = current_source;
    scan->active_source = current_source;
    gc_scan_source(scan, current_source, false);
    free(current_source);

    for (ModuleEntry *module = (ModuleEntry *)interp->modules; module && !scan->failed; module = module->next) {
        char *module_source = gc_env_source(module->env);
        if (module_source) {
            scan->active_source = module_source;
            gc_scan_source(scan, module_source, true);
            free(module_source);
        }
    }
    scan->active_source = NULL;
    return !scan->failed;
}

static bool gc_module_name_referenced(Interpreter *interp, Env *env, const char *name, GcScan *scan) {
    if (!interp || !env || !name || !scan) {
        return false;
    }
    for (ModuleEntry *module = (ModuleEntry *)interp->modules; module; module = module->next) {
        if (module->env != env) {
            continue;
        }
        if (module->name) {
            size_t length = strlen(module->name) + strlen(name) + 2;
            char *qualified = malloc(length);
            if (qualified) {
                snprintf(qualified, length, "%s.%s", module->name, name);
                bool referenced = gc_name_seen(scan->references, qualified);
                free(qualified);
                if (referenced) {
                    return true;
                }
            }
        }
        for (ModuleEntry *alias = (ModuleEntry *)interp->modules; alias; alias = alias->next) {
            if (alias->env != env || !alias->name) {
                continue;
            }
            size_t length = strlen(alias->name) + strlen(name) + 2;
            char *qualified = malloc(length);
            if (qualified) {
                snprintf(qualified, length, "%s.%s", alias->name, name);
                bool referenced = gc_name_seen(scan->references, qualified);
                free(qualified);
                if (referenced) {
                    return true;
                }
            }
        }
        return false;
    }
    return false;
}

static bool gc_name_referenced(Interpreter *interp, Env *env, const char *name, GcScan *scan) {
    return scan && name && (gc_name_seen(scan->references, name) || gc_module_name_referenced(interp, env, name, scan));
}

static bool gc_alias_in_env(Env *env, Env *target_env, const char *name) {
    if (!env || !name) {
        return false;
    }
    for (size_t i = 0; i < env->count; i++) {
        EnvEntry *entry = &env->entries[i];
        if (entry->initialized && entry->alias_target && strcmp(entry->alias_target, name) == 0 &&
            (!entry->alias_target_env || entry->alias_target_env == target_env)) {
            return true;
        }
    }
    return false;
}

static bool gc_has_live_alias(Interpreter *interp, Env *target_env, const char *name, Env *visible_env) {
    for (Env *scope = visible_env; scope; scope = scope->parent) {
        if (gc_alias_in_env(scope, target_env, name)) {
            return true;
        }
    }
    for (ModuleEntry *module = interp ? (ModuleEntry *)interp->modules : NULL; module; module = module->next) {
        if (gc_alias_in_env(module->env, target_env, name)) {
            return true;
        }
    }
    return false;
}

#define RUNTIME_ERROR(interp, msg, line, col)                                                                          \
    do {                                                                                                               \
        free((interp)->error);                                                                                         \
        (interp)->error = strdup(msg);                                                                                 \
        (interp)->error_line = (line);                                                                                 \
        (interp)->error_col = (col);                                                                                   \
        return value_null();                                                                                           \
    } while (0)

static bool gc_argc(Interpreter *interp, int argc, int expected, const char *name, int line, int col) {
    if (argc == expected) {
        return true;
    }
    char message[128];
    snprintf(message, sizeof(message), "%s expects %d argument%s", name, expected, expected == 1 ? "" : "s");
    free(interp->error);
    interp->error = strdup(message);
    interp->error_line = line;
    interp->error_col = col;
    return false;
}

static bool gc_symbol_arg(Interpreter *interp, Value *args, int argc, const char *name, int line, int col) {
    if (argc != 1) {
        return gc_argc(interp, argc, 1, name, line, col);
    }
    if (args[0].type != VAL_STR) {
        char message[128];
        snprintf(message, sizeof(message), "%s expects str argument", name);
        free(interp->error);
        interp->error = strdup(message);
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    return true;
}

static bool gc_tracking_enabled(Env *env) {
    Value value = value_null();
    bool initialized = false;
    if (!env_get(env, "gc.track_new_symbols", &value, NULL, NULL, &initialized) || !initialized) {
        if (!env_get(env, "track_new_symbols", &value, NULL, NULL, &initialized) || !initialized) {
            return true;
        }
    }
    bool enabled = value.type == VAL_BOOL && value.as.boolean;
    value_free(value);
    return enabled;
}

static GcSymbol *gc_find(Interpreter *interp, Env *env, const char *name) {
    for (GcSymbol *symbol = gc_symbols; symbol; symbol = symbol->next) {
        if (symbol->interp == interp && symbol->env == env && strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

static bool gc_add(Interpreter *interp, Env *env, const char *name, bool force) {
    GcSymbol *existing = gc_find(interp, env, name);
    if (existing) {
        if (force && existing->ignored) {
            existing->ignored = false;
            return true;
        }
        return false;
    }
    GcSymbol *symbol = malloc(sizeof(*symbol));
    if (!symbol) {
        return false;
    }
    symbol->interp = interp;
    symbol->env = env;
    symbol->name = strdup(name);
    symbol->ignored = false;
    if (!symbol->name) {
        free(symbol);
        return false;
    }
    symbol->next = gc_symbols;
    gc_symbols = symbol;
    return true;
}

static EnvEntry *gc_find_local(Env *env, const char *name) {
    if (!env || !name) {
        return NULL;
    }
    for (size_t i = 0; i < env->count; i++) {
        if (env->entries[i].name && strcmp(env->entries[i].name, name) == 0) {
            return &env->entries[i];
        }
    }
    return NULL;
}

static bool gc_find_visible(Env *env, const char *name, Env **owner) {
    for (Env *scope = env; scope; scope = scope->parent) {
        if (gc_find_local(scope, name)) {
            if (owner) {
                *owner = scope;
            }
            return true;
        }
    }
    return false;
}

static bool gc_track_visible(Interpreter *interp, Env *env, bool force) {
    bool added = false;
    for (Env *scope = env; scope; scope = scope->parent) {
        for (size_t i = 0; i < scope->count; i++) {
            EnvEntry *entry = &scope->entries[i];
            if (entry->name && !(entry->name[0] == '_' && entry->name[1] == '_') &&
                gc_add(interp, scope, entry->name, force)) {
                added = true;
            }
        }
    }
    return added;
}

static bool gc_track_all_existing(Interpreter *interp, Env *env, bool force) {
    bool added = gc_track_visible(interp, env, force);
    for (ModuleEntry *module = interp ? (ModuleEntry *)interp->modules : NULL; module; module = module->next) {
        for (size_t i = 0; i < module->env->count; i++) {
            EnvEntry *entry = &module->env->entries[i];
            if (entry->name && !(entry->name[0] == '_' && entry->name[1] == '_') &&
                gc_add(interp, module->env, entry->name, force)) {
                added = true;
            }
        }
    }
    return added;
}

static bool gc_sync(Interpreter *interp, Env *env) {
    return gc_tracking_enabled(env) ? gc_track_all_existing(interp, env, false) : false;
}

static bool gc_remove_entry(Env *env, const char *name) {
    EnvEntry *entry = gc_find_local(env, name);
    if (!entry || entry->frozen || entry->permafrozen) {
        return false;
    }
    size_t index = (size_t)(entry - env->entries);
    free(entry->name);
    free(entry->alias_target);
    value_free(entry->schema);
    if (entry->initialized) {
        value_free(entry->value);
    }
    if (index + 1 < env->count) {
        memmove(&env->entries[index], &env->entries[index + 1], (env->count - index - 1) * sizeof(EnvEntry));
    }
    env->count--;
    free(env->ht_slots);
    env->ht_slots = NULL;
    env->ht_capacity = 0;
    env->ht_count = 0;
    return true;
}

static Value gc_track_all(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    if (!gc_argc(interp, argc, 0, "track_all", line, col)) {
        return value_null();
    }
    return value_bool(gc_track_all_existing(interp, env, true));
}

static Value gc_track(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line, int col) {
    (void)arg_nodes;
    if (!gc_symbol_arg(interp, args, argc, "track", line, col)) {
        return value_null();
    }
    gc_sync(interp, env);
    Env *owner = NULL;
    if (!gc_find_visible(env, args[0].as.s, &owner)) {
        char message[256];
        snprintf(message, sizeof(message), "track: symbol '%s' does not exist", args[0].as.s);
        RUNTIME_ERROR(interp, message, line, col);
    }
    return value_bool(gc_add(interp, owner, args[0].as.s, true));
}

static Value gc_ignore_all(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    if (!gc_argc(interp, argc, 0, "ignore_all", line, col)) {
        return value_null();
    }
    gc_sync(interp, env);
    bool removed = false;
    for (GcSymbol *symbol = gc_symbols; symbol; symbol = symbol->next) {
        if (symbol->interp == interp) {
            if (!symbol->ignored) {
                symbol->ignored = true;
                removed = true;
            }
        }
    }
    return value_bool(removed);
}

static Value gc_ignore(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line, int col) {
    (void)arg_nodes;
    if (!gc_symbol_arg(interp, args, argc, "ignore", line, col)) {
        return value_null();
    }
    gc_sync(interp, env);
    Env *owner = NULL;
    if (!gc_find_visible(env, args[0].as.s, &owner)) {
        char message[256];
        snprintf(message, sizeof(message), "ignore: symbol '%s' does not exist", args[0].as.s);
        RUNTIME_ERROR(interp, message, line, col);
    }
    GcSymbol *symbol = gc_find(interp, owner, args[0].as.s);
    if (!symbol || symbol->ignored) {
        return value_bool(false);
    }
    symbol->ignored = true;
    return value_bool(true);
}

static Value gc_collect(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    if (!gc_argc(interp, argc, 0, "collect", line, col)) {
        return value_null();
    }
    gc_sync(interp, env);
    GcScan scan = {0};
    if (!gc_scan_references(interp, env, &scan)) {
        gc_free_scan(&scan);
        return value_bool(false);
    }
    bool collected = false;
    GcSymbol **link = &gc_symbols;
    while (*link) {
        GcSymbol *symbol = *link;
        if (symbol->interp == interp) {
            EnvEntry *entry = gc_find_local(symbol->env, symbol->name);
            if (!entry) {
                *link = symbol->next;
                free(symbol->name);
                free(symbol);
                continue;
            }
            if (!symbol->ignored && !gc_name_referenced(interp, symbol->env, symbol->name, &scan) &&
                !gc_has_live_alias(interp, symbol->env, symbol->name, env) &&
                gc_remove_entry(symbol->env, symbol->name)) {
                *link = symbol->next;
                free(symbol->name);
                free(symbol);
                collected = true;
                continue;
            }
        }
        link = &symbol->next;
    }
    gc_free_scan(&scan);
    return value_bool(collected);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context *ctx) {
    if (!ctx) {
        return;
    }
    ctx->register_operator("track_all", gc_track_all, PREFIX_EXTENSION_MODULE_RESTRICTED);
    ctx->register_operator("track", gc_track, PREFIX_EXTENSION_MODULE_RESTRICTED);
    ctx->register_operator("ignore_all", gc_ignore_all, PREFIX_EXTENSION_MODULE_RESTRICTED);
    ctx->register_operator("ignore", gc_ignore, PREFIX_EXTENSION_MODULE_RESTRICTED);
    ctx->register_operator("collect", gc_collect, PREFIX_EXTENSION_MODULE_RESTRICTED);
}
