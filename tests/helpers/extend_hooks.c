#include "../../src/prefix_extension.h"
#include "../../src/interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_runtime_error(Interpreter* interp, const char* msg, int line, int col) {
    if (!interp) return;
    if (interp->error) free(interp->error);
    interp->error = msg ? strdup(msg) : NULL;
    interp->error_line = line;
    interp->error_col = col;
}

static int expect_argc_eq(Interpreter* interp,
                          int argc,
                          int expected,
                          const char* opname,
                          int line,
                          int col) {
    if (argc == expected) return 1;

    char buf[160];
    snprintf(buf, sizeof(buf), "%s expects %d argument%s", opname, expected, expected == 1 ? "" : "s");
    set_runtime_error(interp, buf, line, col);
    return 0;
}

static char* dup_env(const char* name) {
    const char* value = getenv(name);
    return value ? strdup(value) : NULL;
}

static FILE* open_file(const char* path, const char* mode) {
    if (!path || !mode) return NULL;
    return fopen(path, mode);
}

static void write_text(const char* path, const char* text) {
    FILE* file;

    if (!path) return;
    file = open_file(path, "wb");
    if (!file) return;

    if (text && text[0] != '\0') {
        fwrite(text, 1, strlen(text), file);
    }
    fclose(file);
}

static void append_text(const char* path, const char* text) {
    FILE* file;

    if (!path || !text) return;
    file = open_file(path, "ab");
    if (!file) return;

    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

static void append_line(const char* path, const char* text) {
    if (!path || !text) return;
    append_text(path, text);
    append_text(path, "\n");
}

static char* g_event_log_path = NULL;
static char* g_periodic_log_path = NULL;
static char* g_repl_log_path = NULL;

static void hook_log_event(Interpreter* interp, const char* event_name) {
    (void)interp;
    if (!g_event_log_path || !event_name || event_name[0] == '\0') return;
    append_line(g_event_log_path, event_name);
}

static void hook_log_periodic(Interpreter* interp, const char* event_name) {
    (void)interp;
    (void)event_name;
    if (!g_periodic_log_path) return;
    append_line(g_periodic_log_path, "periodic");
}

static int hook_run_repl(void) {
    if (g_repl_log_path) {
        write_text(g_repl_log_path, "repl-handler");
    }
    return 0;
}

static Value op_tst_hooks_ready(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_HOOKS_READY", line, col)) return value_null();
    return value_str("hooks-ready");
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context* ctx) {
    if (!ctx) return;

    if (!g_event_log_path) g_event_log_path = dup_env("PREFIX_TEST_EVENT_LOG");
    if (!g_periodic_log_path) g_periodic_log_path = dup_env("PREFIX_TEST_PERIODIC_LOG");
    if (!g_repl_log_path) g_repl_log_path = dup_env("PREFIX_TEST_REPL_LOG");

    ctx->register_operator("TST_HOOKS_READY", op_tst_hooks_ready, 0);
    ctx->register_event_handler("program_start", hook_log_event);
    ctx->register_event_handler("program_end", hook_log_event);
    ctx->register_event_handler("on_error", hook_log_event);
    ctx->register_event_handler("before_statement", hook_log_event);
    ctx->register_event_handler("after_statement", hook_log_event);
    ctx->register_event_handler("before_call", hook_log_event);
    ctx->register_event_handler("after_call", hook_log_event);
    ctx->register_periodic_hook(3, hook_log_periodic);
    ctx->register_repl_handler(hook_run_repl);
}