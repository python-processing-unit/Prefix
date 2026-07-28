#include "../../../src/interpreter.h"
#include "../../../src/prefix_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static Value op_tst_ext_package_init(Interpreter *interp, Value *args, int argc, Expr **arg_nodes, Env *env, int line,
                                     int col) {
    (void)args;
    (void)arg_nodes;
    (void)env;
    if (!expect_argc_eq(interp, argc, 0, "TST_EXT_PACKAGE_INIT", line, col)) {
        return value_null();
    }
    return value_str("package-init");
}

#ifdef _WIN32
__declspec(dllexport)
#endif
void prefix_extension_init(prefix_ext_context *ctx) {
    if (!ctx) {
        return;
    }
    ctx->register_operator("TST_EXT_PACKAGE_INIT", op_tst_ext_package_init, 0);
}