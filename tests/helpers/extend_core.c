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

static int g_init_count = 0;
static int g_api_version = -1;

static Value op_tst_ext_global(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_GLOBAL", line, col)) return value_null();
    return value_str("core-global");
}

static Value op_tst_ext_init_count(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_INIT_COUNT", line, col)) return value_null();
    return value_int(g_init_count);
}

static Value op_tst_ext_api_version(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_API_VERSION", line, col)) return value_null();
    return value_int(g_api_version);
}

static Value op_tst_ext_restricted(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_RESTRICTED", line, col)) return value_null();
    return value_str("core-restricted");
}

static Value op_tst_ext_asmodule_only(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_ASMODULE_ONLY", line, col)) return value_null();
    return value_str("asmodule-global");
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context* ctx) {
    if (!ctx) return;

    g_init_count += 1;
    g_api_version = ctx->api_version;

    ctx->register_operator("TST_EXT_GLOBAL", op_tst_ext_global, 0);
    ctx->register_operator("TST_EXT_INIT_COUNT", op_tst_ext_init_count, 0);
    ctx->register_operator("TST_EXT_API_VERSION", op_tst_ext_api_version, 0);
    ctx->register_operator("TST_EXT_RESTRICTED", op_tst_ext_restricted,
                           PREFIX_EXTENSION_ASMODULE | PREFIX_EXTENSION_MODULE_RESTRICTED);
    ctx->register_operator("TST_EXT_ASMODULE_ONLY", op_tst_ext_asmodule_only,
                           PREFIX_EXTENSION_ASMODULE);
}