#include "../../src/interpreter.h"
#include "../../src/prefix_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PREFIX_TEST_EXPORT __declspec(dllexport)
#else
#define PREFIX_TEST_EXPORT
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static char *g_library_path = NULL;

PREFIX_TEST_EXPORT void prefix_extension_init(prefix_ext_context *ctx);

static void set_runtime_error(Interpreter *interp, const char *msg, int line, int col) {
    if (!interp) {
        return;
    }
    if (interp->error) {
        free(interp->error);
    }
    interp->error = msg ? strdup(msg) : NULL;
    interp->error_line = line;
    interp->error_col = col;
}

static int expect_argc_eq(Interpreter *interp, int argc, int expected, const char *opname, int line, int col) {
    if (argc == expected) {
        return 1;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "%s expects %d argument%s", opname, expected, expected == 1 ? "" : "s");
    set_runtime_error(interp, buf, line, col);
    return 0;
}

static char *capture_library_path(void) {
#ifdef _WIN32
    HMODULE module = NULL;
    char buffer[4096];
    DWORD length = 0;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(const void *)&prefix_extension_init, &module)) {
        return NULL;
    }

    length = GetModuleFileNameA(module, buffer, (DWORD)sizeof(buffer));
    if (length == 0 || length >= (DWORD)sizeof(buffer)) {
        return NULL;
    }

    return strdup(buffer);
#else
    Dl_info info;
    if (dladdr((void *)&prefix_extension_init, &info) == 0 || !info.dli_fname) {
        return NULL;
    }
    return strdup(info.dli_fname);
#endif
}

static Value op_tst_search_probe_path(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line,
                                      int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_SEARCH_PROBE_PATH", line, col)) {
        return value_null();
    }
    return value_str(g_library_path ? g_library_path : "");
}

PREFIX_TEST_EXPORT void prefix_extension_init(prefix_ext_context *ctx) {
    if (!ctx) {
        return;
    }

    if (!g_library_path) {
        g_library_path = capture_library_path();
    }

    ctx->register_operator("TST_SEARCH_PROBE_PATH", op_tst_search_probe_path, 0);
}