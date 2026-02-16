#include <stdint.h>
#include <string.h>

#include "../src/prefix_extension.h"

#ifdef _WIN32
#define PREFIX_EXT_EXPORT __declspec(dllexport)
#else
#define PREFIX_EXT_EXPORT
#endif

static Value make_int(int64_t v) {
    Value out;
    memset(&out, 0, sizeof(out));
    out.type = VAL_INT;
    out.as.i = v;
    return out;
}

static int64_t g_counter = 0;

static Value op_get_counter(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)interp; (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    return make_int(g_counter);
}

static Value op_reset_counter(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)interp; (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    g_counter = 0;
    return make_int(0);
}

static Value op_ping(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)interp;
    (void)args;
    (void)arg_nodes;
    (void)env;
    (void)line;
    (void)col;
    /* increment internal counter so tests can observe extension state
       (periodic hooks/event handlers are not implemented in this test runtime) */
    g_counter++;
    return make_int((int64_t)argc);
}

static Value op_iadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    int64_t sum = 0;
    int i;
    (void)interp;
    (void)arg_nodes;
    (void)env;
    (void)line;
    (void)col;

    for (i = 0; i < argc; i++) {
        if (args[i].type != VAL_INT) {
            return make_int(-1);
        }
        sum += args[i].as.i;
    }

    return make_int(sum);
}

static void on_event(Interpreter* interp, const char* event_name) {
    (void)interp;
    /* increment counter when event fires */
    if (strcmp(event_name, "test_event") == 0) {
        g_counter++;
    }
}

static void on_periodic(Interpreter* interp, const char* event_name) {
    (void)interp;
    (void)event_name;
    g_counter++;
}

PREFIX_EXT_EXPORT void prefix_extension_init(prefix_ext_context* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->api_version != PREFIX_EXTENSION_API_VERSION) {
        return;
    }

    (void)ctx->register_operator("PING", op_ping, PREFIX_EXTENSION_ASMODULE);
    (void)ctx->register_operator("IADD", op_iadd, PREFIX_EXTENSION_ASMODULE);
    (void)ctx->register_operator("GET_COUNTER", op_get_counter, PREFIX_EXTENSION_ASMODULE);
    (void)ctx->register_operator("RESET_COUNTER", op_reset_counter, PREFIX_EXTENSION_ASMODULE);
    /* Test global operator registration (asmodule=0) */
    (void)ctx->register_operator("GLOBAL_PING", op_ping, 0);

    /* Use a custom event that we can trigger manually or via tests if possible, 
       but here we use "program_start" as before, plus a custom one. */
    (void)ctx->register_event_handler("program_start", on_event);
    (void)ctx->register_event_handler("test_event", on_event);
    
    /* Register periodic hook to run every 10 instructions */
    (void)ctx->register_periodic_hook(10, on_periodic);
}

