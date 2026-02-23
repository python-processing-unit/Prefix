#include "builtins.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "extensions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#ifndef _MSC_VER
#include <sys/wait.h>
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100) // unreferenced formal parameter
#pragma warning(disable:4996) // unsafe CRT functions like strcpy/strcat
#endif

// Forward declarations for interpreter functions we need
Value eval_expr(Interpreter* interp, Expr* expr, Env* env);
int value_truthiness(Value v);

// Helper macros
#define RUNTIME_ERROR(interp, msg, line, col) \
    do { \
        (interp)->error = strdup(msg); \
        (interp)->error_line = line; \
        (interp)->error_col = col; \
        return value_null(); \
    } while(0)

#define EXPECT_INT(v, name, interp, line, col) \
    do { \
        if ((v).type != VAL_INT) { \
            char buf[128]; \
            snprintf(buf, sizeof(buf), "%s expects INT argument", name); \
            RUNTIME_ERROR(interp, buf, line, col); \
        } \
    } while(0)

#define EXPECT_FLT(v, name, interp, line, col) \
    do { \
        if ((v).type != VAL_FLT) { \
            char buf[128]; \
            snprintf(buf, sizeof(buf), "%s expects FLT argument", name); \
            RUNTIME_ERROR(interp, buf, line, col); \
        } \
    } while(0)

#define EXPECT_STR(v, name, interp, line, col) \
    do { \
        if ((v).type != VAL_STR) { \
            char buf[128]; \
            snprintf(buf, sizeof(buf), "%s expects STR argument", name); \
            RUNTIME_ERROR(interp, buf, line, col); \
        } \
    } while(0)

#define EXPECT_NUM(v, name, interp, line, col) \
    do { \
        if ((v).type != VAL_INT && (v).type != VAL_FLT) { \
            char buf[128]; \
            snprintf(buf, sizeof(buf), "%s expects INT or FLT argument", name); \
            RUNTIME_ERROR(interp, buf, line, col); \
        } \
    } while(0)

static bool writeback_first_ptr(Interpreter* interp, Expr** arg_nodes, Env* env, Value result, const char* rule, int line, int col) {
    if (!arg_nodes || !arg_nodes[0]) return true;
    if (arg_nodes[0]->type != EXPR_PTR) return true;
    const char* name = arg_nodes[0]->as.ptr_name;
    if (!name) {
        interp->error = strdup("Invalid pointer target");
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    if (!env_assign(env, name, result, TYPE_UNKNOWN, false)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s writeback failed", rule);
        interp->error = strdup(buf);
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    return true;
}

static char* canonicalize_existing_path(const char* path) {
    if (!path || path[0] == '\0') return NULL;
#ifdef _WIN32
    char full[_MAX_PATH];
    if (_fullpath(full, path, _MAX_PATH)) {
        return strdup(full);
    }
#else
    char full[PATH_MAX];
    if (realpath(path, full)) {
        return strdup(full);
    }
#endif
    return strdup(path);
}

    // Global argv storage (set by main via builtins_set_argv)
    static int g_argc = 0;
    static char** g_argv = NULL;

// Helper: convert integer to binary string
static char* int_to_binary_str(int64_t val) {
    if (val == 0) return strdup("0");
    
    int is_negative = val < 0;
    uint64_t uval = is_negative ? (uint64_t)(-val) : (uint64_t)val;
    
    char buf[128];
    int pos = 127;
    buf[pos--] = '\0';
    
    while (uval > 0 && pos >= 0) {
        buf[pos--] = (uval & 1) ? '1' : '0';
        uval >>= 1;
    }
    
    if (is_negative && pos >= 0) {
        buf[pos--] = '-';
    }
    
    return strdup(&buf[pos + 1]);
}

// Helper: convert float to binary string
static char* flt_to_binary_str(double val) {
    char buf[128];
    if (isnan(val)) {
        return strdup("NaN");
    }
    if (isinf(val)) {
        return strdup(signbit(val) ? "-INF" : "INF");
    }
    int is_negative = val < 0;
    if (is_negative) val = -val;
    
    int64_t int_part = (int64_t)val;
    double frac_part = val - (double)int_part;
    
    // Integer part
    char* int_str = int_to_binary_str(int_part);
    
    // Fractional part (up to 32 bits of precision)
    char frac_buf[64];
    int frac_pos = 0;
    for (int i = 0; i < 32 && frac_part > 0; i++) {
        frac_part *= 2;
        if (frac_part >= 1.0) {
            frac_buf[frac_pos++] = '1';
            frac_part -= 1.0;
        } else {
            frac_buf[frac_pos++] = '0';
        }
    }
    frac_buf[frac_pos] = '\0';
    
    // Remove trailing zeros
    while (frac_pos > 0 && frac_buf[frac_pos - 1] == '0') {
        frac_buf[--frac_pos] = '\0';
    }
    
    if (frac_pos == 0) {
        snprintf(buf, sizeof(buf), "%s%s.0", is_negative ? "-" : "", int_str);
    } else {
        snprintf(buf, sizeof(buf), "%s%s.%s", is_negative ? "-" : "", int_str, frac_buf);
    }
    
    free(int_str);
    return strdup(buf);
}

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} JsonBuf;

static void jb_init(JsonBuf* jb) {
    jb->data = NULL;
    jb->len = 0;
    jb->cap = 0;
}

static void jb_reserve(JsonBuf* jb, size_t extra) {
    if (jb->len + extra + 1 > jb->cap) {
        size_t new_cap = jb->cap == 0 ? 256 : jb->cap * 2;
        while (new_cap < jb->len + extra + 1) new_cap *= 2;
        jb->data = realloc(jb->data, new_cap);
        if (!jb->data) { fprintf(stderr, "Out of memory\n"); exit(1); }
        jb->cap = new_cap;
    }
}

static void jb_append_char(JsonBuf* jb, char c) {
    jb_reserve(jb, 1);
    jb->data[jb->len++] = c;
    jb->data[jb->len] = '\0';
}

static void jb_append_str(JsonBuf* jb, const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    jb_reserve(jb, n);
    memcpy(jb->data + jb->len, s, n);
    jb->len += n;
    jb->data[jb->len] = '\0';
}

static void jb_append_fmt(JsonBuf* jb, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        jb_append_str(jb, tmp);
        return;
    }
    char* buf = malloc((size_t)n + 1);
    if (!buf) { fprintf(stderr, "Out of memory\n"); exit(1); }
    va_start(args, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, args);
    va_end(args);
    jb_append_str(jb, buf);
    free(buf);
}

static void jb_free(JsonBuf* jb) {
    free(jb->data);
    jb->data = NULL;
    jb->len = 0;
    jb->cap = 0;
}

static void jb_append_json_string(JsonBuf* jb, const char* s) {
    jb_append_char(jb, '"');
    if (!s) s = "";
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"': jb_append_str(jb, "\\\""); break;
            case '\\': jb_append_str(jb, "\\\\"); break;
            case '\b': jb_append_str(jb, "\\b"); break;
            case '\f': jb_append_str(jb, "\\f"); break;
            case '\n': jb_append_str(jb, "\\n"); break;
            case '\r': jb_append_str(jb, "\\r"); break;
            case '\t': jb_append_str(jb, "\\t"); break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    jb_append_fmt(jb, "\\u%04x", (unsigned int)c);
                } else {
                    jb_append_char(jb, (char)c);
                }
                break;
        }
    }
    jb_append_char(jb, '"');
}

static const char* decl_type_name(DeclType dt) {
    switch (dt) {
        case TYPE_INT: return "INT";
        case TYPE_FLT: return "FLT";
        case TYPE_STR: return "STR";
        case TYPE_TNS: return "TNS";
        case TYPE_MAP: return "MAP";
        case TYPE_FUNC: return "FUNC";
        case TYPE_THR: return "THR";
        default: return "UNKNOWN";
    }
}

static DeclType decl_type_from_name(const char* name) {
    if (!name) return TYPE_UNKNOWN;
    if (strcmp(name, "INT") == 0) return TYPE_INT;
    if (strcmp(name, "FLT") == 0) return TYPE_FLT;
    if (strcmp(name, "STR") == 0) return TYPE_STR;
    if (strcmp(name, "TNS") == 0) return TYPE_TNS;
    if (strcmp(name, "MAP") == 0) return TYPE_MAP;
    if (strcmp(name, "FUNC") == 0) return TYPE_FUNC;
    if (strcmp(name, "THR") == 0) return TYPE_THR;
    return TYPE_UNKNOWN;
}

static EnvEntry* env_find_local_entry(Env* env, const char* name) {
    if (!env || !name) return NULL;
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) return &env->entries[i];
    }
    return NULL;
}

static Env* env_find_owner(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        if (env_find_local_entry(e, name)) return e;
    }
    return NULL;
}

// ---- JSON parsing ----
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    JsonValue** items;
    size_t count;
    size_t cap;
} JsonArray;

typedef struct {
    char* key;
    JsonValue* value;
} JsonPair;

typedef struct {
    JsonPair* items;
    size_t count;
    size_t cap;
} JsonObject;

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double num;
        char* str;
        JsonArray arr;
        JsonObject obj;
    } as;
};

typedef struct {
    const char* text;
    size_t pos;
    size_t len;
    const char* error;
} JsonParser;

static void json_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
            continue;
        }
        break;
    }
}

static JsonValue* json_new(JsonType type) {
    JsonValue* v = malloc(sizeof(JsonValue));
    if (!v) { fprintf(stderr, "Out of memory\n"); exit(1); }
    memset(v, 0, sizeof(JsonValue));
    v->type = type;
    return v;
}

static void json_arr_add(JsonArray* arr, JsonValue* v) {
    if (arr->count + 1 > arr->cap) {
        size_t new_cap = arr->cap == 0 ? 4 : arr->cap * 2;
        arr->items = realloc(arr->items, new_cap * sizeof(JsonValue*));
        if (!arr->items) { fprintf(stderr, "Out of memory\n"); exit(1); }
        arr->cap = new_cap;
    }
    arr->items[arr->count++] = v;
}

static void json_obj_add(JsonObject* obj, const char* key, JsonValue* v) {
    if (obj->count + 1 > obj->cap) {
        size_t new_cap = obj->cap == 0 ? 4 : obj->cap * 2;
        obj->items = realloc(obj->items, new_cap * sizeof(JsonPair));
        if (!obj->items) { fprintf(stderr, "Out of memory\n"); exit(1); }
        obj->cap = new_cap;
    }
    obj->items[obj->count].key = key ? strdup(key) : strdup("");
    obj->items[obj->count].value = v;
    obj->count++;
}

static char json_peek(JsonParser* p) {
    if (p->pos >= p->len) return '\0';
    return p->text[p->pos];
}

static char json_next(JsonParser* p) {
    if (p->pos >= p->len) return '\0';
    return p->text[p->pos++];
}

static JsonValue* json_parse_value(JsonParser* p);

static char* json_parse_string_raw(JsonParser* p) {
    if (json_next(p) != '"') return NULL;
    JsonBuf sb; jb_init(&sb);
    while (p->pos < p->len) {
        char c = json_next(p);
        if (c == '"') break;
        if (c == '\\') {
            char e = json_next(p);
            switch (e) {
                case '"': jb_append_char(&sb, '"'); break;
                case '\\': jb_append_char(&sb, '\\'); break;
                case '/': jb_append_char(&sb, '/'); break;
                case 'b': jb_append_char(&sb, '\b'); break;
                case 'f': jb_append_char(&sb, '\f'); break;
                case 'n': jb_append_char(&sb, '\n'); break;
                case 'r': jb_append_char(&sb, '\r'); break;
                case 't': jb_append_char(&sb, '\t'); break;
                case 'u': {
                    int code = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = json_next(p);
                        int v = -1;
                        if (h >= '0' && h <= '9') v = h - '0';
                        else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                        if (v < 0) { p->error = "Invalid unicode escape"; jb_free(&sb); return NULL; }
                        code = (code << 4) | v;
                    }
                    if (code <= 0x7f) {
                        jb_append_char(&sb, (char)code);
                    } else if (code <= 0x7ff) {
                        jb_append_char(&sb, (char)(0xC0 | ((code >> 6) & 0x1F)));
                        jb_append_char(&sb, (char)(0x80 | (code & 0x3F)));
                    } else {
                        jb_append_char(&sb, (char)(0xE0 | ((code >> 12) & 0x0F)));
                        jb_append_char(&sb, (char)(0x80 | ((code >> 6) & 0x3F)));
                        jb_append_char(&sb, (char)(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    p->error = "Invalid escape";
                    jb_free(&sb);
                    return NULL;
            }
        } else {
            jb_append_char(&sb, c);
        }
    }
    char* out = sb.data ? strdup(sb.data) : strdup("");
    jb_free(&sb);
    return out;
}

static JsonValue* json_parse_string(JsonParser* p) {
    char* s = json_parse_string_raw(p);
    if (!s) return NULL;
    JsonValue* v = json_new(JSON_STR);
    v->as.str = s;
    return v;
}

static JsonValue* json_parse_number(JsonParser* p) {
    const char* start = p->text + p->pos;
    char* end = NULL;
    double val = strtod(start, &end);
    if (end == start) { p->error = "Invalid number"; return NULL; }
    p->pos = (size_t)(end - p->text);
    JsonValue* v = json_new(JSON_NUM);
    v->as.num = val;
    return v;
}

static JsonValue* json_parse_array(JsonParser* p) {
    if (json_next(p) != '[') return NULL;
    JsonValue* v = json_new(JSON_ARR);
    json_skip_ws(p);
    if (json_peek(p) == ']') { json_next(p); return v; }
    while (p->pos < p->len) {
        json_skip_ws(p);
        JsonValue* item = json_parse_value(p);
        if (!item) return NULL;
        json_arr_add(&v->as.arr, item);
        json_skip_ws(p);
        char c = json_next(p);
        if (c == ']') break;
        if (c != ',') { p->error = "Expected ',' in array"; return NULL; }
    }
    return v;
}

static JsonValue* json_parse_object(JsonParser* p) {
    if (json_next(p) != '{') return NULL;
    JsonValue* v = json_new(JSON_OBJ);
    json_skip_ws(p);
    if (json_peek(p) == '}') { json_next(p); return v; }
    while (p->pos < p->len) {
        json_skip_ws(p);
        if (json_peek(p) != '"') { p->error = "Expected string key"; return NULL; }
        char* key = json_parse_string_raw(p);
        if (!key) return NULL;
        json_skip_ws(p);
        if (json_next(p) != ':') { free(key); p->error = "Expected ':'"; return NULL; }
        json_skip_ws(p);
        JsonValue* val = json_parse_value(p);
        if (!val) { free(key); return NULL; }
        json_obj_add(&v->as.obj, key, val);
        free(key);
        json_skip_ws(p);
        char c = json_next(p);
        if (c == '}') break;
        if (c != ',') { p->error = "Expected ',' in object"; return NULL; }
    }
    return v;
}

static JsonValue* json_parse_value(JsonParser* p) {
    json_skip_ws(p);
    char c = json_peek(p);
    if (c == '"') return json_parse_string(p);
    if (c == '{') return json_parse_object(p);
    if (c == '[') return json_parse_array(p);
    if (c == '-' || (c >= '0' && c <= '9')) return json_parse_number(p);
    if (strncmp(p->text + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JsonValue* v = json_new(JSON_BOOL);
        v->as.boolean = 1;
        return v;
    }
    if (strncmp(p->text + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JsonValue* v = json_new(JSON_BOOL);
        v->as.boolean = 0;
        return v;
    }
    if (strncmp(p->text + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return json_new(JSON_NULL);
    }
    p->error = "Unexpected token";
    return NULL;
}

static JsonValue* json_parse(const char* text, const char** err) {
    JsonParser p = {0};
    p.text = text ? text : "";
    p.len = strlen(p.text);
    JsonValue* v = json_parse_value(&p);
    if (!v || p.error) {
        if (err) *err = p.error ? p.error : "Invalid JSON";
        return NULL;
    }
    json_skip_ws(&p);
    if (p.pos < p.len) {
        if (err) *err = "Trailing data";
        return NULL;
    }
    return v;
}

static void json_free(JsonValue* v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STR:
            free(v->as.str);
            break;
        case JSON_ARR:
            for (size_t i = 0; i < v->as.arr.count; i++) json_free(v->as.arr.items[i]);
            free(v->as.arr.items);
            break;
        case JSON_OBJ:
            for (size_t i = 0; i < v->as.obj.count; i++) {
                free(v->as.obj.items[i].key);
                json_free(v->as.obj.items[i].value);
            }
            free(v->as.obj.items);
            break;
        default:
            break;
    }
    free(v);
}

static JsonValue* json_obj_get(JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->as.obj.count; i++) {
        if (strcmp(obj->as.obj.items[i].key, key) == 0) return obj->as.obj.items[i].value;
    }
    return NULL;
}

typedef struct {
    Env** envs;
    char** env_ids;
    int* env_state; // 0 = none, 1 = in_progress, 2 = done
    size_t env_count;
    size_t env_cap;
    int next_env_id;
    Func** funcs;
    char** func_ids;
    int* func_state;
    size_t func_count;
    size_t func_cap;
    int next_func_id;
    Thr** thrs;
    char** thr_ids;
    size_t thr_count;
    size_t thr_cap;
    int next_thr_id;
} SerCtx;

static void ser_ctx_init(SerCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static void ser_ctx_free(SerCtx* ctx) {
    for (size_t i = 0; i < ctx->env_count; i++) free(ctx->env_ids[i]);
    for (size_t i = 0; i < ctx->func_count; i++) free(ctx->func_ids[i]);
    for (size_t i = 0; i < ctx->thr_count; i++) free(ctx->thr_ids[i]);
    free(ctx->envs);
    free(ctx->env_ids);
    free(ctx->env_state);
    free(ctx->funcs);
    free(ctx->func_ids);
    free(ctx->func_state);
    free(ctx->thrs);
    free(ctx->thr_ids);
}

static const char* ser_env_id(SerCtx* ctx, Env* env, int* state) {
    for (size_t i = 0; i < ctx->env_count; i++) {
        if (ctx->envs[i] == env) {
            if (state) *state = ctx->env_state[i];
            return ctx->env_ids[i];
        }
    }
    if (ctx->env_count + 1 > ctx->env_cap) {
        size_t new_cap = ctx->env_cap == 0 ? 4 : ctx->env_cap * 2;
        ctx->envs = realloc(ctx->envs, new_cap * sizeof(Env*));
        ctx->env_ids = realloc(ctx->env_ids, new_cap * sizeof(char*));
        ctx->env_state = realloc(ctx->env_state, new_cap * sizeof(int));
        if (!ctx->envs || !ctx->env_ids || !ctx->env_state) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->env_cap = new_cap;
    }
    ctx->next_env_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "e%d", ctx->next_env_id);
    ctx->envs[ctx->env_count] = env;
    ctx->env_ids[ctx->env_count] = strdup(buf);
    ctx->env_state[ctx->env_count] = 0;
    if (state) *state = 0;
    ctx->env_count++;
    return ctx->env_ids[ctx->env_count - 1];
}

static const char* ser_func_id(SerCtx* ctx, Func* func, int* state) {
    for (size_t i = 0; i < ctx->func_count; i++) {
        if (ctx->funcs[i] == func) {
            if (state) *state = ctx->func_state[i];
            return ctx->func_ids[i];
        }
    }
    if (ctx->func_count + 1 > ctx->func_cap) {
        size_t new_cap = ctx->func_cap == 0 ? 4 : ctx->func_cap * 2;
        ctx->funcs = realloc(ctx->funcs, new_cap * sizeof(Func*));
        ctx->func_ids = realloc(ctx->func_ids, new_cap * sizeof(char*));
        ctx->func_state = realloc(ctx->func_state, new_cap * sizeof(int));
        if (!ctx->funcs || !ctx->func_ids || !ctx->func_state) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->func_cap = new_cap;
    }
    ctx->next_func_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "f%d", ctx->next_func_id);
    ctx->funcs[ctx->func_count] = func;
    ctx->func_ids[ctx->func_count] = strdup(buf);
    ctx->func_state[ctx->func_count] = 0;
    if (state) *state = 0;
    ctx->func_count++;
    return ctx->func_ids[ctx->func_count - 1];
}

static const char* ser_thr_id(SerCtx* ctx, Thr* thr) {
    for (size_t i = 0; i < ctx->thr_count; i++) {
        if (ctx->thrs[i] == thr) {
            return ctx->thr_ids[i];
        }
    }
    if (ctx->thr_count + 1 > ctx->thr_cap) {
        size_t new_cap = ctx->thr_cap == 0 ? 4 : ctx->thr_cap * 2;
        ctx->thrs = realloc(ctx->thrs, new_cap * sizeof(Thr*));
        ctx->thr_ids = realloc(ctx->thr_ids, new_cap * sizeof(char*));
        if (!ctx->thrs || !ctx->thr_ids) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->thr_cap = new_cap;
    }
    ctx->next_thr_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d", ctx->next_thr_id);
    ctx->thrs[ctx->thr_count] = thr;
    ctx->thr_ids[ctx->thr_count] = strdup(buf);
    ctx->thr_count++;
    return ctx->thr_ids[ctx->thr_count - 1];
}

static void json_obj_field(JsonBuf* jb, bool* first, const char* key) {
    if (!*first) jb_append_char(jb, ',');
    *first = false;
    jb_append_json_string(jb, key);
    jb_append_char(jb, ':');
}

static void ser_loc(JsonBuf* jb, int line, int col) {
    jb_append_char(jb, '{');
    bool first = true;
    json_obj_field(jb, &first, "file");
    jb_append_json_string(jb, "<unknown>");
    json_obj_field(jb, &first, "line");
    jb_append_fmt(jb, "%d", line > 0 ? line : 1);
    json_obj_field(jb, &first, "column");
    jb_append_fmt(jb, "%d", col > 0 ? col : 1);
    json_obj_field(jb, &first, "statement");
    jb_append_json_string(jb, "");
    jb_append_char(jb, '}');
}

static void ser_expr(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Expr* expr);
static void ser_stmt(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Stmt* stmt);
static void ser_value(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Value v);

static void ser_env(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Env* env) {
    if (!env) {
        jb_append_str(jb, "null");
        return;
    }
    int state = 0;
    const char* env_id = ser_env_id(ctx, env, &state);
    if (state == 1 || state == 2) {
        jb_append_char(jb, '{');
        bool first = true;
        json_obj_field(jb, &first, "t");
        jb_append_json_string(jb, "ENV");
        json_obj_field(jb, &first, "id");
        jb_append_json_string(jb, env_id);
        json_obj_field(jb, &first, "ref");
        jb_append_str(jb, "true");
        jb_append_char(jb, '}');
        return;
    }

    for (size_t i = 0; i < ctx->env_count; i++) {
        if (ctx->envs[i] == env) { ctx->env_state[i] = 1; break; }
    }

    jb_append_char(jb, '{');
    bool first = true;
    json_obj_field(jb, &first, "t");
    jb_append_json_string(jb, "ENV");
    json_obj_field(jb, &first, "id");
    jb_append_json_string(jb, env_id);
    json_obj_field(jb, &first, "def");

    jb_append_char(jb, '{');
    bool def_first = true;

    json_obj_field(jb, &def_first, "values");
    jb_append_char(jb, '{');
    bool val_first = true;
    for (size_t i = 0; i < env->count; i++) {
        EnvEntry* entry = &env->entries[i];
        if (!entry->initialized && !entry->alias_target) continue;
        if (!val_first) jb_append_char(jb, ',');
        val_first = false;
        jb_append_json_string(jb, entry->name);
        jb_append_char(jb, ':');
        if (entry->alias_target) {
            jb_append_char(jb, '{');
            bool pf = true;
            json_obj_field(jb, &pf, "t");
            jb_append_json_string(jb, "PTR");
            json_obj_field(jb, &pf, "name");
            jb_append_json_string(jb, entry->alias_target);
            json_obj_field(jb, &pf, "env");
            Env* owner = env_find_owner(env, entry->alias_target);
            ser_env(jb, ctx, interp, owner ? owner : env);
            json_obj_field(jb, &pf, "value_type");
            jb_append_json_string(jb, decl_type_name(entry->decl_type));
            jb_append_char(jb, '}');
        } else {
            ser_value(jb, ctx, interp, entry->value);
        }
    }
    jb_append_char(jb, '}');

    json_obj_field(jb, &def_first, "declared");
    jb_append_char(jb, '{');
    bool dec_first = true;
    for (size_t i = 0; i < env->count; i++) {
        EnvEntry* entry = &env->entries[i];
        if (entry->decl_type == TYPE_UNKNOWN) continue;
        if (!dec_first) jb_append_char(jb, ',');
        dec_first = false;
        jb_append_json_string(jb, entry->name);
        jb_append_char(jb, ':');
        jb_append_json_string(jb, decl_type_name(entry->decl_type));
    }
    jb_append_char(jb, '}');

    json_obj_field(jb, &def_first, "frozen");
    jb_append_char(jb, '[');
    bool fr_first = true;
    for (size_t i = 0; i < env->count; i++) {
        if (!env->entries[i].frozen) continue;
        if (!fr_first) jb_append_char(jb, ',');
        fr_first = false;
        jb_append_json_string(jb, env->entries[i].name);
    }
    jb_append_char(jb, ']');

    json_obj_field(jb, &def_first, "permafrozen");
    jb_append_char(jb, '[');
    bool pf_first = true;
    for (size_t i = 0; i < env->count; i++) {
        if (!env->entries[i].permafrozen) continue;
        if (!pf_first) jb_append_char(jb, ',');
        pf_first = false;
        jb_append_json_string(jb, env->entries[i].name);
    }
    jb_append_char(jb, ']');

    json_obj_field(jb, &def_first, "parent");
    ser_env(jb, ctx, interp, env->parent);

    jb_append_char(jb, '}');
    jb_append_char(jb, '}');

    for (size_t i = 0; i < ctx->env_count; i++) {
        if (ctx->envs[i] == env) { ctx->env_state[i] = 2; break; }
    }
}

static void ser_expr(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Expr* expr) {
    if (!expr) {
        jb_append_str(jb, "null");
        return;
    }
    switch (expr->type) {
        case EXPR_INT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Literal");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "value");
            jb_append_fmt(jb, "%lld", (long long)expr->as.int_value);
            json_obj_field(jb, &first, "literal_type");
            jb_append_json_string(jb, "INT");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_FLT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Literal");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "value");
            if (isnan(expr->as.flt_value)) {
                jb_append_json_string(jb, "NaN");
            } else if (isinf(expr->as.flt_value)) {
                if (signbit(expr->as.flt_value)) jb_append_json_string(jb, "-INF");
                else jb_append_json_string(jb, "INF");
            } else {
                jb_append_fmt(jb, "%.17g", expr->as.flt_value);
            }
            json_obj_field(jb, &first, "literal_type");
            jb_append_json_string(jb, "FLT");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_STR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Literal");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "value");
            jb_append_json_string(jb, expr->as.str_value ? expr->as.str_value : "");
            json_obj_field(jb, &first, "literal_type");
            jb_append_json_string(jb, "STR");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_TNS: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "TensorLiteral");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "items");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < expr->as.tns_items.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                ser_expr(jb, ctx, interp, expr->as.tns_items.items[i]);
            }
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_MAP: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "MapLiteral");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "items");
            jb_append_char(jb, '[');
            size_t count = expr->as.map_items.keys.count;
            for (size_t i = 0; i < count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                jb_append_char(jb, '{');
                bool ifirst = true;
                json_obj_field(jb, &ifirst, "k");
                ser_expr(jb, ctx, interp, expr->as.map_items.keys.items[i]);
                json_obj_field(jb, &ifirst, "v");
                ser_expr(jb, ctx, interp, expr->as.map_items.values.items[i]);
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_IDENT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Identifier");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "name");
            jb_append_json_string(jb, expr->as.ident ? expr->as.ident : "");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_PTR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "PointerExpression");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "target");
            jb_append_json_string(jb, expr->as.ptr_name ? expr->as.ptr_name : "");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_CALL: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "CallExpression");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "callee");
            ser_expr(jb, ctx, interp, expr->as.call.callee);
            json_obj_field(jb, &first, "args");
            jb_append_char(jb, '[');
            size_t pos_count = expr->as.call.args.count;
            size_t kw_count = expr->as.call.kw_count;
            size_t total = pos_count + kw_count;
            size_t idx = 0;
            for (size_t i = 0; i < pos_count; i++, idx++) {
                if (idx > 0) jb_append_char(jb, ',');
                jb_append_char(jb, '{');
                bool afirst = true;
                json_obj_field(jb, &afirst, "n");
                jb_append_json_string(jb, "CallArgument");
                json_obj_field(jb, &afirst, "name");
                jb_append_str(jb, "null");
                json_obj_field(jb, &afirst, "expression");
                ser_expr(jb, ctx, interp, expr->as.call.args.items[i]);
                jb_append_char(jb, '}');
            }
            for (size_t i = 0; i < kw_count; i++, idx++) {
                if (idx > 0) jb_append_char(jb, ',');
                jb_append_char(jb, '{');
                bool afirst = true;
                json_obj_field(jb, &afirst, "n");
                jb_append_json_string(jb, "CallArgument");
                json_obj_field(jb, &afirst, "name");
                jb_append_json_string(jb, expr->as.call.kw_names[i]);
                json_obj_field(jb, &afirst, "expression");
                ser_expr(jb, ctx, interp, expr->as.call.kw_args.items[i]);
                jb_append_char(jb, '}');
            }
            (void)total;
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_ASYNC: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "AsyncExpression");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, expr->as.async.block);
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_INDEX: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "IndexExpression");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "base");
            ser_expr(jb, ctx, interp, expr->as.index.target);
            json_obj_field(jb, &first, "indices");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < expr->as.index.indices.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                ser_expr(jb, ctx, interp, expr->as.index.indices.items[i]);
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &first, "is_map");
            jb_append_str(jb, "false");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_RANGE: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Range");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "lo");
            ser_expr(jb, ctx, interp, expr->as.range.start);
            json_obj_field(jb, &first, "start");
            ser_expr(jb, ctx, interp, expr->as.range.end);
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_WILDCARD: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Star");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            jb_append_char(jb, '}');
            return;
        }
        default:
            jb_append_str(jb, "null");
            return;
    }
}

static void ser_stmt(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Stmt* stmt) {
    if (!stmt) {
        jb_append_str(jb, "null");
        return;
    }
    switch (stmt->type) {
        case STMT_BLOCK: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Block");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "statements");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < stmt->as.block.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                ser_stmt(jb, ctx, interp, stmt->as.block.items[i]);
            }
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case STMT_ASSIGN: {
            if (stmt->as.assign.target) {
                jb_append_char(jb, '{');
                bool first = true;
                json_obj_field(jb, &first, "n");
                jb_append_json_string(jb, "TensorSetStatement");
                json_obj_field(jb, &first, "loc");
                ser_loc(jb, stmt->line, stmt->column);
                json_obj_field(jb, &first, "target");
                ser_expr(jb, ctx, interp, stmt->as.assign.target);
                json_obj_field(jb, &first, "value");
                ser_expr(jb, ctx, interp, stmt->as.assign.value);
                jb_append_char(jb, '}');
                return;
            }
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Assignment");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "target");
            jb_append_json_string(jb, stmt->as.assign.name ? stmt->as.assign.name : "");
            json_obj_field(jb, &first, "declared_type");
            if (stmt->as.assign.has_type) {
                jb_append_json_string(jb, decl_type_name(stmt->as.assign.decl_type));
            } else {
                jb_append_str(jb, "null");
            }
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.assign.value);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_DECL: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Declaration");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "name");
            jb_append_json_string(jb, stmt->as.decl.name ? stmt->as.decl.name : "");
            json_obj_field(jb, &first, "declared_type");
            jb_append_json_string(jb, decl_type_name(stmt->as.decl.decl_type));
            jb_append_char(jb, '}');
            return;
        }
        case STMT_EXPR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ExpressionStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.expr_stmt.expr);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_IF: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "IfStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "condition");
            ser_expr(jb, ctx, interp, stmt->as.if_stmt.condition);
            json_obj_field(jb, &first, "then_block");
            ser_stmt(jb, ctx, interp, stmt->as.if_stmt.then_branch);
            json_obj_field(jb, &first, "elifs");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < stmt->as.if_stmt.elif_conditions.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                jb_append_char(jb, '{');
                bool ef = true;
                json_obj_field(jb, &ef, "n");
                jb_append_json_string(jb, "IfBranch");
                json_obj_field(jb, &ef, "condition");
                ser_expr(jb, ctx, interp, stmt->as.if_stmt.elif_conditions.items[i]);
                json_obj_field(jb, &ef, "block");
                ser_stmt(jb, ctx, interp, stmt->as.if_stmt.elif_blocks.items[i]);
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &first, "else_block");
            if (stmt->as.if_stmt.else_branch) {
                ser_stmt(jb, ctx, interp, stmt->as.if_stmt.else_branch);
            } else {
                jb_append_str(jb, "null");
            }
            jb_append_char(jb, '}');
            return;
        }
        case STMT_WHILE: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "WhileStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "condition");
            ser_expr(jb, ctx, interp, stmt->as.while_stmt.condition);
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, stmt->as.while_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_FOR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ForStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "counter");
            jb_append_json_string(jb, stmt->as.for_stmt.counter ? stmt->as.for_stmt.counter : "");
            json_obj_field(jb, &first, "target_expr");
            ser_expr(jb, ctx, interp, stmt->as.for_stmt.target);
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, stmt->as.for_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_PARFOR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ParForStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "counter");
            jb_append_json_string(jb, stmt->as.parfor_stmt.counter ? stmt->as.parfor_stmt.counter : "");
            json_obj_field(jb, &first, "target_expr");
            ser_expr(jb, ctx, interp, stmt->as.parfor_stmt.target);
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, stmt->as.parfor_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_FUNC: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "FuncDef");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "name");
            jb_append_json_string(jb, stmt->as.func_stmt.name ? stmt->as.func_stmt.name : "");
            json_obj_field(jb, &first, "params");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < stmt->as.func_stmt.params.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                Param* p = &stmt->as.func_stmt.params.items[i];
                jb_append_char(jb, '{');
                bool pf = true;
                json_obj_field(jb, &pf, "n");
                jb_append_json_string(jb, "Param");
                json_obj_field(jb, &pf, "type");
                jb_append_json_string(jb, decl_type_name(p->type));
                json_obj_field(jb, &pf, "name");
                jb_append_json_string(jb, p->name ? p->name : "");
                json_obj_field(jb, &pf, "default");
                if (p->default_value) ser_expr(jb, ctx, interp, p->default_value);
                else jb_append_str(jb, "null");
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &first, "return_type");
            jb_append_json_string(jb, decl_type_name(stmt->as.func_stmt.return_type));
            json_obj_field(jb, &first, "body");
            ser_stmt(jb, ctx, interp, stmt->as.func_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_RETURN: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ReturnStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.return_stmt.value);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_POP: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "PopStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            jb_append_char(jb, '{');
            bool ef = true;
            json_obj_field(jb, &ef, "n");
            jb_append_json_string(jb, "Identifier");
            json_obj_field(jb, &ef, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &ef, "name");
            jb_append_json_string(jb, stmt->as.pop_stmt.name ? stmt->as.pop_stmt.name : "");
            jb_append_char(jb, '}');
            jb_append_char(jb, '}');
            return;
        }
        case STMT_BREAK: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "BreakStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.break_stmt.value);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_GOTO: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "GotoStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.goto_stmt.target);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_GOTOPOINT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "GotopointStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "expression");
            ser_expr(jb, ctx, interp, stmt->as.gotopoint_stmt.target);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_CONTINUE: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ContinueStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_ASYNC: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "AsyncStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, stmt->as.async_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_THR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "ThrStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "symbol");
            jb_append_json_string(jb, stmt->as.thr_stmt.name ? stmt->as.thr_stmt.name : "");
            json_obj_field(jb, &first, "block");
            ser_stmt(jb, ctx, interp, stmt->as.thr_stmt.body);
            jb_append_char(jb, '}');
            return;
        }
        case STMT_TRY: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "TryStatement");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, stmt->line, stmt->column);
            json_obj_field(jb, &first, "try_block");
            ser_stmt(jb, ctx, interp, stmt->as.try_stmt.try_block);
            json_obj_field(jb, &first, "catch_symbol");
            if (stmt->as.try_stmt.catch_name) jb_append_json_string(jb, stmt->as.try_stmt.catch_name);
            else jb_append_str(jb, "null");
            json_obj_field(jb, &first, "catch_block");
            ser_stmt(jb, ctx, interp, stmt->as.try_stmt.catch_block);
            jb_append_char(jb, '}');
            return;
        }
        default:
            jb_append_str(jb, "null");
            return;
    }
}

static void ser_value(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Value v) {
    switch (v.type) {
        case VAL_INT: {
            char* s = int_to_binary_str(v.as.i);
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "INT");
            json_obj_field(jb, &first, "v");
            jb_append_json_string(jb, s);
            jb_append_char(jb, '}');
            free(s);
            return;
        }
        case VAL_FLT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "FLT");
            json_obj_field(jb, &first, "v");
            if (isnan(v.as.f)) {
                jb_append_json_string(jb, "NaN");
            } else if (isinf(v.as.f)) {
                if (signbit(v.as.f)) jb_append_json_string(jb, "-INF");
                else jb_append_json_string(jb, "INF");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", v.as.f);
                jb_append_json_string(jb, buf);
            }
            jb_append_char(jb, '}');
            return;
        }
        case VAL_STR: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "STR");
            json_obj_field(jb, &first, "v");
            jb_append_json_string(jb, v.as.s ? v.as.s : "");
            jb_append_char(jb, '}');
            return;
        }
        case VAL_TNS: {
            Tensor* t = v.as.tns;
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "TNS");
            json_obj_field(jb, &first, "shape");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < t->ndim; i++) {
                if (i > 0) jb_append_char(jb, ',');
                jb_append_fmt(jb, "%zu", t->shape[i]);
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &first, "v");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < t->length; i++) {
                if (i > 0) jb_append_char(jb, ',');
                ser_value(jb, ctx, interp, t->data[i]);
            }
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case VAL_MAP: {
            Map* m = v.as.map;
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "MAP");
            json_obj_field(jb, &first, "v");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < m->count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                jb_append_char(jb, '{');
                bool pf = true;
                json_obj_field(jb, &pf, "k");
                ser_value(jb, ctx, interp, m->items[i].key);
                json_obj_field(jb, &pf, "v");
                ser_value(jb, ctx, interp, m->items[i].value);
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            jb_append_char(jb, '}');
            return;
        }
        case VAL_FUNC: {
            Func* fn = v.as.func;
            int state = 0;
            const char* id = ser_func_id(ctx, fn, &state);
            if (state == 1) {
                jb_append_char(jb, '{');
                bool first = true;
                json_obj_field(jb, &first, "t");
                jb_append_json_string(jb, "FUNC");
                json_obj_field(jb, &first, "id");
                jb_append_json_string(jb, id);
                json_obj_field(jb, &first, "ref");
                jb_append_str(jb, "true");
                jb_append_char(jb, '}');
                return;
            }
            for (size_t i = 0; i < ctx->func_count; i++) {
                if (ctx->funcs[i] == fn) { ctx->func_state[i] = 1; break; }
            }
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "FUNC");
            json_obj_field(jb, &first, "id");
            jb_append_json_string(jb, id);
            json_obj_field(jb, &first, "name");
            jb_append_json_string(jb, fn->name ? fn->name : "<anon>");
            json_obj_field(jb, &first, "return");
            jb_append_json_string(jb, decl_type_name(fn->return_type));
            json_obj_field(jb, &first, "params");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < fn->params.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                Param* p = &fn->params.items[i];
                jb_append_char(jb, '{');
                bool pf = true;
                json_obj_field(jb, &pf, "name");
                jb_append_json_string(jb, p->name ? p->name : "");
                json_obj_field(jb, &pf, "type");
                jb_append_json_string(jb, decl_type_name(p->type));
                json_obj_field(jb, &pf, "default");
                if (p->default_value) ser_expr(jb, ctx, interp, p->default_value);
                else jb_append_str(jb, "null");
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &first, "def");
            jb_append_char(jb, '{');
            bool df = true;
            json_obj_field(jb, &df, "name");
            jb_append_json_string(jb, fn->name ? fn->name : "<anon>");
            json_obj_field(jb, &df, "return");
            jb_append_json_string(jb, decl_type_name(fn->return_type));
            json_obj_field(jb, &df, "params");
            jb_append_char(jb, '[');
            for (size_t i = 0; i < fn->params.count; i++) {
                if (i > 0) jb_append_char(jb, ',');
                Param* p = &fn->params.items[i];
                jb_append_char(jb, '{');
                bool pf = true;
                json_obj_field(jb, &pf, "name");
                jb_append_json_string(jb, p->name ? p->name : "");
                json_obj_field(jb, &pf, "type");
                jb_append_json_string(jb, decl_type_name(p->type));
                json_obj_field(jb, &pf, "default");
                if (p->default_value) ser_expr(jb, ctx, interp, p->default_value);
                else jb_append_str(jb, "null");
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &df, "body");
            ser_stmt(jb, ctx, interp, fn->body);
            json_obj_field(jb, &df, "closure");
            ser_env(jb, ctx, interp, fn->closure);
            jb_append_char(jb, '}');
            jb_append_char(jb, '}');
            for (size_t i = 0; i < ctx->func_count; i++) {
                if (ctx->funcs[i] == fn) { ctx->func_state[i] = 2; break; }
            }
            return;
        }
        case VAL_THR: {
            Thr* th = v.as.thr;
            Value thv = value_null();
            thv.type = VAL_THR;
            thv.as.thr = th;
            const char* id = ser_thr_id(ctx, th);
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "THR");
            json_obj_field(jb, &first, "id");
            jb_append_json_string(jb, id);
            json_obj_field(jb, &first, "state");
            if (value_thr_get_finished(thv)) jb_append_json_string(jb, "finished");
            else if (value_thr_get_paused(thv)) jb_append_json_string(jb, "paused");
            else jb_append_json_string(jb, "running");
            json_obj_field(jb, &first, "paused");
            jb_append_str(jb, value_thr_get_paused(thv) ? "true" : "false");
            json_obj_field(jb, &first, "finished");
            jb_append_str(jb, value_thr_get_finished(thv) ? "true" : "false");
            json_obj_field(jb, &first, "stop");
            jb_append_str(jb, value_thr_get_finished(thv) ? "true" : "false");
            json_obj_field(jb, &first, "env");
            ser_env(jb, ctx, interp, th->env);
            json_obj_field(jb, &first, "block");
            if (th->body) ser_stmt(jb, ctx, interp, th->body);
            else jb_append_str(jb, "null");
            jb_append_char(jb, '}');
            return;
        }
        default:
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, value_type_name(v));
            json_obj_field(jb, &first, "repr");
            jb_append_json_string(jb, "<unsupported>");
            jb_append_char(jb, '}');
            return;
    }
}

// ---- DESERIALIZATION ----
typedef struct {
    char** ids;
    Env** envs;
    size_t count;
    size_t cap;
} EnvRegistry;

typedef struct {
    char** ids;
    Func** funcs;
    size_t count;
    size_t cap;
} FuncRegistry;

typedef struct {
    char** ids;
    Thr** thrs;
    size_t count;
    size_t cap;
} ThrRegistry;

typedef struct {
    EnvRegistry envs;
    FuncRegistry funcs;
    ThrRegistry thrs;
} UnserCtx;

static void unser_ctx_init(UnserCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static void unser_ctx_free(UnserCtx* ctx) {
    for (size_t i = 0; i < ctx->envs.count; i++) free(ctx->envs.ids[i]);
    for (size_t i = 0; i < ctx->funcs.count; i++) free(ctx->funcs.ids[i]);
    for (size_t i = 0; i < ctx->thrs.count; i++) free(ctx->thrs.ids[i]);
    free(ctx->envs.ids);
    free(ctx->envs.envs);
    free(ctx->funcs.ids);
    free(ctx->funcs.funcs);
    free(ctx->thrs.ids);
    free(ctx->thrs.thrs);
}

static Env* unser_env_get(UnserCtx* ctx, const char* id) {
    for (size_t i = 0; i < ctx->envs.count; i++) {
        if (strcmp(ctx->envs.ids[i], id) == 0) return ctx->envs.envs[i];
    }
    return NULL;
}

static void unser_env_set(UnserCtx* ctx, const char* id, Env* env) {
    if (ctx->envs.count + 1 > ctx->envs.cap) {
        size_t new_cap = ctx->envs.cap == 0 ? 4 : ctx->envs.cap * 2;
        ctx->envs.ids = realloc(ctx->envs.ids, new_cap * sizeof(char*));
        ctx->envs.envs = realloc(ctx->envs.envs, new_cap * sizeof(Env*));
        if (!ctx->envs.ids || !ctx->envs.envs) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->envs.cap = new_cap;
    }
    ctx->envs.ids[ctx->envs.count] = strdup(id);
    ctx->envs.envs[ctx->envs.count] = env;
    ctx->envs.count++;
}

static Func* unser_func_get(UnserCtx* ctx, const char* id) {
    for (size_t i = 0; i < ctx->funcs.count; i++) {
        if (strcmp(ctx->funcs.ids[i], id) == 0) return ctx->funcs.funcs[i];
    }
    return NULL;
}

static void unser_func_set(UnserCtx* ctx, const char* id, Func* func) {
    if (ctx->funcs.count + 1 > ctx->funcs.cap) {
        size_t new_cap = ctx->funcs.cap == 0 ? 4 : ctx->funcs.cap * 2;
        ctx->funcs.ids = realloc(ctx->funcs.ids, new_cap * sizeof(char*));
        ctx->funcs.funcs = realloc(ctx->funcs.funcs, new_cap * sizeof(Func*));
        if (!ctx->funcs.ids || !ctx->funcs.funcs) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->funcs.cap = new_cap;
    }
    ctx->funcs.ids[ctx->funcs.count] = strdup(id);
    ctx->funcs.funcs[ctx->funcs.count] = func;
    ctx->funcs.count++;
}

static Thr* unser_thr_get(UnserCtx* ctx, const char* id) {
    for (size_t i = 0; i < ctx->thrs.count; i++) {
        if (strcmp(ctx->thrs.ids[i], id) == 0) return ctx->thrs.thrs[i];
    }
    return NULL;
}

static void unser_thr_set(UnserCtx* ctx, const char* id, Thr* thr) {
    if (ctx->thrs.count + 1 > ctx->thrs.cap) {
        size_t new_cap = ctx->thrs.cap == 0 ? 4 : ctx->thrs.cap * 2;
        ctx->thrs.ids = realloc(ctx->thrs.ids, new_cap * sizeof(char*));
        ctx->thrs.thrs = realloc(ctx->thrs.thrs, new_cap * sizeof(Thr*));
        if (!ctx->thrs.ids || !ctx->thrs.thrs) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->thrs.cap = new_cap;
    }
    ctx->thrs.ids[ctx->thrs.count] = strdup(id);
    ctx->thrs.thrs[ctx->thrs.count] = thr;
    ctx->thrs.count++;
}

static Expr* deser_expr(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err);
static Stmt* deser_stmt(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err);
static Env* deser_env(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err);
static Value deser_val(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err);

static int json_num_to_int(JsonValue* v, int default_val) {
    if (!v || v->type != JSON_NUM) return default_val;
    return (int)v->as.num;
}

static Expr* deser_default_expr(JsonValue* raw, UnserCtx* ctx, Interpreter* interp, const char** err) {
    if (!raw || raw->type == JSON_NULL) return NULL;
    if (raw->type == JSON_OBJ) {
        JsonValue* n = json_obj_get(raw, "n");
        if (n && n->type == JSON_STR) {
            return deser_expr(raw, ctx, interp, err);
        }
    }
    Value v = deser_val(raw, ctx, interp, err);
    if (*err) return NULL;
    if (v.type == VAL_INT) return expr_int(v.as.i, 1, 1);
    if (v.type == VAL_FLT) return expr_flt(v.as.f, 1, 1);
    if (v.type == VAL_STR) return expr_str(strdup(v.as.s ? v.as.s : ""), 1, 1);
    return NULL;
}

static Expr* deser_expr(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    JsonValue* n = json_obj_get(obj, "n");
    if (!n || n->type != JSON_STR) return NULL;
    const char* name = n->as.str;
    int line = json_num_to_int(json_obj_get(json_obj_get(obj, "loc"), "line"), 1);
    int col = json_num_to_int(json_obj_get(json_obj_get(obj, "loc"), "column"), 1);

    if (strcmp(name, "Literal") == 0) {
        JsonValue* lit_type = json_obj_get(obj, "literal_type");
        JsonValue* val = json_obj_get(obj, "value");
        const char* lt = (lit_type && lit_type->type == JSON_STR) ? lit_type->as.str : "INT";
        if (strcmp(lt, "INT") == 0) {
            int64_t i = 0;
            if (val && val->type == JSON_NUM) i = (int64_t)val->as.num;
            return expr_int(i, line, col);
        }
        if (strcmp(lt, "FLT") == 0) {
            double f = 0.0;
            if (val && val->type == JSON_NUM) f = val->as.num;
            else if (val && val->type == JSON_STR) f = strtod(val->as.str, NULL);
            return expr_flt(f, line, col);
        }
        if (strcmp(lt, "STR") == 0) {
            const char* s = (val && val->type == JSON_STR) ? val->as.str : "";
            return expr_str(strdup(s), line, col);
        }
        return expr_int(0, line, col);
    }
    if (strcmp(name, "TensorLiteral") == 0) {
        Expr* t = expr_tns(line, col);
        JsonValue* items = json_obj_get(obj, "items");
        if (items && items->type == JSON_ARR) {
            for (size_t i = 0; i < items->as.arr.count; i++) {
                Expr* it = deser_expr(items->as.arr.items[i], ctx, interp, err);
                if (*err) return t;
                if (it) expr_list_add(&t->as.tns_items, it);
            }
        }
        return t;
    }
    if (strcmp(name, "MapLiteral") == 0) {
        Expr* m = expr_map(line, col);
        JsonValue* items = json_obj_get(obj, "items");
        if (items && items->type == JSON_ARR) {
            for (size_t i = 0; i < items->as.arr.count; i++) {
                JsonValue* pair = items->as.arr.items[i];
                if (!pair || pair->type != JSON_OBJ) continue;
                Expr* k = deser_expr(json_obj_get(pair, "k"), ctx, interp, err);
                Expr* v = deser_expr(json_obj_get(pair, "v"), ctx, interp, err);
                if (*err) return m;
                if (k && v) {
                    expr_list_add(&m->as.map_items.keys, k);
                    expr_list_add(&m->as.map_items.values, v);
                }
            }
        }
        return m;
    }
    if (strcmp(name, "Identifier") == 0) {
        JsonValue* nm = json_obj_get(obj, "name");
        const char* s = (nm && nm->type == JSON_STR) ? nm->as.str : "";
        return expr_ident(strdup(s), line, col);
    }
    if (strcmp(name, "PointerExpression") == 0) {
        JsonValue* nm = json_obj_get(obj, "target");
        const char* s = (nm && nm->type == JSON_STR) ? nm->as.str : "";
        return expr_ptr(strdup(s), line, col);
    }
    if (strcmp(name, "CallExpression") == 0) {
        Expr* callee = deser_expr(json_obj_get(obj, "callee"), ctx, interp, err);
        Expr* call = expr_call(callee, line, col);
        JsonValue* args = json_obj_get(obj, "args");
        if (args && args->type == JSON_ARR) {
            for (size_t i = 0; i < args->as.arr.count; i++) {
                JsonValue* a = args->as.arr.items[i];
                if (!a || a->type != JSON_OBJ) continue;
                JsonValue* nm = json_obj_get(a, "name");
                JsonValue* ex = json_obj_get(a, "expression");
                Expr* arg = deser_expr(ex, ctx, interp, err);
                if (*err) return call;
                if (nm && nm->type == JSON_STR && nm->as.str && nm->as.str[0]) {
                    call_kw_add(call, strdup(nm->as.str), arg);
                } else {
                    expr_list_add(&call->as.call.args, arg);
                }
            }
        }
        return call;
    }
    if (strcmp(name, "AsyncExpression") == 0) {
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        return expr_async(block, line, col);
    }
    if (strcmp(name, "IndexExpression") == 0) {
        Expr* base = deser_expr(json_obj_get(obj, "base"), ctx, interp, err);
        Expr* idx = expr_index(base, line, col);
        JsonValue* indices = json_obj_get(obj, "indices");
        if (indices && indices->type == JSON_ARR) {
            for (size_t i = 0; i < indices->as.arr.count; i++) {
                Expr* it = deser_expr(indices->as.arr.items[i], ctx, interp, err);
                if (*err) return idx;
                if (it) expr_list_add(&idx->as.index.indices, it);
            }
        }
        return idx;
    }
    if (strcmp(name, "Range") == 0) {
        Expr* lo = deser_expr(json_obj_get(obj, "lo"), ctx, interp, err);
        Expr* start = deser_expr(json_obj_get(obj, "start"), ctx, interp, err);
        return expr_range(lo, start, line, col);
    }
    if (strcmp(name, "Star") == 0) {
        return expr_wildcard(line, col);
    }
    return NULL;
}

static Stmt* deser_stmt(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    JsonValue* n = json_obj_get(obj, "n");
    if (!n || n->type != JSON_STR) return NULL;
    const char* name = n->as.str;
    int line = json_num_to_int(json_obj_get(json_obj_get(obj, "loc"), "line"), 1);
    int col = json_num_to_int(json_obj_get(json_obj_get(obj, "loc"), "column"), 1);

    if (strcmp(name, "Block") == 0) {
        Stmt* b = stmt_block(line, col);
        JsonValue* stmts = json_obj_get(obj, "statements");
        if (stmts && stmts->type == JSON_ARR) {
            for (size_t i = 0; i < stmts->as.arr.count; i++) {
                Stmt* s = deser_stmt(stmts->as.arr.items[i], ctx, interp, err);
                if (*err) return b;
                if (s) stmt_list_add(&b->as.block, s);
            }
        }
        return b;
    }
    if (strcmp(name, "Assignment") == 0) {
        JsonValue* tgt = json_obj_get(obj, "target");
        JsonValue* dt = json_obj_get(obj, "declared_type");
        JsonValue* expr = json_obj_get(obj, "expression");
        const char* tname = (tgt && tgt->type == JSON_STR) ? tgt->as.str : "";
        DeclType dtype = TYPE_UNKNOWN;
        bool has_type = false;
        if (dt && dt->type == JSON_STR) {
            dtype = decl_type_from_name(dt->as.str);
            has_type = true;
        }
        Expr* ex = deser_expr(expr, ctx, interp, err);
        return stmt_assign(has_type, dtype, strdup(tname), NULL, ex, line, col);
    }
    if (strcmp(name, "Declaration") == 0) {
        JsonValue* nm = json_obj_get(obj, "name");
        JsonValue* dt = json_obj_get(obj, "declared_type");
        const char* nms = (nm && nm->type == JSON_STR) ? nm->as.str : "";
        DeclType dtype = decl_type_from_name((dt && dt->type == JSON_STR) ? dt->as.str : NULL);
        return stmt_decl(dtype, strdup(nms), line, col);
    }
    if (strcmp(name, "ExpressionStatement") == 0) {
        Expr* ex = deser_expr(json_obj_get(obj, "expression"), ctx, interp, err);
        return stmt_expr(ex, line, col);
    }
    if (strcmp(name, "IfStatement") == 0) {
        Expr* cond = deser_expr(json_obj_get(obj, "condition"), ctx, interp, err);
        Stmt* then_block = deser_stmt(json_obj_get(obj, "then_block"), ctx, interp, err);
        Stmt* st = stmt_if(cond, then_block, line, col);
        JsonValue* elifs = json_obj_get(obj, "elifs");
        if (elifs && elifs->type == JSON_ARR) {
            for (size_t i = 0; i < elifs->as.arr.count; i++) {
                JsonValue* br = elifs->as.arr.items[i];
                if (!br || br->type != JSON_OBJ) continue;
                Expr* econd = deser_expr(json_obj_get(br, "condition"), ctx, interp, err);
                Stmt* eblk = deser_stmt(json_obj_get(br, "block"), ctx, interp, err);
                if (econd && eblk) {
                    expr_list_add(&st->as.if_stmt.elif_conditions, econd);
                    stmt_list_add(&st->as.if_stmt.elif_blocks, eblk);
                }
            }
        }
        JsonValue* else_block = json_obj_get(obj, "else_block");
        if (else_block && else_block->type != JSON_NULL) {
            st->as.if_stmt.else_branch = deser_stmt(else_block, ctx, interp, err);
        }
        return st;
    }
    if (strcmp(name, "WhileStatement") == 0) {
        Expr* cond = deser_expr(json_obj_get(obj, "condition"), ctx, interp, err);
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        return stmt_while(cond, block, line, col);
    }
    if (strcmp(name, "ForStatement") == 0) {
        JsonValue* counter = json_obj_get(obj, "counter");
        Expr* target = deser_expr(json_obj_get(obj, "target_expr"), ctx, interp, err);
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        const char* cnt = (counter && counter->type == JSON_STR) ? counter->as.str : "";
        return stmt_for(strdup(cnt), target, block, line, col);
    }
    if (strcmp(name, "ParForStatement") == 0) {
        JsonValue* counter = json_obj_get(obj, "counter");
        Expr* target = deser_expr(json_obj_get(obj, "target_expr"), ctx, interp, err);
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        const char* cnt = (counter && counter->type == JSON_STR) ? counter->as.str : "";
        return stmt_parfor(strdup(cnt), target, block, line, col);
    }
    if (strcmp(name, "FuncDef") == 0) {
        JsonValue* nm = json_obj_get(obj, "name");
        JsonValue* params = json_obj_get(obj, "params");
        JsonValue* ret = json_obj_get(obj, "return_type");
        Stmt* body = deser_stmt(json_obj_get(obj, "body"), ctx, interp, err);
        const char* fn = (nm && nm->type == JSON_STR) ? nm->as.str : "";
        DeclType rt = decl_type_from_name((ret && ret->type == JSON_STR) ? ret->as.str : NULL);
        Stmt* st = stmt_func(strdup(fn), rt, body, line, col);
        if (params && params->type == JSON_ARR) {
            for (size_t i = 0; i < params->as.arr.count; i++) {
                JsonValue* p = params->as.arr.items[i];
                if (!p || p->type != JSON_OBJ) continue;
                JsonValue* pname = json_obj_get(p, "name");
                JsonValue* ptype = json_obj_get(p, "type");
                JsonValue* pdef = json_obj_get(p, "default");
                Param pr;
                pr.name = strdup((pname && pname->type == JSON_STR) ? pname->as.str : "");
                pr.type = decl_type_from_name((ptype && ptype->type == JSON_STR) ? ptype->as.str : NULL);
                pr.default_value = deser_default_expr(pdef, ctx, interp, err);
                param_list_add(&st->as.func_stmt.params, pr);
            }
        }
        return st;
    }
    if (strcmp(name, "ReturnStatement") == 0) {
        Expr* ex = deser_expr(json_obj_get(obj, "expression"), ctx, interp, err);
        return stmt_return(ex, line, col);
    }
    if (strcmp(name, "PopStatement") == 0) {
        JsonValue* ex = json_obj_get(obj, "expression");
        if (ex && ex->type == JSON_OBJ) {
            JsonValue* nm = json_obj_get(ex, "name");
            const char* name_s = (nm && nm->type == JSON_STR) ? nm->as.str : "";
            return stmt_pop(strdup(name_s), line, col);
        }
        return stmt_pop(strdup(""), line, col);
    }
    if (strcmp(name, "BreakStatement") == 0) {
        Expr* ex = deser_expr(json_obj_get(obj, "expression"), ctx, interp, err);
        return stmt_break(ex, line, col);
    }
    if (strcmp(name, "GotoStatement") == 0) {
        Expr* ex = deser_expr(json_obj_get(obj, "expression"), ctx, interp, err);
        return stmt_goto(ex, line, col);
    }
    if (strcmp(name, "GotopointStatement") == 0) {
        Expr* ex = deser_expr(json_obj_get(obj, "expression"), ctx, interp, err);
        return stmt_gotopoint(ex, line, col);
    }
    if (strcmp(name, "ContinueStatement") == 0) {
        return stmt_continue(line, col);
    }
    if (strcmp(name, "AsyncStatement") == 0) {
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        return stmt_async(block, line, col);
    }
    if (strcmp(name, "ThrStatement") == 0) {
        JsonValue* sym = json_obj_get(obj, "symbol");
        const char* s = (sym && sym->type == JSON_STR) ? sym->as.str : "";
        Stmt* block = deser_stmt(json_obj_get(obj, "block"), ctx, interp, err);
        return stmt_thr(strdup(s), block, line, col);
    }
    if (strcmp(name, "TryStatement") == 0) {
        Stmt* try_block = deser_stmt(json_obj_get(obj, "try_block"), ctx, interp, err);
        JsonValue* cs = json_obj_get(obj, "catch_symbol");
        const char* s = (cs && cs->type == JSON_STR) ? cs->as.str : NULL;
        Stmt* catch_block = deser_stmt(json_obj_get(obj, "catch_block"), ctx, interp, err);
        return stmt_try(try_block, s ? strdup(s) : NULL, catch_block, line, col);
    }
    if (strcmp(name, "TensorSetStatement") == 0) {
        Expr* target = deser_expr(json_obj_get(obj, "target"), ctx, interp, err);
        Expr* value = deser_expr(json_obj_get(obj, "value"), ctx, interp, err);
        return stmt_assign(false, TYPE_UNKNOWN, NULL, target, value, line, col);
    }
    return NULL;
}

static Env* deser_env(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err) {
    if (!obj || obj->type == JSON_NULL) return NULL;
    if (obj->type != JSON_OBJ) { *err = "UNSER: invalid ENV"; return NULL; }
    JsonValue* t = json_obj_get(obj, "t");
    if (!t || t->type != JSON_STR || strcmp(t->as.str, "ENV") != 0) { *err = "UNSER: invalid ENV"; return NULL; }
    JsonValue* idv = json_obj_get(obj, "id");
    if (!idv || idv->type != JSON_STR) { *err = "UNSER: invalid ENV id"; return NULL; }
    Env* existing = unser_env_get(ctx, idv->as.str);
    if (existing) return existing;

    Env* env = env_create(NULL);
    unser_env_set(ctx, idv->as.str, env);

    JsonValue* ref = json_obj_get(obj, "ref");
    if (ref && ref->type == JSON_BOOL && ref->as.boolean) {
        return env;
    }

    JsonValue* def = json_obj_get(obj, "def");
    if (def && def->type == JSON_OBJ) {
        env->parent = deser_env(json_obj_get(def, "parent"), ctx, interp, err);

        JsonValue* declared = json_obj_get(def, "declared");
        if (declared && declared->type == JSON_OBJ) {
            for (size_t i = 0; i < declared->as.obj.count; i++) {
                JsonPair* p = &declared->as.obj.items[i];
                DeclType dt = decl_type_from_name(p->value && p->value->type == JSON_STR ? p->value->as.str : NULL);
                if (!env_find_local_entry(env, p->key)) env_define(env, p->key, dt);
            }
        }

        JsonValue* values = json_obj_get(def, "values");
        if (values && values->type == JSON_OBJ) {
            for (size_t i = 0; i < values->as.obj.count; i++) {
                JsonPair* p = &values->as.obj.items[i];
                JsonValue* vv = p->value;
                if (!env_find_local_entry(env, p->key)) env_define(env, p->key, TYPE_UNKNOWN);
                EnvEntry* entry = env_find_local_entry(env, p->key);
                if (vv && vv->type == JSON_OBJ) {
                    JsonValue* vt = json_obj_get(vv, "t");
                    if (vt && vt->type == JSON_STR && strcmp(vt->as.str, "PTR") == 0) {
                        JsonValue* pname = json_obj_get(vv, "name");
                        JsonValue* vtype = json_obj_get(vv, "value_type");
                        const char* target = (pname && pname->type == JSON_STR) ? pname->as.str : NULL;
                        if (entry->alias_target) { free(entry->alias_target); entry->alias_target = NULL; }
                        if (target) entry->alias_target = strdup(target);
                        entry->decl_type = decl_type_from_name((vtype && vtype->type == JSON_STR) ? vtype->as.str : NULL);
                        entry->initialized = true;
                        continue;
                    }
                }
                Value val = deser_val(vv, ctx, interp, err);
                if (*err) return env;
                if (entry->initialized) value_free(entry->value);
                entry->value = value_copy(val);
                entry->initialized = true;
            }
        }

        JsonValue* frozen = json_obj_get(def, "frozen");
        if (frozen && frozen->type == JSON_ARR) {
            for (size_t i = 0; i < frozen->as.arr.count; i++) {
                JsonValue* it = frozen->as.arr.items[i];
                if (it && it->type == JSON_STR) {
                    EnvEntry* e = env_find_local_entry(env, it->as.str);
                    if (!e) { env_define(env, it->as.str, TYPE_UNKNOWN); e = env_find_local_entry(env, it->as.str); }
                    if (e) e->frozen = true;
                }
            }
        }

        JsonValue* perma = json_obj_get(def, "permafrozen");
        if (perma && perma->type == JSON_ARR) {
            for (size_t i = 0; i < perma->as.arr.count; i++) {
                JsonValue* it = perma->as.arr.items[i];
                if (it && it->type == JSON_STR) {
                    EnvEntry* e = env_find_local_entry(env, it->as.str);
                    if (!e) { env_define(env, it->as.str, TYPE_UNKNOWN); e = env_find_local_entry(env, it->as.str); }
                    if (e) { e->permafrozen = true; e->frozen = true; }
                }
            }
        }
    }
    return env;
}

static Value deser_val(JsonValue* obj, UnserCtx* ctx, Interpreter* interp, const char** err) {
    if (!obj || obj->type != JSON_OBJ) { *err = "UNSER: invalid serialized form"; return value_null(); }
    JsonValue* t = json_obj_get(obj, "t");
    if (!t || t->type != JSON_STR) { *err = "UNSER: invalid serialized form"; return value_null(); }
    const char* tp = t->as.str;

    if (strcmp(tp, "INT") == 0) {
        JsonValue* v = json_obj_get(obj, "v");
        const char* s = (v && v->type == JSON_STR) ? v->as.str : "0";
        bool neg = s[0] == '-';
        const char* core = neg ? s + 1 : s;
        int64_t val = 0;
        for (const char* p = core; *p; p++) {
            if (*p == '0' || *p == '1') {
                val = (val << 1) | (*p - '0');
            }
        }
        if (neg) val = -val;
        return value_int(val);
    }
    if (strcmp(tp, "FLT") == 0) {
        JsonValue* v = json_obj_get(obj, "v");
        const char* s = (v && v->type == JSON_STR) ? v->as.str : "0.0";
        double f = strtod(s, NULL);
        return value_flt(f);
    }
    if (strcmp(tp, "STR") == 0) {
        JsonValue* v = json_obj_get(obj, "v");
        const char* s = (v && v->type == JSON_STR) ? v->as.str : "";
        return value_str(s);
    }
    if (strcmp(tp, "TNS") == 0) {
        JsonValue* shape = json_obj_get(obj, "shape");
        JsonValue* flat = json_obj_get(obj, "v");
        if (!shape || shape->type != JSON_ARR || !flat || flat->type != JSON_ARR) {
            *err = "UNSER: invalid TNS shape";
            return value_null();
        }
        size_t ndim = shape->as.arr.count;
        size_t* shp = malloc(sizeof(size_t) * (ndim > 0 ? ndim : 1));
        if (!shp) { *err = "Out of memory"; return value_null(); }
        for (size_t i = 0; i < ndim; i++) {
            JsonValue* it = shape->as.arr.items[i];
            size_t sv = (size_t)((it && it->type == JSON_NUM) ? it->as.num : 0);
            shp[i] = sv;
        }
        size_t total = flat->as.arr.count;
        Value* items = malloc(sizeof(Value) * (total > 0 ? total : 1));
        if (!items) { free(shp); *err = "Out of memory"; return value_null(); }
        DeclType elem_type = TYPE_UNKNOWN;
        for (size_t i = 0; i < total; i++) {
            items[i] = deser_val(flat->as.arr.items[i], ctx, interp, err);
            if (*err) { free(shp); free(items); return value_null(); }
            DeclType dt = TYPE_UNKNOWN;
            if (items[i].type == VAL_INT) dt = TYPE_INT;
            else if (items[i].type == VAL_FLT) dt = TYPE_FLT;
            else if (items[i].type == VAL_STR) dt = TYPE_STR;
            else if (items[i].type == VAL_TNS) dt = TYPE_TNS;
            else if (items[i].type == VAL_FUNC) dt = TYPE_FUNC;
            if (i == 0) elem_type = dt;
            else if (elem_type != dt) elem_type = TYPE_UNKNOWN;
        }
        Value out = value_tns_from_values(elem_type, ndim, shp, items, total);
        for (size_t i = 0; i < total; i++) value_free(items[i]);
        free(items);
        free(shp);
        return out;
    }
    if (strcmp(tp, "MAP") == 0) {
        JsonValue* items = json_obj_get(obj, "v");
        if (!items || items->type != JSON_ARR) { *err = "UNSER: invalid MAP form"; return value_null(); }
        Value mv = value_map_new();
        for (size_t i = 0; i < items->as.arr.count; i++) {
            JsonValue* pair = items->as.arr.items[i];
            if (!pair || pair->type != JSON_OBJ) continue;
            Value k = deser_val(json_obj_get(pair, "k"), ctx, interp, err);
            if (*err) { value_free(mv); return value_null(); }
            if (!(k.type == VAL_INT || k.type == VAL_FLT || k.type == VAL_STR)) {
                value_free(k);
                value_free(mv);
                *err = "UNSER: invalid MAP key type";
                return value_null();
            }
            Value v = deser_val(json_obj_get(pair, "v"), ctx, interp, err);
            if (*err) { value_free(k); value_free(mv); return value_null(); }
            value_map_set(&mv, k, v);
            value_free(k);
            value_free(v);
        }
        return mv;
    }
    if (strcmp(tp, "FUNC") == 0) {
        JsonValue* idv = json_obj_get(obj, "id");
        const char* id = (idv && idv->type == JSON_STR) ? idv->as.str : NULL;
        if (id) {
            Func* existing = unser_func_get(ctx, id);
            if (existing) return value_func(existing);
        }
        JsonValue* def = json_obj_get(obj, "def");
        if (def && def->type == JSON_OBJ) {
            JsonValue* nm = json_obj_get(def, "name");
            JsonValue* rt = json_obj_get(def, "return");
            const char* name = (nm && nm->type == JSON_STR) ? nm->as.str : "<unser_func>";
            DeclType ret = decl_type_from_name((rt && rt->type == JSON_STR) ? rt->as.str : NULL);

            Func* fn = malloc(sizeof(Func));
            if (!fn) { *err = "Out of memory"; return value_null(); }
            memset(fn, 0, sizeof(Func));
            fn->name = strdup(name);
            fn->return_type = ret == TYPE_UNKNOWN ? TYPE_INT : ret;
            fn->body = stmt_block(1, 1);
            fn->closure = env_create(NULL);
            if (id) unser_func_set(ctx, id, fn);

            JsonValue* params = json_obj_get(def, "params");
            if (params && params->type == JSON_ARR) {
                for (size_t i = 0; i < params->as.arr.count; i++) {
                    JsonValue* p = params->as.arr.items[i];
                    if (!p || p->type != JSON_OBJ) continue;
                    JsonValue* pn = json_obj_get(p, "name");
                    JsonValue* pt = json_obj_get(p, "type");
                    JsonValue* pd = json_obj_get(p, "default");
                    Param pr;
                    pr.name = strdup((pn && pn->type == JSON_STR) ? pn->as.str : "");
                    pr.type = decl_type_from_name((pt && pt->type == JSON_STR) ? pt->as.str : NULL);
                    pr.default_value = deser_default_expr(pd, ctx, interp, err);
                    param_list_add(&fn->params, pr);
                }
            }

            Stmt* body = deser_stmt(json_obj_get(def, "body"), ctx, interp, err);
            if (body) fn->body = body;
            Env* closure = deser_env(json_obj_get(def, "closure"), ctx, interp, err);
            if (closure) fn->closure = closure;
            return value_func(fn);
        }

        JsonValue* nm = json_obj_get(obj, "name");
        if (nm && nm->type == JSON_STR) {
            Value existing = value_null();
            DeclType dt = TYPE_UNKNOWN;
            bool initialized = false;
            if (interp && interp->global_env && env_get(interp->global_env, nm->as.str, &existing, &dt, &initialized)) {
                if (initialized && existing.type == VAL_FUNC && existing.as.func) {
                    if (id) unser_func_set(ctx, id, existing.as.func);
                    Value ret = value_func(existing.as.func);
                    value_free(existing);
                    return ret;
                }
                value_free(existing);
            }
        }

        const char* nm_s = (nm && nm->type == JSON_STR) ? nm->as.str : "<unser_func>";
        Func* fn = malloc(sizeof(Func));
        if (!fn) { *err = "Out of memory"; return value_null(); }
        memset(fn, 0, sizeof(Func));
        fn->name = strdup(nm_s);
        fn->return_type = TYPE_INT;
        fn->closure = env_create(NULL);

        Stmt* block = stmt_block(1, 1);
        Expr* callee = expr_ident(strdup("THROW"), 1, 1);
        Expr* call = expr_call(callee, 1, 1);
        Expr* arg = expr_str(strdup("UNSER: function not available"), 1, 1);
        expr_list_add(&call->as.call.args, arg);
        Stmt* exprs = stmt_expr(call, 1, 1);
        stmt_list_add(&block->as.block, exprs);
        fn->body = block;
        if (id) unser_func_set(ctx, id, fn);
        return value_func(fn);
    }
    if (strcmp(tp, "THR") == 0) {
        JsonValue* idv = json_obj_get(obj, "id");
        const char* id = (idv && idv->type == JSON_STR) ? idv->as.str : NULL;
        if (id) {
            Thr* existing = unser_thr_get(ctx, id);
            if (existing) {
                Value ret; ret.type = VAL_THR; ret.as.thr = existing;
                return value_copy(ret);
            }
        }
        Value thr = value_thr_new();
        value_thr_set_finished(thr, 1);
        value_thr_set_paused(thr, json_obj_get(obj, "paused") && json_obj_get(obj, "paused")->type == JSON_BOOL ? json_obj_get(obj, "paused")->as.boolean : 0);
        value_thr_set_started(thr, 0);
        thr.as.thr->body = NULL;
        thr.as.thr->env = NULL;
        JsonValue* blk = json_obj_get(obj, "block");
        JsonValue* envv = json_obj_get(obj, "env");
        if (blk && blk->type == JSON_OBJ) thr.as.thr->body = deser_stmt(blk, ctx, interp, err);
        if (envv && envv->type == JSON_OBJ) thr.as.thr->env = deser_env(envv, ctx, interp, err);
        if (id) unser_thr_set(ctx, id, thr.as.thr);
        return thr;
    }

    *err = "UNSER: cannot reconstruct type";
    return value_null();
}

static Value builtin_ser(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "SER expects 1 argument", line, col);
    }
    SerCtx ctx;
    ser_ctx_init(&ctx);
    JsonBuf jb;
    jb_init(&jb);
    ser_value(&jb, &ctx, interp, args[0]);
    Value out = value_str(jb.data ? jb.data : "");
    jb_free(&jb);
    ser_ctx_free(&ctx);
    return out;
}

static Value builtin_unser(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "UNSER expects 1 argument", line, col);
    }
    EXPECT_STR(args[0], "UNSER", interp, line, col);
    const char* text = args[0].as.s ? args[0].as.s : "";
    const char* jerr = NULL;
    JsonValue* root = json_parse(text, &jerr);
    if (!root) {
        RUNTIME_ERROR(interp, "UNSER: invalid JSON", line, col);
    }
    UnserCtx ctx;
    unser_ctx_init(&ctx);
    const char* err = NULL;
    Value out = deser_val(root, &ctx, interp, &err);
    json_free(root);
    unser_ctx_free(&ctx);
    if (err) {
        RUNTIME_ERROR(interp, err, line, col);
    }
    return out;
}

// ============ Arithmetic operators ============

static Value builtin_add(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
    EXPECT_NUM(args[0], "ADD", interp, line, col);
    EXPECT_NUM(args[1], "ADD", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "ADD cannot mix INT and FLT", line, col);
    }
    
    Value result = value_null();
    if (args[0].type == VAL_INT) {
        result = value_int(args[0].as.i + args[1].as.i);
    } else {
        result = value_flt(args[0].as.f + args[1].as.f);
    }
    if (!writeback_first_ptr(interp, arg_nodes, env, result, "ADD", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_sub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
    EXPECT_NUM(args[0], "SUB", interp, line, col);
    EXPECT_NUM(args[1], "SUB", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "SUB cannot mix INT and FLT", line, col);
    }
    
    Value result = value_null();
    if (args[0].type == VAL_INT) {
        result = value_int(args[0].as.i - args[1].as.i);
    } else {
        result = value_flt(args[0].as.f - args[1].as.f);
    }
    if (!writeback_first_ptr(interp, arg_nodes, env, result, "SUB", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_mul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "MUL", interp, line, col);
    EXPECT_NUM(args[1], "MUL", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "MUL cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i * args[1].as.i);
    }
    return value_flt(args[0].as.f * args[1].as.f);
}

static Value builtin_div(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "DIV", interp, line, col);
    EXPECT_NUM(args[1], "DIV", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "DIV cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        if (args[1].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        return value_int(args[0].as.i / args[1].as.i);
    }
    if (args[1].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    return value_flt(args[0].as.f / args[1].as.f);
}

// CDIV: ceiling integer division (int-only semantics similar to Python's safe_cdiv)
static Value builtin_cdiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "CDIV", interp, line, col);
    EXPECT_INT(args[1], "CDIV", interp, line, col);

    int64_t a = args[0].as.i;
    int64_t b = args[1].as.i;
    if (b == 0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    double res = ceil((double)a / (double)b);
    return value_int((int64_t)res);
}

static Value builtin_mod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "MOD", interp, line, col);
    EXPECT_NUM(args[1], "MOD", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "MOD cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        if (args[1].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        int64_t b = args[1].as.i < 0 ? -args[1].as.i : args[1].as.i;
        return value_int(args[0].as.i % b);
    }
    if (args[1].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    double b = args[1].as.f < 0 ? -args[1].as.f : args[1].as.f;
    return value_flt(fmod(args[0].as.f, b));
}

static Value builtin_pow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "POW", interp, line, col);
    EXPECT_NUM(args[1], "POW", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "POW cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        if (args[1].as.i < 0) {
            RUNTIME_ERROR(interp, "Negative exponent not supported", line, col);
        }
        int64_t result = 1;
        int64_t base = args[0].as.i;
        int64_t exp = args[1].as.i;
        while (exp > 0) {
            if (exp & 1) result *= base;
            base *= base;
            exp >>= 1;
        }
        return value_int(result);
    }
    return value_flt(pow(args[0].as.f, args[1].as.f));
}

static Value builtin_neg(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "NEG", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        return value_int(-args[0].as.i);
    }
    return value_flt(-args[0].as.f);
}

static Value builtin_abs(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ABS", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i);
    }
    return value_flt(args[0].as.f < 0 ? -args[0].as.f : args[0].as.f);
}

// Coercing variants
static Value builtin_iadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IADD", interp, line, col);
    EXPECT_NUM(args[1], "IADD", interp, line, col);
    
    int64_t a = args[0].type == VAL_INT ? args[0].as.i : (int64_t)args[0].as.f;
    int64_t b = args[1].type == VAL_INT ? args[1].as.i : (int64_t)args[1].as.f;
    return value_int(a + b);
}

static Value builtin_isub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ISUB", interp, line, col);
    EXPECT_NUM(args[1], "ISUB", interp, line, col);
    
    int64_t a = args[0].type == VAL_INT ? args[0].as.i : (int64_t)args[0].as.f;
    int64_t b = args[1].type == VAL_INT ? args[1].as.i : (int64_t)args[1].as.f;
    return value_int(a - b);
}

static Value builtin_imul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IMUL", interp, line, col);
    EXPECT_NUM(args[1], "IMUL", interp, line, col);
    
    int64_t a = args[0].type == VAL_INT ? args[0].as.i : (int64_t)args[0].as.f;
    int64_t b = args[1].type == VAL_INT ? args[1].as.i : (int64_t)args[1].as.f;
    return value_int(a * b);
}

static Value builtin_idiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IDIV", interp, line, col);
    EXPECT_NUM(args[1], "IDIV", interp, line, col);
    
    int64_t a = args[0].type == VAL_INT ? args[0].as.i : (int64_t)args[0].as.f;
    int64_t b = args[1].type == VAL_INT ? args[1].as.i : (int64_t)args[1].as.f;
    if (b == 0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    return value_int(a / b);
}

static Value builtin_fadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
    EXPECT_NUM(args[0], "FADD", interp, line, col);
    EXPECT_NUM(args[1], "FADD", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    Value result = value_flt(a + b);
    if (!writeback_first_ptr(interp, arg_nodes, env, result, "FADD", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_fsub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FSUB", interp, line, col);
    EXPECT_NUM(args[1], "FSUB", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    return value_flt(a - b);
}

static Value builtin_fmul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FMUL", interp, line, col);
    EXPECT_NUM(args[1], "FMUL", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    return value_flt(a * b);
}

static Value builtin_fdiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FDIV", interp, line, col);
    EXPECT_NUM(args[1], "FDIV", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    if (b == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    return value_flt(a / b);
}

static Value builtin_ipow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IPOW", interp, line, col);
    EXPECT_NUM(args[1], "IPOW", interp, line, col);
    
    int64_t base = args[0].type == VAL_INT ? args[0].as.i : (int64_t)args[0].as.f;
    int64_t exp = args[1].type == VAL_INT ? args[1].as.i : (int64_t)args[1].as.f;
    if (exp < 0) {
        RUNTIME_ERROR(interp, "Negative exponent not supported", line, col);
    }
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return value_int(result);
}

static Value builtin_fpow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FPOW", interp, line, col);
    EXPECT_NUM(args[1], "FPOW", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    return value_flt(pow(a, b));
}

// ============ Tensor elementwise operators ============

// op: 0=add,1=sub,2=mul,3=div,4=pow
static Value tensor_elemwise_op(Interpreter* interp, Value a, Value b, int op, int line, int col) {
    // Both tensors
    if (a.type == VAL_TNS && b.type == VAL_TNS) {
        Tensor* ta = a.as.tns;
        Tensor* tb = b.as.tns;
        if (ta->elem_type != tb->elem_type) {
            RUNTIME_ERROR(interp, "T* operators require same element types", line, col);
        }
        if (ta->ndim != tb->ndim) {
            RUNTIME_ERROR(interp, "T* operators require same tensor dimensionality", line, col);
        }
        for (size_t i = 0; i < ta->ndim; i++) {
            if (ta->shape[i] != tb->shape[i]) {
                RUNTIME_ERROR(interp, "T* operators require identical tensor shapes", line, col);
            }
        }

        Value out = value_tns_new(ta->elem_type, ta->ndim, ta->shape);
        Tensor* ot = out.as.tns;
        for (size_t i = 0; i < ta->length; i++) {
            Value va = ta->data[i];
            Value vb = tb->data[i];
            // Only support numeric element types
            if (va.type != vb.type) {
                value_free(out);
                RUNTIME_ERROR(interp, "T* element type mismatch", line, col);
            }
            if (va.type == VAL_INT) {
                int64_t ra = va.as.i;
                int64_t rb = vb.as.i;
                if (op == 0) ot->data[i] = value_int(ra + rb);
                else if (op == 1) ot->data[i] = value_int(ra - rb);
                else if (op == 2) ot->data[i] = value_int(ra * rb);
                else if (op == 3) {
                    if (rb == 0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); }
                    ot->data[i] = value_int(ra / rb);
                } else if (op == 4) {
                    if (rb < 0) { value_free(out); RUNTIME_ERROR(interp, "Negative exponent not supported", line, col); }
                    int64_t result = 1;
                    int64_t base = ra;
                    int64_t exp = rb;
                    while (exp > 0) {
                        if (exp & 1) result *= base;
                        base *= base;
                        exp >>= 1;
                    }
                    ot->data[i] = value_int(result);
                }
            } else if (va.type == VAL_FLT) {
                double ra = va.as.f;
                double rb = vb.as.f;
                if (op == 0) ot->data[i] = value_flt(ra + rb);
                else if (op == 1) ot->data[i] = value_flt(ra - rb);
                else if (op == 2) ot->data[i] = value_flt(ra * rb);
                else if (op == 3) {
                    if (rb == 0.0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); }
                    ot->data[i] = value_flt(ra / rb);
                } else if (op == 4) {
                    ot->data[i] = value_flt(pow(ra, rb));
                }
            } else if (va.type == VAL_TNS) {
                // nested tensors: recurse
                ot->data[i] = tensor_elemwise_op(interp, va, vb, op, line, col);
            } else {
                value_free(out);
                RUNTIME_ERROR(interp, "T* operators only support numeric or nested tensor elements", line, col);
            }
        }
        return out;
    }

    // One tensor and one scalar: broadcast scalar
    if (a.type == VAL_TNS && (b.type == VAL_INT || b.type == VAL_FLT)) {
        Tensor* ta = a.as.tns;
        // element static type must match scalar
        if (!((ta->elem_type == TYPE_INT && b.type == VAL_INT) || (ta->elem_type == TYPE_FLT && b.type == VAL_FLT))) {
            RUNTIME_ERROR(interp, "Tensor element type and scalar type mismatch", line, col);
        }
        Value out = value_tns_new(ta->elem_type, ta->ndim, ta->shape);
        Tensor* ot = out.as.tns;
        for (size_t i = 0; i < ta->length; i++) {
            Value va = ta->data[i];
            if (va.type == VAL_INT) {
                int64_t ra = va.as.i;
                int64_t rb = b.as.i;
                if (op == 0) ot->data[i] = value_int(ra + rb);
                else if (op == 1) ot->data[i] = value_int(ra - rb);
                else if (op == 2) ot->data[i] = value_int(ra * rb);
                else if (op == 3) { if (rb == 0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); } ot->data[i] = value_int(ra / rb); }
                else if (op == 4) { if (rb < 0) { value_free(out); RUNTIME_ERROR(interp, "Negative exponent not supported", line, col); } int64_t result = 1; int64_t base = ra; int64_t exp = rb; while (exp > 0) { if (exp & 1) result *= base; base *= base; exp >>= 1; } ot->data[i] = value_int(result); }
            } else if (va.type == VAL_FLT) {
                double ra = va.as.f;
                double rb = b.as.f;
                if (op == 0) ot->data[i] = value_flt(ra + rb);
                else if (op == 1) ot->data[i] = value_flt(ra - rb);
                else if (op == 2) ot->data[i] = value_flt(ra * rb);
                else if (op == 3) { if (rb == 0.0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); } ot->data[i] = value_flt(ra / rb); }
                else if (op == 4) ot->data[i] = value_flt(pow(ra, rb));
            } else if (va.type == VAL_TNS) {
                ot->data[i] = tensor_elemwise_op(interp, va, b, op, line, col);
            } else {
                value_free(out);
                RUNTIME_ERROR(interp, "Unsupported tensor element type for T*", line, col);
            }
        }
        return out;
    }

    if (b.type == VAL_TNS && (a.type == VAL_INT || a.type == VAL_FLT)) {
        // scalar on left, tensor on right: compute scalar OP element
        Tensor* tb = b.as.tns;
        // element static type must match scalar
        if (!((tb->elem_type == TYPE_INT && a.type == VAL_INT) || (tb->elem_type == TYPE_FLT && a.type == VAL_FLT))) {
            RUNTIME_ERROR(interp, "Tensor element type and scalar type mismatch", line, col);
        }
        Value out = value_tns_new(tb->elem_type, tb->ndim, tb->shape);
        Tensor* ot = out.as.tns;
        for (size_t i = 0; i < tb->length; i++) {
            Value vb = tb->data[i];
            if (vb.type == VAL_INT) {
                int64_t ra = a.as.i;
                int64_t rb = vb.as.i;
                if (op == 0) ot->data[i] = value_int(ra + rb);
                else if (op == 1) ot->data[i] = value_int(ra - rb);
                else if (op == 2) ot->data[i] = value_int(ra * rb);
                else if (op == 3) { if (rb == 0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); } ot->data[i] = value_int(ra / rb); }
                else if (op == 4) { if (rb < 0) { value_free(out); RUNTIME_ERROR(interp, "Negative exponent not supported", line, col); } int64_t result = 1; int64_t base = ra; int64_t exp = rb; while (exp > 0) { if (exp & 1) result *= base; base *= base; exp >>= 1; } ot->data[i] = value_int(result); }
            } else if (vb.type == VAL_FLT) {
                double ra = a.as.f;
                double rb = vb.as.f;
                if (op == 0) ot->data[i] = value_flt(ra + rb);
                else if (op == 1) ot->data[i] = value_flt(ra - rb);
                else if (op == 2) ot->data[i] = value_flt(ra * rb);
                else if (op == 3) { if (rb == 0.0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); } ot->data[i] = value_flt(ra / rb); }
                else if (op == 4) ot->data[i] = value_flt(pow(ra, rb));
            } else if (vb.type == VAL_TNS) {
                ot->data[i] = tensor_elemwise_op(interp, a, vb, op, line, col);
            } else {
                value_free(out);
                RUNTIME_ERROR(interp, "Unsupported tensor element type for scalar-left T*", line, col);
            }
        }
        return out;
    }

    RUNTIME_ERROR(interp, "T* operators expect tensors or tensor+scalar", line, col);
}

static Value builtin_tadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    return tensor_elemwise_op(interp, args[0], args[1], 0, line, col);
}

static Value builtin_tsub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    return tensor_elemwise_op(interp, args[0], args[1], 1, line, col);
}

static Value builtin_tmul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    return tensor_elemwise_op(interp, args[0], args[1], 2, line, col);
}

static Value builtin_tdiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    return tensor_elemwise_op(interp, args[0], args[1], 3, line, col);
}

static Value builtin_tpow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    return tensor_elemwise_op(interp, args[0], args[1], 4, line, col);
}

// SHAPE: returns 1-D tensor of INT lengths (one per dimension)
static Value builtin_shape(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "SHAPE expects TNS argument", line, col);
    }
    Tensor* t = args[0].as.tns;
    size_t ndim = t->ndim;
    // prepare items: INT values of each dimension length
    Value* items = malloc(sizeof(Value) * ndim);
    if (!items) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
    for (size_t i = 0; i < ndim; i++) items[i] = value_int((int64_t)t->shape[i]);
    size_t out_shape[1]; out_shape[0] = ndim;
    Value out = value_tns_from_values(TYPE_INT, 1, out_shape, items, ndim);
    for (size_t i = 0; i < ndim; i++) value_free(items[i]);
    free(items);
    return out;
}

// CONV: N-D discrete convolution (two-argument backward-compatible form)
// Usage: CONV(TNS: x, TNS: kernel) -> TNS (same shape as x)
static Value builtin_conv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS || args[1].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "CONV expects (TNS, TNS)", line, col);
    }
    Tensor* x = args[0].as.tns;
    Tensor* k = args[1].as.tns;

    // kernel must have same rank
    if (x->ndim != k->ndim) {
        RUNTIME_ERROR(interp, "CONV kernel must have same rank as input", line, col);
    }

    // kernel dims must be odd
    for (size_t d = 0; d < k->ndim; d++) {
        if ((k->shape[d] & 1) == 0) {
            RUNTIME_ERROR(interp, "CONV kernel dimensions must be odd", line, col);
        }
    }

    // Element types must be numeric
    if (!((x->elem_type == TYPE_INT || x->elem_type == TYPE_FLT) && (k->elem_type == TYPE_INT || k->elem_type == TYPE_FLT))) {
        RUNTIME_ERROR(interp, "CONV only supports INT or FLT element types", line, col);
    }

    // Output typing: INT only if both are INT, otherwise FLT
    DeclType out_decl = (x->elem_type == TYPE_INT && k->elem_type == TYPE_INT) ? TYPE_INT : TYPE_FLT;

    Value out = value_tns_new(out_decl, x->ndim, x->shape);
    Tensor* ot = out.as.tns;

    // Precompute kernel centers
    size_t* centers = malloc(sizeof(size_t) * k->ndim);
    for (size_t d = 0; d < k->ndim; d++) centers[d] = k->shape[d] / 2;

    // For each output position, compute convolution
    for (size_t pos = 0; pos < x->length; pos++) {
        // compute multi-index for pos
        size_t rem = pos;
        size_t idx[64]; // support up to 64 dims (practical)
        if (x->ndim > 64) { free(centers); value_free(out); RUNTIME_ERROR(interp, "CONV: too many dimensions", line, col); }
        for (size_t d = 0; d < x->ndim; d++) {
            idx[d] = rem / x->strides[d];
            rem = rem % x->strides[d];
        }

        if (out_decl == TYPE_INT) {
            int64_t acc = 0;
            for (size_t kpos = 0; kpos < k->length; kpos++) {
                // kernel multi-index
                size_t krem = kpos;
                size_t kidx[64];
                for (size_t d = 0; d < k->ndim; d++) {
                    kidx[d] = krem / k->strides[d];
                    krem = krem % k->strides[d];
                }
                // compute input index with replicate padding
                size_t in_offset = 0;
                for (size_t d = 0; d < x->ndim; d++) {
                    int64_t rel = (int64_t)idx[d] + (int64_t)kidx[d] - (int64_t)centers[d];
                    if (rel < 0) rel = 0;
                    if ((size_t)rel >= x->shape[d]) rel = (int64_t)x->shape[d] - 1;
                    in_offset += (size_t)rel * x->strides[d];
                }
                Value vx = x->data[in_offset];
                Value vk = k->data[kpos];
                if (vx.type != VAL_INT || vk.type != VAL_INT) { free(centers); value_free(out); RUNTIME_ERROR(interp, "CONV integer-mode requires INT elements", line, col); }
                acc += vx.as.i * vk.as.i;
            }
            ot->data[pos] = value_int(acc);
        } else {
            double acc = 0.0;
            for (size_t kpos = 0; kpos < k->length; kpos++) {
                size_t krem = kpos;
                size_t kidx[64];
                for (size_t d = 0; d < k->ndim; d++) {
                    kidx[d] = krem / k->strides[d];
                    krem = krem % k->strides[d];
                }
                size_t in_offset = 0;
                for (size_t d = 0; d < x->ndim; d++) {
                    int64_t rel = (int64_t)idx[d] + (int64_t)kidx[d] - (int64_t)centers[d];
                    if (rel < 0) rel = 0;
                    if ((size_t)rel >= x->shape[d]) rel = (int64_t)x->shape[d] - 1;
                    in_offset += (size_t)rel * x->strides[d];
                }
                Value vx = x->data[in_offset];
                Value vk = k->data[kpos];
                double aval = (vx.type == VAL_FLT) ? vx.as.f : (double)vx.as.i;
                double kval = (vk.type == VAL_FLT) ? vk.as.f : (double)vk.as.i;
                acc += aval * kval;
            }
            ot->data[pos] = value_flt(acc);
        }
    }

    free(centers);
    return out;
}

// TLEN: returns length of 1-based dimension
static Value builtin_tlen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "TLEN expects TNS as first argument", line, col);
    }
    EXPECT_INT(args[1], "TLEN", interp, line, col);
    Tensor* t = args[0].as.tns;
    int64_t dim = args[1].as.i; // 1-based
    if (dim < 1 || (size_t)dim > t->ndim) {
        RUNTIME_ERROR(interp, "TLEN dimension out of range", line, col);
    }
    return value_int((int64_t)t->shape[(size_t)dim - 1]);
}

// TFLIP: returns a new tensor with elements along 1-based dimension dim reversed
static Value builtin_tflip(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "TFLIP expects TNS as first argument", line, col);
    }
    EXPECT_INT(args[1], "TFLIP", interp, line, col);
    Tensor* t = args[0].as.tns;
    int64_t dim1 = args[1].as.i; // 1-based
    if (dim1 < 1 || (size_t)dim1 > t->ndim) {
        RUNTIME_ERROR(interp, "TFLIP dimension out of range", line, col);
    }
    size_t dim = (size_t)dim1 - 1;
    // create output tensor
    Value out = value_tns_new(t->elem_type, t->ndim, t->shape);
    Tensor* ot = out.as.tns;

    // iterate source positions
    for (size_t src = 0; src < t->length; src++) {
        // compute multi-index
        size_t rem = src;
        size_t dst_offset = 0;
        for (size_t d = 0; d < t->ndim; d++) {
            size_t pos = rem / t->strides[d];
            rem = rem % t->strides[d];
            size_t flip_pos = (d == dim) ? (t->shape[d] - 1 - pos) : pos;
            dst_offset += flip_pos * t->strides[d];
        }
        ot->data[dst_offset] = value_copy(t->data[src]);
    }
    return out;
}

// FILL: return a new tensor with the same shape as the first arg,
// filled with the supplied value. The fill value's runtime type
// must match the existing element types in the source tensor.
static Value builtin_fill(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "FILL expects TNS as first argument", line, col);
    }
    Tensor* t = args[0].as.tns;
    Value fill = args[1];
    // Ensure element runtime types match the fill value's type
    for (size_t i = 0; i < t->length; i++) {
        if (t->data[i].type != fill.type) {
            RUNTIME_ERROR(interp, "FILL value type must match existing tensor element types", line, col);
        }
    }

    Value out = value_tns_new(t->elem_type, t->ndim, t->shape);
    Tensor* ot = out.as.tns;
    for (size_t i = 0; i < t->length; i++) {
        ot->data[i] = value_copy(fill);
    }
    return out;
}

// SCAT: return a copy of dst with a rectangular slice replaced by src.
// Args: SCAT(TNS: src, TNS: dst, TNS: ind)
static Value builtin_scat(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS || args[1].type != VAL_TNS || args[2].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "SCAT expects (TNS, TNS, TNS)", line, col);
    }
    Tensor* src = args[0].as.tns;
    Tensor* dst = args[1].as.tns;
    Tensor* ind = args[2].as.tns;

    size_t rank = dst->ndim;
    // ind must be 2-D with shape [rank, 2]
    if (ind->ndim != 2) {
        RUNTIME_ERROR(interp, "SCAT index tensor must be 2-dimensional", line, col);
    }
    if (ind->shape[0] != rank || ind->shape[1] != 2) {
        RUNTIME_ERROR(interp, "SCAT index tensor shape must be [rank,2]", line, col);
    }

    // src must have same dimensionality as dst and element types must match
    if (src->ndim != rank) {
        RUNTIME_ERROR(interp, "SCAT src must have same rank as dst", line, col);
    }
    if (src->elem_type != dst->elem_type) {
        RUNTIME_ERROR(interp, "SCAT src and dst element types must match", line, col);
    }

    // Read lo/hi per dimension and validate bounds
    int64_t* lo = malloc(sizeof(int64_t) * rank);
    int64_t* hi = malloc(sizeof(int64_t) * rank);
    if (!lo || !hi) { free(lo); free(hi); RUNTIME_ERROR(interp, "Out of memory", line, col); }

    for (size_t d = 0; d < rank; d++) {
        // index into ind: row d, col 0 and 1 -> linear index = d*ind->strides[0] + col*ind->strides[1]
        size_t base = d * ind->strides[0];
        Value vlo = ind->data[base + 0 * ind->strides[1]];
        Value vhi = ind->data[base + 1 * ind->strides[1]];
        if (vlo.type != VAL_INT || vhi.type != VAL_INT) {
            free(lo); free(hi);
            RUNTIME_ERROR(interp, "SCAT indices must be INT", line, col);
        }
        int64_t l = vlo.as.i;
        int64_t h = vhi.as.i;
        if (l == 0 || h == 0) { free(lo); free(hi); RUNTIME_ERROR(interp, "SCAT indices are 1-based and cannot be 0", line, col); }
        // handle negative indices: -1 means last element
        if (l < 0) l = (int64_t)dst->shape[d] + l + 1;
        if (h < 0) h = (int64_t)dst->shape[d] + h + 1;
        // convert to 0-based for internal checks
        int64_t l0 = l - 1;
        int64_t h0 = h - 1;
        if (l0 < 0 || h0 < 0 || (size_t)h0 >= dst->shape[d] || l0 > h0) { free(lo); free(hi); RUNTIME_ERROR(interp, "SCAT index out of range or invalid", line, col); }
        // check slice length matches src dimension
        int64_t expected = h0 - l0 + 1;
        if ((size_t)expected != src->shape[d]) { free(lo); free(hi); RUNTIME_ERROR(interp, "SCAT src dimension lengths must match index spans", line, col); }
        lo[d] = l0;
        hi[d] = h0;
    }

    // Build output tensor as a copy of dst structure
    Value out = value_tns_new(dst->elem_type, dst->ndim, dst->shape);
    Tensor* ot = out.as.tns;

    // Iterate over all positions in dst. For positions inside the slice, copy from src; otherwise copy dst
    for (size_t pos = 0; pos < dst->length; pos++) {
        // compute multi-index
        size_t rem = pos;
        size_t dst_offset = 0;
        size_t src_offset = 0;
        int inside = 1;
        for (size_t d = 0; d < rank; d++) {
            size_t idx = rem / dst->strides[d];
            rem = rem % dst->strides[d];
            if ((int64_t)idx < lo[d] || (int64_t)idx > hi[d]) {
                inside = 0;
            } else {
                size_t src_idx = (size_t)((int64_t)idx - lo[d]);
                src_offset += src_idx * src->strides[d];
            }
            dst_offset += idx * dst->strides[d];
        }
        if (inside) {
            ot->data[dst_offset] = value_copy(src->data[src_offset]);
        } else {
            ot->data[dst_offset] = value_copy(dst->data[dst_offset]);
        }
    }

    free(lo); free(hi);
    return out;
}

// M* operators: strict elementwise operations for two tensors (no broadcasting)
static Value builtin_mop(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col, int op) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS || args[1].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "M* operators expect TNS arguments", line, col);
    }
    Tensor* ta = args[0].as.tns;
    Tensor* tb = args[1].as.tns;
    if (ta->ndim != tb->ndim) {
        RUNTIME_ERROR(interp, "M* operators require same tensor dimensionality", line, col);
    }
    for (size_t i = 0; i < ta->ndim; i++) {
        if (ta->shape[i] != tb->shape[i]) {
            RUNTIME_ERROR(interp, "M* operators require identical tensor shapes", line, col);
        }
    }
    if (ta->elem_type != tb->elem_type) {
        RUNTIME_ERROR(interp, "M* operators require same element types", line, col);
    }
    if (!(ta->elem_type == TYPE_INT || ta->elem_type == TYPE_FLT)) {
        RUNTIME_ERROR(interp, "M* operators only support INT or FLT element types", line, col);
    }

    Value out = value_tns_new(ta->elem_type, ta->ndim, ta->shape);
    Tensor* ot = out.as.tns;

    for (size_t i = 0; i < ta->length; i++) {
        Value va = ta->data[i];
        Value vb = tb->data[i];
        // Expect scalar numeric elements
        if (va.type != vb.type) { value_free(out); RUNTIME_ERROR(interp, "M* element type mismatch", line, col); }
        if (va.type == VAL_INT) {
            int64_t a = va.as.i;
            int64_t b = vb.as.i;
            if (op == 0) ot->data[i] = value_int(a + b);
            else if (op == 1) ot->data[i] = value_int(a - b);
            else if (op == 2) ot->data[i] = value_int(a * b);
            else if (op == 3) {
                if (b == 0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); }
                ot->data[i] = value_int(a / b);
            }
        } else if (va.type == VAL_FLT) {
            double a = va.as.f;
            double b = vb.as.f;
            if (op == 0) ot->data[i] = value_flt(a + b);
            else if (op == 1) ot->data[i] = value_flt(a - b);
            else if (op == 2) ot->data[i] = value_flt(a * b);
            else if (op == 3) {
                if (b == 0.0) { value_free(out); RUNTIME_ERROR(interp, "Division by zero", line, col); }
                ot->data[i] = value_flt(a / b);
            }
        } else {
            value_free(out);
            RUNTIME_ERROR(interp, "M* operators only support numeric scalar elements", line, col);
        }
    }
    return out;
}

static Value builtin_madd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    return builtin_mop(interp, args, argc, arg_nodes, env, line, col, 0);
}
static Value builtin_msub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    return builtin_mop(interp, args, argc, arg_nodes, env, line, col, 1);
}
static Value builtin_mmul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    return builtin_mop(interp, args, argc, arg_nodes, env, line, col, 2);
}
static Value builtin_mdiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    return builtin_mop(interp, args, argc, arg_nodes, env, line, col, 3);
}

// MSUM: elementwise sum across N tensors
static Value builtin_msum(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "MSUM requires at least one tensor", line, col);
    }
    // all args must be tensors with same shape and element type
    for (int j = 0; j < argc; j++) {
        if (args[j].type != VAL_TNS) {
            RUNTIME_ERROR(interp, "MSUM expects TNS arguments", line, col);
        }
    }
    Tensor* t0 = args[0].as.tns;
    for (int j = 1; j < argc; j++) {
        Tensor* tj = args[j].as.tns;
        if (tj->ndim != t0->ndim) {
            RUNTIME_ERROR(interp, "MSUM requires same tensor dimensionality", line, col);
        }
        for (size_t d = 0; d < t0->ndim; d++) {
            if (tj->shape[d] != t0->shape[d]) {
                RUNTIME_ERROR(interp, "MSUM requires identical tensor shapes", line, col);
            }
        }
        if (tj->elem_type != t0->elem_type) {
            RUNTIME_ERROR(interp, "MSUM requires same element types", line, col);
        }
    }
    if (!(t0->elem_type == TYPE_INT || t0->elem_type == TYPE_FLT)) {
        RUNTIME_ERROR(interp, "MSUM only supports INT or FLT element types", line, col);
    }

    Value out = value_tns_new(t0->elem_type, t0->ndim, t0->shape);
    Tensor* ot = out.as.tns;
    for (size_t i = 0; i < t0->length; i++) {
        if (t0->elem_type == TYPE_INT) {
            int64_t acc = 0;
            for (int j = 0; j < argc; j++) {
                Value v = args[j].as.tns->data[i];
                if (v.type != VAL_INT) { value_free(out); RUNTIME_ERROR(interp, "MSUM element type mismatch", line, col); }
                acc += v.as.i;
            }
            ot->data[i] = value_int(acc);
        } else {
            double acc = 0.0;
            for (int j = 0; j < argc; j++) {
                Value v = args[j].as.tns->data[i];
                if (v.type != VAL_FLT) { value_free(out); RUNTIME_ERROR(interp, "MSUM element type mismatch", line, col); }
                acc += v.as.f;
            }
            ot->data[i] = value_flt(acc);
        }
    }
    return out;
}

// MPROD: elementwise product across N tensors
static Value builtin_mprod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "MPROD requires at least one tensor", line, col);
    }
    for (int j = 0; j < argc; j++) {
        if (args[j].type != VAL_TNS) {
            RUNTIME_ERROR(interp, "MPROD expects TNS arguments", line, col);
        }
    }
    Tensor* t0 = args[0].as.tns;
    for (int j = 1; j < argc; j++) {
        Tensor* tj = args[j].as.tns;
        if (tj->ndim != t0->ndim) {
            RUNTIME_ERROR(interp, "MPROD requires same tensor dimensionality", line, col);
        }
        for (size_t d = 0; d < t0->ndim; d++) {
            if (tj->shape[d] != t0->shape[d]) {
                RUNTIME_ERROR(interp, "MPROD requires identical tensor shapes", line, col);
            }
        }
        if (tj->elem_type != t0->elem_type) {
            RUNTIME_ERROR(interp, "MPROD requires same element types", line, col);
        }
    }
    if (!(t0->elem_type == TYPE_INT || t0->elem_type == TYPE_FLT)) {
        RUNTIME_ERROR(interp, "MPROD only supports INT or FLT element types", line, col);
    }

    Value out = value_tns_new(t0->elem_type, t0->ndim, t0->shape);
    Tensor* ot = out.as.tns;
    for (size_t i = 0; i < t0->length; i++) {
        if (t0->elem_type == TYPE_INT) {
            int64_t acc = 1;
            for (int j = 0; j < argc; j++) {
                Value v = args[j].as.tns->data[i];
                if (v.type != VAL_INT) { value_free(out); RUNTIME_ERROR(interp, "MPROD element type mismatch", line, col); }
                acc *= v.as.i;
            }
            ot->data[i] = value_int(acc);
        } else {
            double acc = 1.0;
            for (int j = 0; j < argc; j++) {
                Value v = args[j].as.tns->data[i];
                if (v.type != VAL_FLT) { value_free(out); RUNTIME_ERROR(interp, "MPROD element type mismatch", line, col); }
                acc *= v.as.f;
            }
            ot->data[i] = value_flt(acc);
        }
    }
    return out;
}

// ROOT and variants
static Value builtin_root(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ROOT", interp, line, col);
    EXPECT_NUM(args[1], "ROOT", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "ROOT cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t x = args[0].as.i;
        int64_t n = args[1].as.i;
        if (n == 0) {
            RUNTIME_ERROR(interp, "ROOT exponent must be non-zero", line, col);
        }
        if (n < 0) {
            if (x == 0) {
                RUNTIME_ERROR(interp, "Division by zero", line, col);
            }
            if (x != 1 && x != -1) {
                RUNTIME_ERROR(interp, "Negative ROOT exponent yields non-integer result", line, col);
            }
            return value_int(x);
        }
        if (n == 1) return value_int(x);
        if (x >= 0) {
            // Binary search for floor of nth root
            int64_t lo = 0, hi = 1;
            while (1) {
                int64_t pw = 1;
                for (int64_t i = 0; i < n && pw <= x; i++) pw *= hi;
                if (pw > x) break;
                hi <<= 1;
            }
            while (lo + 1 < hi) {
                int64_t mid = (lo + hi) / 2;
                int64_t pw = 1;
                for (int64_t i = 0; i < n; i++) pw *= mid;
                if (pw <= x) lo = mid;
                else hi = mid;
            }
            return value_int(lo);
        } else {
            if (n % 2 == 0) {
                RUNTIME_ERROR(interp, "Even root of negative integer", line, col);
            }
            int64_t ax = -x;
            int64_t lo = 0, hi = 1;
            while (1) {
                int64_t pw = 1;
                for (int64_t i = 0; i < n && pw <= ax; i++) pw *= hi;
                if (pw > ax) break;
                hi <<= 1;
            }
            while (lo + 1 < hi) {
                int64_t mid = (lo + hi) / 2;
                int64_t pw = 1;
                for (int64_t i = 0; i < n; i++) pw *= mid;
                if (pw <= ax) lo = mid;
                else hi = mid;
            }
            return value_int(-lo);
        }
    }
    
    double x = args[0].as.f;
    double n = args[1].as.f;
    if (n == 0.0) {
        RUNTIME_ERROR(interp, "ROOT exponent must be non-zero", line, col);
    }
    if (x == 0.0 && n < 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    if (x < 0.0) {
        double abs_n = n < 0 ? -n : n;
        if (floor(abs_n) != abs_n || ((int64_t)abs_n) % 2 == 0) {
            RUNTIME_ERROR(interp, "ROOT of negative float requires odd integer root", line, col);
        }
        return value_flt(-pow(-x, 1.0 / n));
    }
    return value_flt(pow(x, 1.0 / n));
}

// IROOT: integer-specific root (coerces/expects integers)
static Value builtin_iroot(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "IROOT", interp, line, col);
    EXPECT_INT(args[1], "IROOT", interp, line, col);
    return builtin_root(interp, args, argc, arg_nodes, env, line, col);
}

// FROOT: float-specific root (coerce args to float and delegate)
static Value builtin_froot(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // Create temporary float-valued args and call root
    Value tmp[2];
    tmp[0].type = VAL_FLT;
    tmp[0].as.f = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    tmp[1].type = VAL_FLT;
    tmp[1].as.f = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    return builtin_root(interp, tmp, 2, NULL, NULL, line, col);
}

// LOG
static Value builtin_log(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LOG", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        int64_t x = args[0].as.i;
        if (x <= 0) {
            RUNTIME_ERROR(interp, "LOG argument must be > 0", line, col);
        }
        int64_t result = 0;
        while (x > 1) {
            x >>= 1;
            result++;
        }
        return value_int(result);
    }
    
    double x = args[0].as.f;
    if (x <= 0.0) {
        RUNTIME_ERROR(interp, "LOG argument must be > 0", line, col);
    }
    return value_flt(floor(log2(x)));
}

// CLOG: integer-only variant of LOG with ceiling-like behavior for powers of two
static Value builtin_clog(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "CLOG", interp, line, col);
    int64_t x = args[0].as.i;
    if (x <= 0) {
        RUNTIME_ERROR(interp, "CLOG argument must be > 0", line, col);
    }
    int bits = 0;
    int64_t tmp = x;
    while (tmp > 0) { tmp >>= 1; bits++; }
    if ((x & (x - 1)) == 0) {
        return value_int(bits - 1);
    }
    return value_int(bits);
}

// GCD
static int64_t gcd_int(int64_t a, int64_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        int64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static Value builtin_gcd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "GCD", interp, line, col);
    EXPECT_NUM(args[1], "GCD", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "GCD cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(gcd_int(args[0].as.i, args[1].as.i));
    }
    
    double a = args[0].as.f;
    double b = args[1].as.f;
    if (floor(a) != a || floor(b) != b) {
        RUNTIME_ERROR(interp, "GCD expects integer-valued floats", line, col);
    }
    return value_flt((double)gcd_int((int64_t)a, (int64_t)b));
}

// LCM
static Value builtin_lcm(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LCM", interp, line, col);
    EXPECT_NUM(args[1], "LCM", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LCM cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t a = args[0].as.i;
        int64_t b = args[1].as.i;
        if (a == 0 || b == 0) return value_int(0);
        int64_t g = gcd_int(a, b);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        return value_int((a / g) * b);
    }
    
    double a = args[0].as.f;
    double b = args[1].as.f;
    if (floor(a) != a || floor(b) != b) {
        RUNTIME_ERROR(interp, "LCM expects integer-valued floats", line, col);
    }
    int64_t ai = (int64_t)a;
    int64_t bi = (int64_t)b;
    if (ai == 0 || bi == 0) return value_flt(0.0);
    int64_t g = gcd_int(ai, bi);
    if (ai < 0) ai = -ai;
    if (bi < 0) bi = -bi;
    return value_flt((double)((ai / g) * bi));
}

// ============ Comparison operators ============

// Recursive deep equality helper for Values (returns 1 if equal, 0 otherwise)
static int value_deep_eq(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_INT:
            return a.as.i == b.as.i ? 1 : 0;
        case VAL_FLT:
            return a.as.f == b.as.f ? 1 : 0;
        case VAL_STR:
            if (a.as.s == NULL || b.as.s == NULL) return (a.as.s == b.as.s) ? 1 : 0;
            return strcmp(a.as.s, b.as.s) == 0 ? 1 : 0;
        case VAL_FUNC:
            return a.as.func == b.as.func ? 1 : 0;
        case VAL_TNS: {
            Tensor* ta = a.as.tns;
            Tensor* tb = b.as.tns;
            if (ta == NULL || tb == NULL) return (ta == tb) ? 1 : 0;
            if (ta->elem_type != tb->elem_type) return 0;
            if (ta->ndim != tb->ndim) return 0;
            for (size_t i = 0; i < ta->ndim; i++) {
                if (ta->shape[i] != tb->shape[i]) return 0;
            }
            if (ta->length != tb->length) return 0;
            for (size_t i = 0; i < ta->length; i++) {
                if (!value_deep_eq(ta->data[i], tb->data[i])) return 0;
            }
            return 1;
        }
        case VAL_MAP: {
            Map* ma = a.as.map;
            Map* mb = b.as.map;
            if (ma == NULL || mb == NULL) return (ma == mb) ? 1 : 0;
            if (ma->count != mb->count) return 0;
            for (size_t i = 0; i < ma->count; i++) {
                int found = 0;
                for (size_t j = 0; j < mb->count; j++) {
                    if (value_deep_eq(ma->items[i].key, mb->items[j].key)) {
                        if (!value_deep_eq(ma->items[i].value, mb->items[j].value)) return 0;
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            }
            return 1;
        }
        case VAL_THR:
            return a.as.thr == b.as.thr ? 1 : 0;
        default:
            return 0;
    }
}

static Value builtin_eq(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp;

    // If types differ, not equal
    if (args[0].type != args[1].type) {
        return value_int(0);
    }

    return value_int(value_deep_eq(args[0], args[1]) ? 1 : 0);
}

static Value builtin_gt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "GT", interp, line, col);
    EXPECT_NUM(args[1], "GT", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "GT cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i > args[1].as.i ? 1 : 0);
    }
    return value_int(args[0].as.f > args[1].as.f ? 1 : 0);
}

static Value builtin_lt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LT", interp, line, col);
    EXPECT_NUM(args[1], "LT", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LT cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i < args[1].as.i ? 1 : 0);
    }
    return value_int(args[0].as.f < args[1].as.f ? 1 : 0);
}

static Value builtin_gte(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "GTE", interp, line, col);
    EXPECT_NUM(args[1], "GTE", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "GTE cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i >= args[1].as.i ? 1 : 0);
    }
    return value_int(args[0].as.f >= args[1].as.f ? 1 : 0);
}

static Value builtin_lte(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LTE", interp, line, col);
    EXPECT_NUM(args[1], "LTE", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LTE cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i <= args[1].as.i ? 1 : 0);
    }
    return value_int(args[0].as.f <= args[1].as.f ? 1 : 0);
}

// ============ Logical operators ============

static Value builtin_and(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(value_truthiness(args[0]) && value_truthiness(args[1]) ? 1 : 0);
}

static Value builtin_or(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(value_truthiness(args[0]) || value_truthiness(args[1]) ? 1 : 0);
}

static Value builtin_xor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    int a = value_truthiness(args[0]) ? 1 : 0;
    int b = value_truthiness(args[1]) ? 1 : 0;
    return value_int(a ^ b);
}

static Value builtin_not(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(value_truthiness(args[0]) ? 0 : 1);
}

static Value builtin_bool(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(value_truthiness(args[0]) ? 1 : 0);
}

// ============ Bitwise operators ============

static Value builtin_band(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BAND", interp, line, col);
    EXPECT_INT(args[1], "BAND", interp, line, col);
    return value_int(args[0].as.i & args[1].as.i);
}

static Value builtin_bor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BOR", interp, line, col);
    EXPECT_INT(args[1], "BOR", interp, line, col);
    return value_int(args[0].as.i | args[1].as.i);
}

static Value builtin_bxor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BXOR", interp, line, col);
    EXPECT_INT(args[1], "BXOR", interp, line, col);
    return value_int(args[0].as.i ^ args[1].as.i);
}

static Value builtin_bnot(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BNOT", interp, line, col);
    return value_int(~args[0].as.i);
}

static Value builtin_shl(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "SHL", interp, line, col);
    EXPECT_INT(args[1], "SHL", interp, line, col);
    if (args[1].as.i < 0) {
        RUNTIME_ERROR(interp, "SHL amount must be non-negative", line, col);
    }
    return value_int(args[0].as.i << args[1].as.i);
}

static Value builtin_shr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "SHR", interp, line, col);
    EXPECT_INT(args[1], "SHR", interp, line, col);
    if (args[1].as.i < 0) {
        RUNTIME_ERROR(interp, "SHR amount must be non-negative", line, col);
    }
    return value_int(args[0].as.i >> args[1].as.i);
}

// ============ Type conversion ============

static Value builtin_int(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i);
    }
    if (args[0].type == VAL_FLT) {
        return value_int((int64_t)args[0].as.f);
    }
    if (args[0].type == VAL_STR) {
        const char* s = args[0].as.s;
        if (s == NULL || *s == '\0') {
            return value_int(0);
        }
        // Parse as binary integer
        bool neg = false;
        if (*s == '-') {
            neg = true;
            s++;
        }
        // Check if it's a valid binary string
        bool valid = true;
        for (const char* p = s; *p; p++) {
            if (*p != '0' && *p != '1') {
                valid = false;
                break;
            }
        }
        if (!valid || *s == '\0') {
            // Non-binary non-empty string -> 1
            return value_int(1);
        }
        int64_t val = strtoll(s, NULL, 2);
        return value_int(neg ? -val : val);
    }
    RUNTIME_ERROR(interp, "INT expects INT, FLT, or STR argument", line, col);
}

static Value builtin_flt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (args[0].type == VAL_FLT) {
        return value_flt(args[0].as.f);
    }
    if (args[0].type == VAL_INT) {
        return value_flt((double)args[0].as.i);
    }
    if (args[0].type == VAL_STR) {
        // Parse binary float string
        const char* s = args[0].as.s;
        if (s == NULL || *s == '\0') {
            return value_flt(0.0);
        }
        // Accept special textual FLT values
        if (strcmp(s, "INF") == 0) return value_flt(INFINITY);
        if (strcmp(s, "-INF") == 0) return value_flt(-INFINITY);
        if (strcmp(s, "NaN") == 0) return value_flt(NAN);
        bool neg = false;
        if (*s == '-') {
            neg = true;
            s++;
        }
        // Find dot
        const char* dot = strchr(s, '.');
        double int_part = 0.0;
        double frac_part = 0.0;
        
        if (dot) {
            // Parse integer part
            for (const char* p = s; p < dot; p++) {
                if (*p == '0' || *p == '1') {
                    int_part = int_part * 2 + (*p - '0');
                }
            }
            // Parse fractional part
            double weight = 0.5;
            for (const char* p = dot + 1; *p; p++) {
                if (*p == '0' || *p == '1') {
                    frac_part += (*p - '0') * weight;
                    weight /= 2;
                }
            }
        } else {
            // Just integer
            for (const char* p = s; *p; p++) {
                if (*p == '0' || *p == '1') {
                    int_part = int_part * 2 + (*p - '0');
                }
            }
        }
        double val = int_part + frac_part;
        return value_flt(neg ? -val : val);
    }
    RUNTIME_ERROR(interp, "FLT expects INT, FLT, or STR argument", line, col);
}

static Value builtin_str(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    if (args[0].type == VAL_STR) {
        return value_str(args[0].as.s);
    }
    if (args[0].type == VAL_INT) {
        char* s = int_to_binary_str(args[0].as.i);
        Value v = value_str(s);
        free(s);
        return v;
    }
    if (args[0].type == VAL_FLT) {
        char* s = flt_to_binary_str(args[0].as.f);
        Value v = value_str(s);
        free(s);
        return v;
    }
    if (args[0].type == VAL_FUNC) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<func %p>", (void*)args[0].as.func);
        return value_str(buf);
    }
    return value_str("");
}

// BYTES(INT: n, endian = "big"):TNS
static Value builtin_bytes(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // Expect first arg INT
    EXPECT_INT(args[0], "BYTES", interp, line, col);
    int64_t n = args[0].as.i;
    if (n < 0) {
        RUNTIME_ERROR(interp, "BYTES: negative integer not allowed", line, col);
    }

    // Default endian is "big"
    bool little = false;
    if (argc >= 2) {
        if (args[1].type != VAL_STR) {
            RUNTIME_ERROR(interp, "BYTES: endian must be a string\n", line, col);
        }
        const char* e = args[1].as.s;
        if (strcmp(e, "little") == 0) {
            little = true;
        } else if (strcmp(e, "big") == 0) {
            little = false;
        } else {
            RUNTIME_ERROR(interp, "BYTES: endian must be \"big\" or \"little\"", line, col);
        }
    }

    // Compute byte length: max(1, ceil(bit_length(n)/8))
    uint64_t un = (uint64_t)n;
    int bits = 0;
    if (un == 0) bits = 1; else {
        while (un > 0) { bits++; un >>= 1; }
    }
    int bytelength = (bits + 7) / 8;
    if (bytelength < 1) bytelength = 1;

    // Recompute unsigned value for extraction
    uint64_t val = (uint64_t)n;
    Value* items = malloc(sizeof(Value) * (size_t)bytelength);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (int i = 0; i < bytelength; i++) {
        uint8_t b;
        if (little) {
            b = (uint8_t)((val >> (8 * i)) & 0xFFULL);
        } else {
            int shift = 8 * (bytelength - 1 - i);
            b = (uint8_t)((val >> shift) & 0xFFULL);
        }
        items[i] = value_int((int64_t)b);
    }
    size_t shape[1]; shape[0] = (size_t)bytelength;
    Value out = value_tns_from_values(TYPE_INT, 1, shape, items, (size_t)bytelength);
    for (int i = 0; i < bytelength; i++) value_free(items[i]);
    free(items);
    return out;
}

// ============ String operations ============

static Value builtin_slen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "SLEN", interp, line, col);
    return value_int((int64_t)strlen(args[0].as.s));
}

static Value builtin_upper(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "UPPER", interp, line, col);
    char* s = strdup(args[0].as.s);
    for (char* p = s; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    Value v = value_str(s);
    free(s);
    return v;
}

static Value builtin_lower(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "LOWER", interp, line, col);
    char* s = strdup(args[0].as.s);
    for (char* p = s; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    Value v = value_str(s);
    free(s);
    return v;
}

static Value builtin_flip(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // Accept INT or STR
    if (args[0].type == VAL_INT) {
        int64_t v = args[0].as.i;
        int is_negative = v < 0;
        uint64_t u = is_negative ? (uint64_t)(-v) : (uint64_t)v;

        // get binary digits for absolute value
        char buf[128];
        int pos = 0;
        if (u == 0) {
            buf[pos++] = '0';
        } else {
            // build digits in MSB-first order
            // find highest bit manually for portability
            int highest = -1;
            for (int b = 63; b >= 0; --b) {
                if ((u >> b) & 1ULL) { highest = b; break; }
            }
            if (highest < 0) { buf[pos++] = '0'; }
            else {
                for (int i = highest; i >= 0; --i) {
                    buf[pos++] = ((u >> i) & 1ULL) ? '1' : '0';
                }
            }
        }
        buf[pos] = '\0';

        // reverse the digit string
        for (int i = 0, j = pos - 1; i < j; ++i, --j) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }

        // parse reversed binary string into integer
        uint64_t out = 0;
        for (int i = 0; i < pos; ++i) {
            out = (out << 1) + (buf[i] == '1');
        }

        int64_t result = (int64_t)out;
        if (is_negative) result = -result;
        return value_int(result);
    }

    if (args[0].type == VAL_STR) {
        const char* s = args[0].as.s;
        size_t n = strlen(s);
        char* out = malloc(n + 1);
        if (!out) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        for (size_t i = 0; i < n; ++i) {
            out[i] = s[n - 1 - i];
        }
        out[n] = '\0';
        Value v = value_str(out);
        free(out);
        return v;
    }

    RUNTIME_ERROR(interp, "FLIP expects INT or STR", line, col);
}

static Value builtin_join(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // JOIN(a1, a2, ..., aN): if first arg is STR, treat it as separator
    // and join subsequent STR args; otherwise join INTs by binary spellings
    if (argc < 1) {
        RUNTIME_ERROR(interp, "JOIN requires at least 1 argument", line, col);
    }

    // Disallow tensors
    for (int i = 0; i < argc; ++i) {
        if (args[i].type == VAL_TNS) {
            RUNTIME_ERROR(interp, "JOIN cannot operate on tensors", line, col);
        }
    }
    int first_type = args[0].type;
    if (first_type == VAL_STR) {
        // If the first string is a single-character separator and there
        // are at least two following items, treat it as `sep, a, b, ...`
        // and join the remaining args with `sep` between them. Otherwise
        // simply concatenate all string arguments in order.
        const char* first_s = args[0].as.s;
        size_t first_len = strlen(first_s);
        if (first_len == 1 && argc >= 3) {
            const char* sep = first_s;
            size_t sep_len = 1;
            // ensure following args are strings
            size_t total = 0;
            for (int i = 1; i < argc; ++i) {
                if (args[i].type != VAL_STR) {
                    RUNTIME_ERROR(interp, "JOIN cannot mix integers and strings", line, col);
                }
                total += strlen(args[i].as.s);
                if (i > 1) total += sep_len;
            }
            char* out = malloc(total + 1);
            if (!out) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
            out[0] = '\0';
            for (int i = 1; i < argc; ++i) {
                if (i > 1) strcat(out, sep);
                strcat(out, args[i].as.s);
            }
            Value v = value_str(out);
            free(out);
            return v;
        } else {
            // Concatenate all string arguments in order
            size_t total = 0;
            for (int i = 0; i < argc; ++i) {
                if (args[i].type != VAL_STR) {
                    RUNTIME_ERROR(interp, "JOIN cannot mix integers and strings", line, col);
                }
                total += strlen(args[i].as.s);
            }
            char* out = malloc(total + 1);
            if (!out) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
            size_t pos = 0;
            for (int i = 0; i < argc; ++i) {
                const char* s = args[i].as.s;
                size_t n = strlen(s);
                memcpy(out + pos, s, n);
                pos += n;
            }
            out[pos] = '\0';
            Value v = value_str(out);
            free(out);
            return v;
        }
    }

    // Integer path: concatenate binary spellings
    // Ensure all args are integers and check sign consistency
    for (int i = 0; i < argc; ++i) {
        if (args[i].type != VAL_INT) {
            RUNTIME_ERROR(interp, "JOIN cannot mix integers and strings", line, col);
        }
    }
    // Check sign consistency
    bool any_neg = false;
    bool any_pos = false;
    for (int i = 0; i < argc; ++i) {
        int64_t val = args[i].as.i;
        if (val < 0) any_neg = true; else any_pos = true;
    }
    if (any_neg && any_pos) {
        RUNTIME_ERROR(interp, "JOIN arguments must not mix positive and negative values", line, col);
    }

    // Build concatenated bits
    size_t bits_len = 0;
    for (int i = 0; i < argc; ++i) {
        int64_t v = args[i].as.i;
        uint64_t av = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
        if (av == 0) bits_len += 1;
        else {
            uint64_t tmp = av;
            while (tmp) { bits_len++; tmp >>= 1; }
        }
    }
    char* bits = malloc(bits_len + 1);
    if (!bits) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
    size_t p = 0;
    for (int i = 0; i < argc; ++i) {
        int64_t v = args[i].as.i;
        uint64_t av = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
        if (av == 0) {
            bits[p++] = '0';
        } else {
            // use int_to_binary_str to get textual bits for absolute value
            char* s = int_to_binary_str((int64_t)av);
            size_t n = strlen(s);
            memcpy(bits + p, s, n);
            p += n;
            free(s);
        }
    }
    bits[p] = '\0';

    // parse bits into integer
    uint64_t outv = 0;
    for (size_t i = 0; i < p; ++i) {
        outv = (outv << 1) + (bits[i] == '1');
    }
    free(bits);

    int64_t result = (int64_t)outv;
    if (any_neg) result = -result;
    return value_int(result);
}

static Value builtin_split(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // SPLIT(str, sep?) -> 1-D TNS of STR
    EXPECT_STR(args[0], "SPLIT", interp, line, col);
    const char* sep = NULL;
    if (argc >= 2) {
        EXPECT_STR(args[1], "SPLIT", interp, line, col);
        sep = args[1].as.s;
    }
    const char* s = args[0].as.s;
    // simple separator: if sep==NULL split on whitespace, else split on sep exactly
    char* copy = strdup(s);
    char* saveptr = NULL;
    char* token;
    size_t cap = 8;
    size_t count = 0;
    Value* items = malloc(sizeof(Value) * cap);
    if (!items) { free(copy); RUNTIME_ERROR(interp, "Out of memory", line, col); }
    if (sep == NULL) {
        // whitespace split
        token = strtok_s(copy, " \t\r\n", &saveptr);
        if (token) {
            if (count + 1 > cap) { cap *= 2; items = realloc(items, sizeof(Value) * cap); }
            items[count++] = value_str(token);
        }
    } else {
        // split on sep: iterate
        size_t seplen = strlen(sep);
        char* cur = copy;
        char* found;
        while ((found = strstr(cur, sep)) != NULL) {
            size_t len = (size_t)(found - cur);
            char* piece = malloc(len + 1);
            memcpy(piece, cur, len);
            piece[len] = '\0';
            if (count + 1 > cap) { cap *= 2; items = realloc(items, sizeof(Value) * cap); }
            items[count++] = value_str(piece);
            free(piece);
            cur = found + seplen;
        }
        // last piece
        if (*cur != '\0') {
            if (count + 1 > cap) { cap *= 2; items = realloc(items, sizeof(Value) * cap); }
            items[count++] = value_str(cur);
        }
        free(copy);
        if (count == 0) {
            free(items);
            return value_tns_new(TYPE_STR, 1, (const size_t[]){0});
        }
        size_t shape[1] = { count };
        Value out = value_tns_from_values(TYPE_STR, 1, shape, items, count);
        for (size_t i = 0; i < count; i++) value_free(items[i]);
        free(items);
        return out;
    }

    while ((token = strtok_s(NULL, " \t\r\n", &saveptr)) != NULL) {
        if (count + 1 > cap) { cap *= 2; items = realloc(items, sizeof(Value) * cap); }
        items[count++] = value_str(token);
    }
    free(copy);
    if (count == 0) {
        free(items);
        return value_tns_new(TYPE_STR, 1, (const size_t[]){0});
    }
    size_t shape[1] = { count };
    Value out = value_tns_from_values(TYPE_STR, 1, shape, items, count);
    for (size_t i = 0; i < count; i++) value_free(items[i]);
    free(items);
    return out;
}

// IN (membership): IN(value, container)
// Only supports container of type TNS. Returns 1 if any element in the
// tensor is deeply equal to the provided value, otherwise 0. No special
// handling for STRs (no substring semantics).
static Value builtin_in(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 2) {
        RUNTIME_ERROR(interp, "IN requires two arguments", line, col);
    }

    // Container must be a tensor; otherwise membership is false
    if (args[1].type != VAL_TNS) {
        return value_int(0);
    }

    Tensor* t = args[1].as.tns;
    if (!t || t->length == 0) return value_int(0);

    for (size_t i = 0; i < t->length; i++) {
        if (value_deep_eq(args[0], t->data[i])) return value_int(1);
    }
    return value_int(0);
}

// IMPORT_PATH: import a module by explicit filesystem path (string)
static Value builtin_import_path(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "IMPORT_PATH expects a path string", line, col);
    }
    if (args[0].type != VAL_STR) {
        RUNTIME_ERROR(interp, "IMPORT_PATH first argument must be STR", line, col);
    }
    const char* inpath = args[0].as.s ? args[0].as.s : "";

    const char* alias = NULL;
    char* alias_dup = NULL;
    if (argc >= 2) {
        if (arg_nodes[1]->type != EXPR_IDENT) {
            RUNTIME_ERROR(interp, "IMPORT_PATH second argument must be an identifier (alias)", line, col);
        }
        alias = arg_nodes[1]->as.ident;
    } else {
        // Derive alias from basename of path (strip directories and extension)
        const char* p = inpath + strlen(inpath);
        while (p > inpath && *(p-1) != '/' && *(p-1) != '\\') p--;
        char base[512];
        strncpy(base, p, sizeof(base)-1);
        base[sizeof(base)-1] = '\0';
        char* dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        alias_dup = strdup(base);
        if (!alias_dup) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        alias = alias_dup;
    }

    // Determine if path is file or directory and resolve an import source path.
    struct stat st;
    char candidate[2048];
    char* found_path = NULL;

    if (stat(inpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        if (snprintf(candidate, sizeof(candidate), "%s/init.pre", inpath) >= 0) {
            if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                found_path = strdup(candidate);
            } else {
                if (alias_dup) free(alias_dup);
                RUNTIME_ERROR(interp, "IMPORT_PATH: package missing init.pre", line, col);
            }
        }
    } else {
        if (stat(inpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
            found_path = strdup(inpath);
        } else if (snprintf(candidate, sizeof(candidate), "%s.pre", inpath) >= 0) {
            if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                found_path = strdup(candidate);
            }
        }
    }

    char* canonical_path = found_path ? canonicalize_existing_path(found_path) : NULL;
    const char* cache_key = canonical_path ? canonical_path : inpath;

    // If the path did not resolve to an existing file/dir and the module
    // isn't already registered, treat this as a missing module error.
    if (!found_path) {
        Env* existing = module_env_lookup(interp, cache_key);
        if (!existing) {
            if (alias_dup) free(alias_dup);
            RUNTIME_ERROR(interp, "IMPORT_PATH: module not found", line, col);
        }
    }

    Env* mod_env = module_env_lookup(interp, cache_key);
    if (!mod_env) {
        if (module_register(interp, cache_key) != 0) {
            free(found_path);
            free(canonical_path);
            if (alias_dup) free(alias_dup);
            RUNTIME_ERROR(interp, "IMPORT_PATH failed to register module", line, col);
        }
        mod_env = module_env_lookup(interp, cache_key);
        if (!mod_env) {
            free(found_path);
            free(canonical_path);
            if (alias_dup) free(alias_dup);
            RUNTIME_ERROR(interp, "IMPORT_PATH failed to lookup module env", line, col);
        }
    }

    // Add aliases so equivalent spellings/path forms reuse the same cache entry.
    if (strcmp(inpath, cache_key) != 0) {
        (void)module_register_alias(interp, inpath, mod_env);
    }
    if (found_path && strcmp(found_path, cache_key) != 0) {
        (void)module_register_alias(interp, found_path, mod_env);
    }
    // Register provided alias name in module registry so callers can
    // refer to the module by that identifier (EXPORT relies on this).
    if (alias && strcmp(alias, cache_key) != 0) {
        (void)module_register_alias(interp, alias, mod_env);
    }

    // If not already loaded, execute module source once.
    EnvEntry* marker = env_get_entry(mod_env, "__MODULE_LOADED__");
    if ((!marker || !marker->initialized) && found_path) {
        FILE* f = fopen(found_path, "rb");
        char* srcbuf = NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            srcbuf = malloc((size_t)len + 1);
            if (!srcbuf) {
                fclose(f);
                free(found_path);
                free(canonical_path);
                if (alias_dup) free(alias_dup);
                RUNTIME_ERROR(interp, "Out of memory", line, col);
            }
            if (fread(srcbuf, 1, (size_t)len, f) != (size_t)len) {
                free(srcbuf);
                srcbuf = NULL;
            }
            if (srcbuf) {
                srcbuf[len] = '\0';
                fclose(f);

                env_assign(mod_env, "__MODULE_SOURCE__", value_str(cache_key), TYPE_STR, true);

                Lexer lex;
                lexer_init(&lex, srcbuf, found_path);
                Parser parser;
                parser_init(&parser, &lex);
                Stmt* program = parser_parse(&parser);
                if (parser.had_error) {
                    free(srcbuf);
                    free(found_path);
                    free(canonical_path);
                    if (alias_dup) free(alias_dup);
                    interp->error = strdup("IMPORT_PATH: parse error");
                    interp->error_line = parser.current_token.line;
                    interp->error_col = parser.current_token.column;
                    return value_null();
                }

                ExecResult res = exec_program_in_env(interp, program, mod_env);
                if (res.status == EXEC_ERROR) {
                    free(srcbuf);
                    free(found_path);
                    free(canonical_path);
                    if (res.error) interp->error = strdup(res.error);
                    interp->error_line = res.error_line;
                    interp->error_col = res.error_column;
                    free(res.error);
                    if (alias_dup) free(alias_dup);
                    return value_null();
                }

                env_assign(mod_env, "__MODULE_LOADED__", value_int(1), TYPE_INT, true);
                free(srcbuf);
            } else {
                fclose(f);
            }
        }
    }

    free(found_path);
    free(canonical_path);

    // Expose module symbols into caller env under alias prefix: alias.name -> value
    size_t alias_len = strlen(alias);
    for (size_t i = 0; i < mod_env->count; i++) {
        EnvEntry* e = &mod_env->entries[i];
        if (!e->initialized) continue;
        if (e->name && e->name[0] == '_' && e->name[1] == '_') continue;
        size_t qlen = alias_len + 1 + strlen(e->name) + 1;
        char* qualified = malloc(qlen);
        if (!qualified) { if (alias_dup) free(alias_dup); RUNTIME_ERROR(interp, "Out of memory", line, col); }
        snprintf(qualified, qlen, "%s.%s", alias, e->name);
        if (!env_assign(env, qualified, e->value, e->decl_type, true)) {
            free(qualified);
            if (alias_dup) free(alias_dup);
            RUNTIME_ERROR(interp, "IMPORT_PATH failed to assign qualified name", line, col);
        }
        free(qualified);
    }

    // Ensure the module name itself exists in caller env
    EnvEntry* alias_entry = env_get_entry(env, alias);
    if (!alias_entry) {
        if (!env_assign(env, alias, value_str("") , TYPE_STR, true)) {
            if (alias_dup) free(alias_dup);
            RUNTIME_ERROR(interp, "IMPORT_PATH failed to assign module name", line, col);
        }
    }

    if (alias_dup) free(alias_dup);
    return value_int(0);
}
static Value builtin_slice(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // SLICE per spec: SLICE(INT|STR: a, INT: start, INT: end)
    // INT -> bit-slice [start:end] (1-based, negatives from end)
    // STR -> inclusive char-slice counting from the left (index 1 = first char)
    if (args[0].type == VAL_INT) {
        EXPECT_INT(args[1], "SLICE", interp, line, col);
        EXPECT_INT(args[2], "SLICE", interp, line, col);

        int64_t v = args[0].as.i;
        uint64_t u = (v < 0) ? (uint64_t)(-v) : (uint64_t)v;

        // compute bit length (ILEN semantics)
        int64_t bitlen = 0;
        if (u == 0) bitlen = 1;
        else {
            uint64_t tmp = u;
            while (tmp > 0) { bitlen++; tmp >>= 1; }
        }

        int64_t start = args[1].as.i;
        int64_t end = args[2].as.i;
        if (start < 0) start = bitlen + start + 1;
        if (end < 0) end = bitlen + end + 1;

        if (start < 1) start = 1;
        if (end < 1) end = 1;
        if (start > bitlen) start = bitlen;
        if (end > bitlen) end = bitlen;

        // ensure start <= end for a non-empty inclusive slice
        if (start > end) return value_int(0);

        // convert positions (1-based from left/MSB) to bit indices (0-based from LSB)
        int64_t hi_bit = bitlen - start; // index of high bit (from LSB)
        int64_t lo_bit = bitlen - end; // index of low bit (from LSB)
        int64_t nbits = hi_bit - lo_bit + 1;

        uint64_t result = 0;
        if (nbits > 0) {
            // shift right by lo_bit then mask nbits
            result = (u >> lo_bit) & ((nbits >= 64) ? UINT64_MAX : ((1ULL << nbits) - 1ULL));
        }

        return value_int((int64_t)result);
    }

    if (args[0].type == VAL_STR) {
        EXPECT_INT(args[1], "SLICE", interp, line, col);
        EXPECT_INT(args[2], "SLICE", interp, line, col);
        const char* s = args[0].as.s;
        size_t len = strlen(s);
          /* Treat string slice arguments as start,end (first -> start, second -> end).
              This matches test usage where callers pass start then end positions. */
          int64_t start = args[1].as.i;
          int64_t end = args[2].as.i;
          if (start < 0) start = (int64_t)len + start + 1;
          if (end < 0) end = (int64_t)len + end + 1;

          if (start < 1) start = 1;
          if (end < 1) end = 1;
          if (start > (int64_t)len) start = (int64_t)len;
          if (end > (int64_t)len) end = (int64_t)len;

          /* inclusive indices: start..end */
          int64_t low_idx = start - 1;
          int64_t high_idx = end - 1;
          if (low_idx > high_idx) return value_str("");

        size_t result_len = (size_t)(high_idx - low_idx + 1);
        char* result = malloc(result_len + 1);
        if (!result) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        memcpy(result, s + low_idx, result_len);
        result[result_len] = '\0';
        Value v = value_str(result);
        free(result);
        return v;
    }

    RUNTIME_ERROR(interp, "SLICE expects INT or STR", line, col);
}

static Value builtin_replace(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "REPLACE", interp, line, col);
    EXPECT_STR(args[1], "REPLACE", interp, line, col);
    EXPECT_STR(args[2], "REPLACE", interp, line, col);
    
    const char* haystack = args[0].as.s;
    const char* needle = args[1].as.s;
    const char* replacement = args[2].as.s;
    
    size_t needle_len = strlen(needle);
    size_t repl_len = strlen(replacement);
    size_t haystack_len = strlen(haystack);
    
    if (needle_len == 0) {
        return value_str(haystack);
    }
    
    // Count occurrences
    size_t count = 0;
    const char* p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    
    if (count == 0) {
        return value_str(haystack);
    }
    
    size_t result_len = haystack_len + count * (repl_len - needle_len);
    char* result = malloc(result_len + 1);
    char* dst = result;
    p = haystack;
    const char* prev = haystack;
    
    while ((p = strstr(prev, needle)) != NULL) {
        size_t before = (size_t)(p - prev);
        memcpy(dst, prev, before);
        dst += before;
        memcpy(dst, replacement, repl_len);
        dst += repl_len;
        prev = p + needle_len;
    }
    strcpy(dst, prev);
    
    Value v = value_str(result);
    free(result);
    return v;
}

static Value builtin_strip(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "STRIP", interp, line, col);
    EXPECT_STR(args[1], "STRIP", interp, line, col);
    
    const char* s = args[0].as.s;
    const char* chars = args[1].as.s;
    size_t len = strlen(s);
    
    // Find start
    size_t start = 0;
    while (start < len && strchr(chars, s[start]) != NULL) {
        start++;
    }
    
    // Find end
    size_t end = len;
    while (end > start && strchr(chars, s[end - 1]) != NULL) {
        end--;
    }
    
    size_t result_len = end - start;
    char* result = malloc(result_len + 1);
    memcpy(result, s + start, result_len);
    result[result_len] = '\0';
    
    Value v = value_str(result);
    free(result);
    return v;
}

// ============ I/O operations ============

static Value builtin_print(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)line; (void)col;

    int forward = !(interp && interp->shushed);

    for (int i = 0; i < argc; i++) {
        if (i > 0 && forward) printf(" ");
        switch (args[i].type) {
            case VAL_INT: {
                char* s = int_to_binary_str(args[i].as.i);
                if (forward) printf("%s", s);
                free(s);
                break;
            }
            case VAL_FLT: {
                char* s = flt_to_binary_str(args[i].as.f);
                if (forward) printf("%s", s);
                free(s);
                break;
            }
            case VAL_STR:
                if (forward) printf("%s", args[i].as.s);
                break;
            case VAL_FUNC:
                if (forward) printf("<func %p>", (void*)args[i].as.func);
                break;
            default:
                if (forward) printf("<null>");
                break;
        }
    }
    if (forward) printf("\n");
    return value_int(0);
}

static Value builtin_input(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    if (argc >= 1) {
        EXPECT_STR(args[0], "INPUT", interp, line, col);
        printf("%s", args[0].as.s);
        fflush(stdout);
    }
    
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) != NULL) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }
        return value_str(buf);
    }
    return value_str("");
}

// SHUSH():INT - suppress forwarding of console output
static Value builtin_shush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    if (!interp) return value_int(0);
    interp->shushed = 1;
    return value_int(0);
}

// UNSHUSH():INT - re-enable forwarding of console output
static Value builtin_unshush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    if (!interp) return value_int(0);
    interp->shushed = 0;
    return value_int(0);
}

// CL: execute a command string using the host shell and return exit code
static Value builtin_cl(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "CL expects 1 argument", line, col);
    }
    EXPECT_STR(args[0], "CL", interp, line, col);
    const char* cmd = args[0].as.s;
    int rc;
    if (interp && interp->shushed) {
        // When shushed, suppress forwarding of command output by redirecting to null
#ifdef _WIN32
        const char* redir = " >NUL 2>&1";
#else
        const char* redir = " >/dev/null 2>&1";
#endif
        size_t n = strlen(cmd) + strlen(redir) + 1;
        char* tmp = malloc(n);
        if (!tmp) RUNTIME_ERROR(interp, "Out of memory", line, col);
        strcpy(tmp, cmd);
        strcat(tmp, redir);
        rc = system(tmp);
        free(tmp);
    } else {
        rc = system(cmd);
    }
    if (rc == -1) {
        RUNTIME_ERROR(interp, "Failed to invoke shell for CL", line, col);
    }
#ifdef WIFEXITED
    if (WIFEXITED(rc)) {
        return value_int(WEXITSTATUS(rc));
    }
#endif
    return value_int(rc);
}

// READFILE(STR: path, STR: coding = "UTF-8"):STR
static Value builtin_readfile(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "READFILE expects at least 1 argument", line, col);
    }
    EXPECT_STR(args[0], "READFILE", interp, line, col);
    const char* coding = "utf-8";
    if (argc >= 2) {
        EXPECT_STR(args[1], "READFILE", interp, line, col);
        coding = args[1].as.s;
    }

    // normalize coding to lowercase
    char codelb[64];
    size_t clen = strlen(coding);
    if (clen >= sizeof(codelb)) clen = sizeof(codelb)-1;
    for (size_t i = 0; i < clen; i++) codelb[i] = (char)tolower((unsigned char)coding[i]);
    codelb[clen] = '\0';

    FILE* f = fopen(args[0].as.s, "rb");
    if (!f) {
        RUNTIME_ERROR(interp, "READFILE: cannot open file", line, col);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); RUNTIME_ERROR(interp, "READFILE: seek failed", line, col); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); RUNTIME_ERROR(interp, "READFILE: ftell failed", line, col); }
    rewind(f);
    unsigned char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); RUNTIME_ERROR(interp, "Out of memory", line, col); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    (void)got; // allow binary files smaller than reported on some platforms

    // binary -> return bitstring
    if (strcmp(codelb, "binary") == 0 || strcmp(codelb, "bin") == 0) {
        // each byte -> 8 chars
        size_t outlen = (size_t)sz * 8;
        char* out = malloc(outlen + 1);
        if (!out) { free(buf); RUNTIME_ERROR(interp, "Out of memory", line, col); }
        size_t p = 0;
        for (size_t i = 0; i < (size_t)sz; i++) {
            unsigned char b = buf[i];
            for (int bit = 7; bit >= 0; bit--) {
                out[p++] = ((b >> bit) & 1) ? '1' : '0';
            }
        }
        out[p] = '\0';
        free(buf);
        Value v = value_str(out);
        free(out);
        return v;
    }

    // hex -> lowercase hex string
    if (strcmp(codelb, "hex") == 0 || strcmp(codelb, "hexadecimal") == 0) {
        static const char* hex = "0123456789abcdef";
        size_t outlen = (size_t)sz * 2;
        char* out = malloc(outlen + 1);
        if (!out) { free(buf); RUNTIME_ERROR(interp, "Out of memory", line, col); }
        size_t p = 0;
        for (size_t i = 0; i < (size_t)sz; i++) {
            unsigned char b = buf[i];
            out[p++] = hex[(b >> 4) & 0xf];
            out[p++] = hex[b & 0xf];
        }
        out[p] = '\0';
        free(buf);
        Value v = value_str(out);
        free(out);
        return v;
    }

    // Text modes: handle UTF-8 BOM strip
    size_t start = 0;
    if ((strcmp(codelb, "utf-8-bom") == 0 || strcmp(codelb, "utf-8 bom") == 0) && sz >= 3) {
        if (buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) start = 3;
    } else if (strcmp(codelb, "utf-8") == 0 && sz >= 3) {
        if (buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) start = 3;
    }

    // For other encodings (ANSI, UTF-16 LE/BE) we fall back to returning raw bytes as-is.
    size_t tlen = (size_t)sz - start;
    char* out = malloc(tlen + 1);
    if (!out) { free(buf); RUNTIME_ERROR(interp, "Out of memory", line, col); }
    memcpy(out, buf + start, tlen);
    out[tlen] = '\0';
    free(buf);
    Value v = value_str(out);
    free(out);
    return v;
}

// WRITEFILE(STR: blob, STR: path, STR: coding = "UTF-8"):INT
static Value builtin_writefile(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 2) {
        RUNTIME_ERROR(interp, "WRITEFILE expects at least 2 arguments", line, col);
    }
    EXPECT_STR(args[0], "WRITEFILE", interp, line, col);
    EXPECT_STR(args[1], "WRITEFILE", interp, line, col);
    const char* coding = "utf-8";
    if (argc >= 3) {
        EXPECT_STR(args[2], "WRITEFILE", interp, line, col);
        coding = args[2].as.s;
    }
    // normalize
    char codelb[64];
    size_t clen = strlen(coding);
    if (clen >= sizeof(codelb)) clen = sizeof(codelb)-1;
    for (size_t i = 0; i < clen; i++) codelb[i] = (char)tolower((unsigned char)coding[i]);
    codelb[clen] = '\0';

    const char* blob = args[0].as.s ? args[0].as.s : "";

    // binary
    if (strcmp(codelb, "binary") == 0 || strcmp(codelb, "bin") == 0) {
        size_t blen = strlen(blob);
        if (blen % 8 != 0) {
            RUNTIME_ERROR(interp, "WRITEFILE(binary) expects bitstring length multiple of 8", line, col);
        }
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_int(0);
        }
        for (size_t i = 0; i < blen; i += 8) {
            unsigned char byte = 0;
            for (int b = 0; b < 8; b++) {
                char c = blob[i+b];
                if (c != '0' && c != '1') { fclose(f); RUNTIME_ERROR(interp, "WRITEFILE(binary) expects only 0/1 characters", line, col); }
                byte = (byte << 1) | (unsigned char)(c - '0');
            }
            if (fwrite(&byte, 1, 1, f) != 1) { fclose(f); return value_int(0); }
        }
        fclose(f);
        return value_int(1);
    }

    // hex
    if (strcmp(codelb, "hex") == 0 || strcmp(codelb, "hexadecimal") == 0) {
        size_t blen = strlen(blob);
        if (blen % 2 != 0) RUNTIME_ERROR(interp, "WRITEFILE(hex) expects even-length hex string", line, col);
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_int(0);
        }
        for (size_t i = 0; i < blen; i += 2) {
            char a = blob[i]; char b = blob[i+1];
            int start = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
            int lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
            if (start < 0 || lo < 0) { fclose(f); RUNTIME_ERROR(interp, "WRITEFILE(hex) expects valid hex digits", line, col); }
            unsigned char byte = (unsigned char)((start << 4) | lo);
            if (fwrite(&byte, 1, 1, f) != 1) { fclose(f); return value_int(0); }
        }
        fclose(f);
        return value_int(1);
    }

    // Text encodings: write raw bytes; for utf-8-bom emit BOM
    FILE* f = fopen(args[1].as.s, "wb");
    if (!f) {
        // Try text mode as a fallback (may succeed on some platforms)
        fprintf(stderr, "WRITEFILE: open('%s','wb') failed: %s; trying text mode...\n", args[1].as.s, strerror(errno));
        f = fopen(args[1].as.s, "w");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_int(0);
        }
    }
    if (strcmp(codelb, "utf-8-bom") == 0 || strcmp(codelb, "utf-8 bom") == 0) {
        unsigned char bom[3] = {0xEF,0xBB,0xBF};
        if (fwrite(bom, 1, 3, f) != 3) { fclose(f); return value_int(0); }
    }
    size_t towrite = strlen(blob);
    if (towrite > 0) {
        if (fwrite(blob, 1, towrite, f) != towrite) { fclose(f); return value_int(0); }
    }
    fclose(f);
    return value_int(1);
}

// EXISTFILE(STR: path):INT
static Value builtin_existfile(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "EXISTFILE expects 1 argument", line, col);
    }
    EXPECT_STR(args[0], "EXISTFILE", interp, line, col);
    FILE* f = fopen(args[0].as.s, "rb");
    if (f) { fclose(f); return value_int(1); }
    return value_int(0);
}

// DELETEFILE(STR: path):INT
static Value builtin_deletefile(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "DELETEFILE expects 1 argument", line, col);
    }
    EXPECT_STR(args[0], "DELETEFILE", interp, line, col);
    if (remove(args[0].as.s) != 0) {
        RUNTIME_ERROR(interp, "DELETEFILE failed", line, col);
    }
    return value_int(1);
}

// ============ Control flow helpers ============

static Value builtin_assert(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (!value_truthiness(args[0])) {
        RUNTIME_ERROR(interp, "Assertion failed", line, col);
    }
    return value_int(1);
}

static Value builtin_throw(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc >= 1) {
        if (args[0].type == VAL_STR) {
            RUNTIME_ERROR(interp, args[0].as.s, line, col);
        }
    }
    RUNTIME_ERROR(interp, "Exception thrown", line, col);
}

// ============ Type checking ============

static Value builtin_isint(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(args[0].type == VAL_INT ? 1 : 0);
}

static Value builtin_isflt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(args[0].type == VAL_FLT ? 1 : 0);
}

static Value builtin_isstr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(args[0].type == VAL_STR ? 1 : 0);
}

static Value builtin_istns(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_int(args[0].type == VAL_TNS ? 1 : 0);
}

static Value builtin_type(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_str(value_type_name(args[0]));
}

// SIGNATURE(SYMBOL: sym):STR
static Value builtin_signature(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "SIGNATURE expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    EnvEntry* entry = env_get_entry(env, name);
    // Prefer environment entry if it exists and is initialized
    if (entry && entry->initialized) {
        if (entry->value.type == VAL_FUNC && entry->value.as.func != NULL) {
            struct Func* f = entry->value.as.func;
            // Build signature
            size_t cap = 256;
            char* buf = malloc(cap);
            if (!buf) RUNTIME_ERROR(interp, "Out of memory", line, col);
            buf[0] = '\0';
            strcat(buf, f->name ? f->name : name);
            strcat(buf, "(");
            for (size_t i = 0; i < f->params.count; i++) {
                Param p = f->params.items[i];
                const char* tname = "UNKNOWN";
                switch (p.type) {
                    case TYPE_INT: tname = "INT"; break;
                    case TYPE_FLT: tname = "FLT"; break;
                    case TYPE_STR: tname = "STR"; break;
                    case TYPE_TNS: tname = "TNS"; break;
                    case TYPE_MAP: tname = "MAP"; break;
                    case TYPE_FUNC: tname = "FUNC"; break;
                    case TYPE_THR: tname = "THR"; break;
                    default: tname = "ANY"; break;
                }
                if (i > 0) strcat(buf, ", ");
                strcat(buf, tname);
                strcat(buf, ": ");
                strcat(buf, p.name ? p.name : "");
                if (p.default_value != NULL) {
                    Value dv = eval_expr(interp, p.default_value, f->closure);
                    strcat(buf, " = ");
                    if (dv.type == VAL_STR) {
                        size_t need = strlen(buf) + strlen(dv.as.s) + 4;
                        if (need > cap) { cap = need * 2; buf = realloc(buf, cap); }
                        strcat(buf, "\"");
                        strcat(buf, dv.as.s);
                        strcat(buf, "\"");
                    } else if (dv.type == VAL_INT) {
                        char* s = int_to_binary_str(dv.as.i);
                        size_t need = strlen(buf) + strlen(s) + 2;
                        if (need > cap) { cap = need * 2; buf = realloc(buf, cap); }
                        strcat(buf, s);
                        free(s);
                    } else if (dv.type == VAL_FLT) {
                        char* s = flt_to_binary_str(dv.as.f);
                        size_t need = strlen(buf) + strlen(s) + 2;
                        if (need > cap) { cap = need * 2; buf = realloc(buf, cap); }
                        strcat(buf, s);
                        free(s);
                    } else {
                        const char* tn = value_type_name(dv);
                        size_t need = strlen(buf) + strlen(tn) + 2;
                        if (need > cap) { cap = need * 2; buf = realloc(buf, cap); }
                        strcat(buf, tn);
                    }
                    value_free(dv);
                }
            }
            strcat(buf, "):");
            const char* rname = "ANY";
            switch (f->return_type) {
                case TYPE_INT: rname = "INT"; break;
                case TYPE_FLT: rname = "FLT"; break;
                case TYPE_STR: rname = "STR"; break;
                case TYPE_TNS: rname = "TNS"; break;
                case TYPE_MAP: rname = "MAP"; break;
                case TYPE_FUNC: rname = "FUNC"; break;
                case TYPE_THR: rname = "THR"; break;
                default: rname = "ANY"; break;
            }
            strcat(buf, rname);
            Value out = value_str(buf);
            free(buf);
            return out;
        }
    }

    // Non-function: return "TYPE: name" using declared type if available
    if (!entry) {
        RUNTIME_ERROR(interp, "SIGNATURE: identifier not found or uninitialized", line, col);
    }
    const char* tname = "UNKNOWN";
    switch (entry->decl_type) {
        case TYPE_INT: tname = "INT"; break;
        case TYPE_FLT: tname = "FLT"; break;
        case TYPE_STR: tname = "STR"; break;
        case TYPE_TNS: tname = "TNS"; break;
        case TYPE_MAP: tname = "MAP"; break;
        case TYPE_FUNC: tname = "FUNC"; break;
        case TYPE_THR: tname = "THR"; break;
        default: tname = value_type_name(entry->value); break;
    }
    size_t len = strlen(tname) + 2 + strlen(name) + 1;
    char* res = malloc(len + 1);
    if (!res) RUNTIME_ERROR(interp, "Out of memory", line, col);
    snprintf(res, len + 1, "%s: %s", tname, name);
    Value out = value_str(res);
    free(res);
    return out;
}

// ============ Variable management ============

static Value builtin_del(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "DEL expects an identifier", line, col);
    }
    
    const char* name = arg_nodes[0]->as.ident;
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry || !entry->initialized) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot delete undefined identifier '%s'", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    if (entry->frozen || entry->permafrozen) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot delete frozen identifier '%s'", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    if (!env_delete(env, name)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot delete identifier '%s'", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int(0);
}

static Value builtin_freeze(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "FREEZE expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int r = env_freeze(env, name);
    if (r != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "FREEZE: identifier '%s' not found", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int(0);
}

static Value builtin_thaw(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "THAW expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int r = env_thaw(env, name);
    if (r == -1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "THAW: identifier '%s' not found", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    if (r == -2) {
        char buf[128];
        snprintf(buf, sizeof(buf), "THAW: identifier '%s' is permanently frozen", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int(0);
}

static Value builtin_permafreeze(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "PERMAFREEZE expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int r = env_permafreeze(env, name);
    if (r != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "PERMAFREEZE: identifier '%s' not found", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int(0);
}

static Value builtin_export(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 2 || arg_nodes[0]->type != EXPR_IDENT || arg_nodes[1]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "EXPORT expects two identifiers", line, col);
    }
    const char* sym = arg_nodes[0]->as.ident;
    const char* module = arg_nodes[1]->as.ident;

    // Find the symbol in caller environment
    EnvEntry* entry = env_get_entry(env, sym);
    if (!entry || !entry->initialized) {
        char buf[128];
        snprintf(buf, sizeof(buf), "EXPORT: identifier '%s' not found", sym);
        RUNTIME_ERROR(interp, buf, line, col);
    }

    // Lookup module env (must be previously imported)
    Env* mod_env = module_env_lookup(interp, module);
    if (!mod_env) {
        char buf[128];
        snprintf(buf, sizeof(buf), "EXPORT: module '%s' not imported", module);
        RUNTIME_ERROR(interp, buf, line, col);
    }

    // Assign into module's env under the plain symbol name
    if (!env_assign(mod_env, sym, entry->value, entry->decl_type, true)) {
        RUNTIME_ERROR(interp, "EXPORT failed to assign into module", line, col);
    }

    // Also create qualified name in caller env: module.symbol
    size_t len = strlen(module) + 1 + strlen(sym) + 1;
    char* qualified = malloc(len);
    if (!qualified) RUNTIME_ERROR(interp, "Out of memory", line, col);
    snprintf(qualified, len, "%s.%s", module, sym);
    if (!env_assign(env, qualified, entry->value, entry->decl_type, true)) {
        free(qualified);
        RUNTIME_ERROR(interp, "EXPORT failed to assign qualified name", line, col);
    }
    free(qualified);

    return value_int(0);
}

static Value builtin_frozen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "FROZEN expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int st = env_frozen_state(env, name);
    return value_int(st);
}

static Value builtin_permafrozen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "PERMAFROZEN expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int p = env_permafrozen(env, name);
    return value_int(p);
}

static Value builtin_exist(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)interp; (void)line; (void)col;
    
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        return value_int(0);
    }
    
    const char* name = arg_nodes[0]->as.ident;
    return value_int(env_exists(env, name) ? 1 : 0);
}

// ============ Variadic math ============

static Value builtin_sum(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "SUM requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t sum = 0;
        for (int i = 0; i < argc; i++) {
            EXPECT_INT(args[i], "SUM", interp, line, col);
            sum += args[i].as.i;
        }
        return value_int(sum);
    }
    if (args[0].type == VAL_FLT) {
        double sum = 0.0;
        for (int i = 0; i < argc; i++) {
            EXPECT_FLT(args[i], "SUM", interp, line, col);
            sum += args[i].as.f;
        }
        return value_flt(sum);
    }
    RUNTIME_ERROR(interp, "SUM expects INT or FLT arguments", line, col);
}

static Value builtin_prod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "PROD requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t prod = 1;
        for (int i = 0; i < argc; i++) {
            EXPECT_INT(args[i], "PROD", interp, line, col);
            prod *= args[i].as.i;
        }
        return value_int(prod);
    }
    if (args[0].type == VAL_FLT) {
        double prod = 1.0;
        for (int i = 0; i < argc; i++) {
            EXPECT_FLT(args[i], "PROD", interp, line, col);
            prod *= args[i].as.f;
        }
        return value_flt(prod);
    }
    RUNTIME_ERROR(interp, "PROD expects INT or FLT arguments", line, col);
}

static Value builtin_max(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "MAX requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t max = args[0].as.i;
        for (int i = 1; i < argc; i++) {
            EXPECT_INT(args[i], "MAX", interp, line, col);
            if (args[i].as.i > max) max = args[i].as.i;
        }
        return value_int(max);
    }
    if (args[0].type == VAL_FLT) {
        double max = args[0].as.f;
        for (int i = 1; i < argc; i++) {
            EXPECT_FLT(args[i], "MAX", interp, line, col);
            if (args[i].as.f > max) max = args[i].as.f;
        }
        return value_flt(max);
    }
    if (args[0].type == VAL_STR) {
        const char* max = args[0].as.s;
        size_t max_len = strlen(max);
        for (int i = 1; i < argc; i++) {
            EXPECT_STR(args[i], "MAX", interp, line, col);
            size_t len = strlen(args[i].as.s);
            if (len > max_len) {
                max = args[i].as.s;
                max_len = len;
            }
        }
        return value_str(max);
    }
    if (args[0].type == VAL_TNS) {
        // MAX(TNS: t1, ..., tN) -> flatten tensors and return largest scalar element
        // All tensors must have same scalar element type (INT/FLT/STR)
        Tensor* t0 = args[0].as.tns;
        DeclType etype = t0->elem_type;
        if (!(etype == TYPE_INT || etype == TYPE_FLT || etype == TYPE_STR)) {
            RUNTIME_ERROR(interp, "MAX TNS form requires scalar element types", line, col);
        }
        // verify all args are tensors with same element type
        for (int j = 0; j < argc; j++) {
            if (args[j].type != VAL_TNS) {
                RUNTIME_ERROR(interp, "MAX expects TNS arguments in this form", line, col);
            }
            if (args[j].as.tns->elem_type != etype) {
                RUNTIME_ERROR(interp, "MAX TNS arguments must share the same element type", line, col);
            }
        }
        // find first element to seed
        bool seeded = false;
        Value best = value_null();
        for (int j = 0; j < argc && !seeded; j++) {
            Tensor* tj = args[j].as.tns;
            for (size_t i = 0; i < tj->length; i++) {
                Value v = tj->data[i];
                if (etype == TYPE_INT && v.type == VAL_INT) { best = value_int(v.as.i); seeded = true; break; }
                if (etype == TYPE_FLT && v.type == VAL_FLT) { best = value_flt(v.as.f); seeded = true; break; }
                if (etype == TYPE_STR && v.type == VAL_STR) { best = value_str(v.as.s); seeded = true; break; }
                // skip non-matching elements (elem_type check above should prevent mismatches)
                continue;
            }
        }
        if (!seeded) {
            RUNTIME_ERROR(interp, "MAX requires non-empty tensors", line, col);
        }
        // compare remaining elements
        for (int j = 0; j < argc; j++) {
            Tensor* tj = args[j].as.tns;
            for (size_t i = 0; i < tj->length; i++) {
                Value v = tj->data[i];
                if (etype == TYPE_INT) {
                    EXPECT_INT(v, "MAX", interp, line, col);
                    if (v.as.i > best.as.i) { value_free(best); best = value_int(v.as.i); }
                } else if (etype == TYPE_FLT) {
                    EXPECT_FLT(v, "MAX", interp, line, col);
                    if (v.as.f > best.as.f) { value_free(best); best = value_flt(v.as.f); }
                } else { // STR
                    EXPECT_STR(v, "MAX", interp, line, col);
                    if (strlen(v.as.s) > strlen(best.as.s)) { value_free(best); best = value_str(v.as.s); }
                }
            }
        }
        return best;
    }
    RUNTIME_ERROR(interp, "MAX expects INT, FLT, or STR arguments", line, col);
}

static Value builtin_min(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "MIN requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t min = args[0].as.i;
        for (int i = 1; i < argc; i++) {
            EXPECT_INT(args[i], "MIN", interp, line, col);
            if (args[i].as.i < min) min = args[i].as.i;
        }
        return value_int(min);
    }
    if (args[0].type == VAL_FLT) {
        double min = args[0].as.f;
        for (int i = 1; i < argc; i++) {
            EXPECT_FLT(args[i], "MIN", interp, line, col);
            if (args[i].as.f < min) min = args[i].as.f;
        }
        return value_flt(min);
    }
    if (args[0].type == VAL_STR) {
        const char* min = args[0].as.s;
        size_t min_len = strlen(min);
        for (int i = 1; i < argc; i++) {
            EXPECT_STR(args[i], "MIN", interp, line, col);
            size_t len = strlen(args[i].as.s);
            if (len < min_len) {
                min = args[i].as.s;
                min_len = len;
            }
        }
        return value_str(min);
    }
    if (args[0].type == VAL_TNS) {
        // MIN(TNS: t1, ..., tN) -> flatten tensors and return smallest scalar element
        Tensor* t0 = args[0].as.tns;
        DeclType etype = t0->elem_type;
        if (!(etype == TYPE_INT || etype == TYPE_FLT || etype == TYPE_STR)) {
            RUNTIME_ERROR(interp, "MIN TNS form requires scalar element types", line, col);
        }
        for (int j = 0; j < argc; j++) {
            if (args[j].type != VAL_TNS) {
                RUNTIME_ERROR(interp, "MIN expects TNS arguments in this form", line, col);
            }
            if (args[j].as.tns->elem_type != etype) {
                RUNTIME_ERROR(interp, "MIN TNS arguments must share the same element type", line, col);
            }
        }
        bool seeded = false;
        Value best = value_null();
        for (int j = 0; j < argc && !seeded; j++) {
            Tensor* tj = args[j].as.tns;
            for (size_t i = 0; i < tj->length; i++) {
                Value v = tj->data[i];
                if (etype == TYPE_INT && v.type == VAL_INT) { best = value_int(v.as.i); seeded = true; break; }
                if (etype == TYPE_FLT && v.type == VAL_FLT) { best = value_flt(v.as.f); seeded = true; break; }
                if (etype == TYPE_STR && v.type == VAL_STR) { best = value_str(v.as.s); seeded = true; break; }
                // skip non-matching elements (elem_type check above should prevent mismatches)
                continue;
            }
        }
        if (!seeded) {
            RUNTIME_ERROR(interp, "MIN requires non-empty tensors", line, col);
        }
        for (int j = 0; j < argc; j++) {
            Tensor* tj = args[j].as.tns;
            for (size_t i = 0; i < tj->length; i++) {
                Value v = tj->data[i];
                if (etype == TYPE_INT) {
                    EXPECT_INT(v, "MIN", interp, line, col);
                    if (v.as.i < best.as.i) { value_free(best); best = value_int(v.as.i); }
                } else if (etype == TYPE_FLT) {
                    EXPECT_FLT(v, "MIN", interp, line, col);
                    if (v.as.f < best.as.f) { value_free(best); best = value_flt(v.as.f); }
                } else {
                    EXPECT_STR(v, "MIN", interp, line, col);
                    if (strlen(v.as.s) < strlen(best.as.s)) { value_free(best); best = value_str(v.as.s); }
                }
            }
        }
        return best;
    }
    RUNTIME_ERROR(interp, "MIN expects INT, FLT, or STR arguments", line, col);
}

static Value builtin_any(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    for (int i = 0; i < argc; i++) {
        if (value_truthiness(args[i])) return value_int(1);
    }
    return value_int(0);
}

static Value builtin_all(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    for (int i = 0; i < argc; i++) {
        if (!value_truthiness(args[i])) return value_int(0);
    }
    return value_int(1);
}

// Coercing sum/prod
static Value builtin_isum(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "ISUM requires at least one argument", line, col);
    }
    
    int64_t sum = 0;
    for (int i = 0; i < argc; i++) {
        EXPECT_NUM(args[i], "ISUM", interp, line, col);
        sum += args[i].type == VAL_INT ? args[i].as.i : (int64_t)args[i].as.f;
    }
    return value_int(sum);
}

static Value builtin_fsum(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "FSUM requires at least one argument", line, col);
    }
    
    double sum = 0.0;
    for (int i = 0; i < argc; i++) {
        EXPECT_NUM(args[i], "FSUM", interp, line, col);
        sum += args[i].type == VAL_FLT ? args[i].as.f : (double)args[i].as.i;
    }
    return value_flt(sum);
}

static Value builtin_iprod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "IPROD requires at least one argument", line, col);
    }
    
    int64_t prod = 1;
    for (int i = 0; i < argc; i++) {
        EXPECT_NUM(args[i], "IPROD", interp, line, col);
        prod *= args[i].type == VAL_INT ? args[i].as.i : (int64_t)args[i].as.f;
    }
    return value_int(prod);
}

static Value builtin_fprod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "FPROD requires at least one argument", line, col);
    }
    
    double prod = 1.0;
    for (int i = 0; i < argc; i++) {
        EXPECT_NUM(args[i], "FPROD", interp, line, col);
        prod *= args[i].type == VAL_FLT ? args[i].as.f : (double)args[i].as.i;
    }
    return value_flt(prod);
}

// ROUND
static Value builtin_round(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ROUND", interp, line, col);

    // Signature: ROUND(x, ndigits = 0, mode = "floor")
    int64_t places = 0;
    const char* mode = "floor";

    if (argc >= 2 && args[1].type != VAL_NULL) {
        EXPECT_INT(args[1], "ROUND", interp, line, col);
        places = args[1].as.i;
    }
    if (argc >= 3 && args[2].type != VAL_NULL) {
        if (args[2].type != VAL_STR) {
            RUNTIME_ERROR(interp, "ROUND expects STR mode", line, col);
        }
        mode = args[2].as.s;
        if (!mode) mode = "floor";
    }

    // INT behavior: keep prior semantics (ndigits >= 0 is a no-op; ndigits < 0 rounds toward zero to multiple of 2^(-ndigits)).
    if (args[0].type == VAL_INT) {
        if (places >= 0) {
            return value_int(args[0].as.i);
        }
        int64_t shift = -places;
        if (shift >= 63) {
            // 2^shift exceeds int64 range; rounding to such a large factor yields 0.
            return value_int(0);
        }
        int64_t factor = 1LL << shift;
        return value_int((args[0].as.i / factor) * factor);
    }

    double val = args[0].as.f;
    double factor = pow(2.0, (double)places);
    double scaled = val * factor;
    double rs;

    if (strcmp(mode, "floor") == 0) {
        rs = floor(scaled);
    } else if (strcmp(mode, "ceiling") == 0 || strcmp(mode, "ceil") == 0) {
        rs = ceil(scaled);
    } else if (strcmp(mode, "zero") == 0) {
        rs = (scaled >= 0.0) ? floor(scaled) : ceil(scaled);
    } else if (strcmp(mode, "logical") == 0 || strcmp(mode, "half-up") == 0) {
        rs = round(scaled);
    } else {
        RUNTIME_ERROR(interp, "Unknown ROUND mode", line, col);
    }

    return value_flt(rs / factor);
}

// INV (1/x)
static Value builtin_inv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    (void)interp; (void)line; (void)col;
    if (args[0].type == VAL_MAP) {
        // Map inversion: values become keys, keys become values
        Map* m = args[0].as.map;
        if (!m) return value_map_new();
        Value out = value_map_new();
        for (size_t i = 0; i < m->count; i++) {
            Value key = m->items[i].key; // original key
            Value val = m->items[i].value; // original value
            // Only scalar values may be used as keys
            if (val.type != VAL_INT && val.type != VAL_FLT && val.type != VAL_STR) {
                value_free(out);
                RUNTIME_ERROR(interp, "INV(map) requires scalar values", line, col);
            }
            // Check for duplicate values
            int found = 0;
            Value existing = value_map_get(out, val, &found);
            if (found) {
                value_free(existing);
                value_free(out);
                RUNTIME_ERROR(interp, "INV(map) contains duplicate values", line, col);
            }
            if (found == 0) value_free(existing);
            // Insert inverted pair: new_key = value, new_value = key
            value_map_set(&out, val, key);
        }
        return out;
    }

    // numeric inverse behavior preserved
    EXPECT_NUM(args[0], "INV", interp, line, col);
    if (args[0].type == VAL_INT) {
        if (args[0].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        if (args[0].as.i == 1) return value_int(1);
        if (args[0].as.i == -1) return value_int(-1);
        return value_int(0);
    }
    if (args[0].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    return value_flt(1.0 / args[0].as.f);
}

// KEYS(map):TNS - return 1-D tensor of keys in insertion order
static Value builtin_keys(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_MAP) RUNTIME_ERROR(interp, "KEYS expects MAP argument", line, col);
    Map* m = args[0].as.map;
    size_t count = m ? m->count : 0;
    if (count == 0) {
        size_t shape[1] = {0};
        return value_tns_new(TYPE_INT, 1, shape);
    }
    // determine key type
    ValueType kt = m->items[0].key.type;
    DeclType dt = TYPE_UNKNOWN;
    if (kt == VAL_INT) dt = TYPE_INT;
    else if (kt == VAL_FLT) dt = TYPE_FLT;
    else if (kt == VAL_STR) dt = TYPE_STR;
    else RUNTIME_ERROR(interp, "KEYS: unsupported key type", line, col);

    Value* items = malloc(sizeof(Value) * count);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < count; i++) {
        if (m->items[i].key.type != kt) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "KEYS: mixed key types in map", line, col);
        }
        items[i] = value_copy(m->items[i].key);
    }
    size_t shape[1] = { count };
    Value out = value_tns_from_values(dt, 1, shape, items, count);
    for (size_t i = 0; i < count; i++) value_free(items[i]);
    free(items);
    return out;
}

// VALUES(map):TNS - return 1-D tensor of values in insertion order (requires uniform element type)
static Value builtin_values(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_MAP) RUNTIME_ERROR(interp, "VALUES expects MAP argument", line, col);
    Map* m = args[0].as.map;
    size_t count = m ? m->count : 0;
    if (count == 0) {
        size_t shape[1] = {0};
        return value_tns_new(TYPE_INT, 1, shape);
    }
    // determine element DeclType from first value
    ValueType vt = m->items[0].value.type;
    DeclType dt = TYPE_UNKNOWN;
    if (vt == VAL_INT) dt = TYPE_INT;
    else if (vt == VAL_FLT) dt = TYPE_FLT;
    else if (vt == VAL_STR) dt = TYPE_STR;
    else if (vt == VAL_TNS) dt = TYPE_TNS;
    else if (vt == VAL_FUNC) dt = TYPE_FUNC;
    else if (vt == VAL_THR) dt = TYPE_THR;
    else if (vt == VAL_MAP) dt = TYPE_TNS; // no TYPE_MAP, use TNS as container type
    else RUNTIME_ERROR(interp, "VALUES: unsupported value type", line, col);

    Value* items = malloc(sizeof(Value) * count);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < count; i++) {
        Value v = m->items[i].value;
        // map all MAP values to TYPE_TNS element classification but keep actual Value
        ValueType cur = v.type;
        DeclType cur_dt = TYPE_UNKNOWN;
        if (cur == VAL_INT) cur_dt = TYPE_INT;
        else if (cur == VAL_FLT) cur_dt = TYPE_FLT;
        else if (cur == VAL_STR) cur_dt = TYPE_STR;
        else if (cur == VAL_TNS) cur_dt = TYPE_TNS;
        else if (cur == VAL_FUNC) cur_dt = TYPE_FUNC;
        else if (cur == VAL_THR) cur_dt = TYPE_THR;
        else if (cur == VAL_MAP) cur_dt = TYPE_TNS;
        if (cur_dt != dt) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "VALUES: mixed value types in map", line, col);
        }
        items[i] = value_copy(v);
    }
    size_t shape[1] = { count };
    Value out = value_tns_from_values(dt, 1, shape, items, count);
    for (size_t i = 0; i < count; i++) value_free(items[i]);
    free(items);
    return out;
}

// KEYIN(key, map):INT - returns 1 if map contains key (type+value)
static Value builtin_keyin(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[1].type != VAL_MAP) RUNTIME_ERROR(interp, "KEYIN expects MAP as second argument", line, col);
    int found = 0;
    Value res = value_map_get(args[1], args[0], &found);
    if (found) value_free(res);
    return value_int(found ? 1 : 0);
}

// VALUEIN(value, map):INT - returns 1 if any stored value equals the provided value
static Value builtin_valuein(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[1].type != VAL_MAP) RUNTIME_ERROR(interp, "VALUEIN expects MAP as second argument", line, col);
    Map* m = args[1].as.map;
    if (!m) return value_int(0);
    for (size_t i = 0; i < m->count; i++) {
        if (value_deep_eq(args[0], m->items[i].value)) return value_int(1);
    }
    return value_int(0);
}

// Helper: recursive match implementation
static int match_map_internal(Map* m, Map* tpl, int typing, int recurse, int shape) {
    if (!tpl) return 1;
    for (size_t i = 0; i < tpl->count; i++) {
        Value tkey = tpl->items[i].key;
        Value tval = tpl->items[i].value;
        // find key in m
        int found = 0;
        Value got = value_map_get((Value){ .type = VAL_MAP, .as.map = m }, tkey, &found);
        if (!found) { if (found) value_free(got); return 0; }
        Value mval = got;
        // typing: types must match
        if (typing && mval.type != tval.type) { value_free(mval); return 0; }
        // shape: if either side is TNS, both must be TNS and shapes identical
        if (shape) {
            if (mval.type == VAL_TNS || tval.type == VAL_TNS) {
                if (mval.type != VAL_TNS || tval.type != VAL_TNS) { value_free(mval); return 0; }
                Tensor* a = mval.as.tns;
                Tensor* b = tval.as.tns;
                if (a->ndim != b->ndim) { value_free(mval); return 0; }
                for (size_t d = 0; d < a->ndim; d++) { if (a->shape[d] != b->shape[d]) { value_free(mval); return 0; } }
            }
        }
        // recurse: if true and both are maps, apply recursively to the
        // corresponding nested template map (not to unrelated nested maps).
        if (recurse && mval.type == VAL_MAP && tval.type == VAL_MAP) {
            Map* mm = mval.as.map;
            Map* tt = tval.as.map;
            int ok = match_map_internal(mm, tt, typing, recurse, shape);
            value_free(mval);
            if (!ok) return 0;
        } else {
            value_free(mval);
        }
    }
    return 1;
}

// MATCH(map, template, typing=0, recurse=0, shape=0):INT
static Value builtin_match(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_MAP || args[1].type != VAL_MAP) RUNTIME_ERROR(interp, "MATCH expects two MAP arguments", line, col);
    int typing = 0, recurse = 0, shape = 0;
    if (argc >= 3) { if (args[2].type == VAL_INT) typing = args[2].as.i ? 1 : 0; }
    if (argc >= 4) { if (args[3].type == VAL_INT) recurse = args[3].as.i ? 1 : 0; }
    if (argc >= 5) { if (args[4].type == VAL_INT) shape = args[4].as.i ? 1 : 0; }
    Map* m = args[0].as.map;
    Map* tpl = args[1].as.map;
    int ok = match_map_internal(m, tpl, typing, recurse, shape);
    return value_int(ok ? 1 : 0);
}

// COPY (shallow copy for scalars)
static Value builtin_copy(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_copy(args[0]);
}

// DEEPCOPY: return a recursive deep copy of the argument
static Value builtin_deepcopy(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_deep_copy(args[0]);
}

// ASSIGN(target, expr): evaluate expr, assign into target lvalue, return assigned value
static Value builtin_assign(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
    if (!arg_nodes || !arg_nodes[0]) {
        RUNTIME_ERROR(interp, "ASSIGN: missing target expression", line, col);
    }

    Expr* target = arg_nodes[0];

    // RHS should have been evaluated into args[1]
    if (args == NULL) RUNTIME_ERROR(interp, "ASSIGN internal error", line, col);

    Value rhs = args[1];

    // Identifier target
    if (target->type == EXPR_IDENT) {
        const char* name = target->as.ident;
        EnvEntry* e = env_get_entry(env, name);
        if (!e) {
            RUNTIME_ERROR(interp, "ASSIGN requires target identifier to be declared", line, col);
        }
        // Check static type compatibility if present
        DeclType expected = e->decl_type;
        if (e->decl_type != TYPE_UNKNOWN) {
            DeclType actual;
            switch (rhs.type) {
                case VAL_INT: actual = TYPE_INT; break;
                case VAL_FLT: actual = TYPE_FLT; break;
                case VAL_STR: actual = TYPE_STR; break;
                case VAL_TNS: actual = TYPE_TNS; break;
                case VAL_MAP: actual = TYPE_MAP; break;
                case VAL_FUNC: actual = TYPE_FUNC; break;
                case VAL_THR: actual = TYPE_THR; break;
                default: actual = TYPE_UNKNOWN; break;
            }
            if (expected != actual) {
                RUNTIME_ERROR(interp, "ASSIGN: type mismatch", line, col);
            }
        }

        if (!env_assign(env, name, rhs, TYPE_UNKNOWN, false)) {
            RUNTIME_ERROR(interp, "ASSIGN: cannot assign to target (frozen?)", line, col);
        }
        return value_copy(rhs);
    }

    // Indexed target (e.g., tns[...], map<...>)
    if (target->type == EXPR_INDEX) {
        ExecResult res = assign_index_chain(interp, env, target, rhs, line, col);
        if (res.status == EXEC_ERROR) {
            if (res.error) {
                interp->error = strdup(res.error);
                interp->error_line = res.error_line;
                interp->error_col = res.error_column;
                free(res.error);
            }
            return value_null();
        }
        return value_copy(rhs);
    }

    RUNTIME_ERROR(interp, "ASSIGN: unsupported target expression", line, col);
}
// ILEN - integer length (number of bits)
static Value builtin_ilen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "ILEN", interp, line, col);
    
    int64_t v = args[0].as.i;
    if (v < 0) v = -v;
    if (v == 0) return value_int(1);
    
    int64_t len = 0;
    while (v > 0) {
        len++;
        v >>= 1;
    }
    return value_int(len);
}

// LEN (length - for now just strings)
static Value builtin_len(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "LEN requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_STR) {
        return value_int((int64_t)strlen(args[0].as.s));
    }
    if (args[0].type == VAL_TNS) {
        Tensor* t = args[0].as.tns;
        if (!t) return value_int(0);
        if (t->ndim == 0) return value_int(0);
        return value_int((int64_t)t->shape[0]);
    }
    RUNTIME_ERROR(interp, "LEN expects STR or TNS", line, col);
}

// Main, OS
static Value builtin_main(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)line; (void)col;
    // Determine module source for this call site (from env) and compare to interpreter primary source
    EnvEntry* call_src = env_get_entry(env, "__MODULE_SOURCE__");
    EnvEntry* primary_src = interp && interp->global_env ? env_get_entry(interp->global_env, "__MODULE_SOURCE__") : NULL;
    if (!primary_src || !primary_src->initialized) {
        // No recorded primary source -> treat as main
        return value_int(1);
    }
    if (!call_src || !call_src->initialized) {
        // Call site has no source recorded; treat as main if equal to primary (unlikely) else main
        return value_int(1);
    }
    if (call_src->value.type == VAL_STR && primary_src->value.type == VAL_STR && call_src->value.as.s && primary_src->value.as.s) {
        return value_int(strcmp(call_src->value.as.s, primary_src->value.as.s) == 0 ? 1 : 0);
    }
    return value_int(1);
}

static Value builtin_os(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
#ifdef _WIN32
    return value_str("win");
#elif defined(__APPLE__)
    return value_str("macos");
#elif defined(__linux__)
    return value_str("linux");
#else
    return value_str("unix"); // probably...
#endif
}

// Exit
static Value builtin_exit(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    int code = 0;
    if (argc >= 1) {
        EXPECT_INT(args[0], "EXIT", interp, line, col);
        code = (int)args[0].as.i;
    }
    exit(code);
    (void)code; // exit does not return
}

// Stubs for operations requiring TNS/MAP/THD
static Value builtin_import(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc;
    if (argc < 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "IMPORT expects a module identifier", line, col);
    }
    const char* modname = arg_nodes[0]->as.ident;
    const char* alias = NULL;
    if (argc >= 2) {
        if (arg_nodes[1]->type != EXPR_IDENT) {
            RUNTIME_ERROR(interp, "IMPORT second argument must be an identifier (alias)", line, col);
        }
        alias = arg_nodes[1]->as.ident;
    } else {
        alias = modname;
    }

    // Determine referring directory from caller env's __MODULE_SOURCE__ if present
    const char* referer_source = NULL;
    EnvEntry* src_entry = env_get_entry(env, "__MODULE_SOURCE__");
    if (src_entry && src_entry->initialized && src_entry->value.type == VAL_STR) {
        referer_source = src_entry->value.as.s;
    }

    char referer_dir[1024] = {0};
    if (referer_source && referer_source[0] != '\0') {
        strncpy(referer_dir, referer_source, sizeof(referer_dir)-1);
        char* last_sep = NULL;
        for (char* p = referer_dir; *p; p++) if (*p == '/' || *p == '\\') last_sep = p;
        if (last_sep) *last_sep = '\0';
    } else {
        strncpy(referer_dir, ".", sizeof(referer_dir)-1);
    }

    // Build base path by replacing '..' separators with platform path sep
#ifdef _WIN32
    const char PATH_SEP = '\\';
#else
    const char PATH_SEP = '/';
#endif
    char base[1024]; base[0] = '\0';
    const char* p = modname;
    char* b = base;
    while (*p && (size_t)(b - base) + 1 < sizeof(base)) {
        if (p[0] == '.' && p[1] == '.') { *b++ = PATH_SEP; p += 2; continue; }
        *b++ = *p++;
    }
    *b = '\0';

    struct stat st;
    char candidate[2048];
    char* found_path = NULL;
    char* srcbuf = NULL;

    // Search locations: referring dir, then primary-source lib/, then executable lib/.
    const char* search_dirs[3];
    search_dirs[0] = referer_dir;

    EnvEntry* primary_src_entry = interp && interp->global_env ? env_get_entry(interp->global_env, "__MODULE_SOURCE__") : NULL;
    char primary_lib_dir[1024];
    primary_lib_dir[0] = '\0';
    if (primary_src_entry && primary_src_entry->initialized && primary_src_entry->value.type == VAL_STR && primary_src_entry->value.as.s && primary_src_entry->value.as.s[0] != '\0') {
        strncpy(primary_lib_dir, primary_src_entry->value.as.s, sizeof(primary_lib_dir)-1);
        primary_lib_dir[sizeof(primary_lib_dir)-1] = '\0';
        char* last_sep = NULL;
        for (char* q = primary_lib_dir; *q; q++) if (*q == '/' || *q == '\\') last_sep = q;
        if (last_sep) *last_sep = '\0';
        size_t used = strnlen(primary_lib_dir, sizeof(primary_lib_dir));
        if (used + 4 < sizeof(primary_lib_dir)) {
            primary_lib_dir[used] = '/';
            primary_lib_dir[used+1] = 'l';
            primary_lib_dir[used+2] = 'i';
            primary_lib_dir[used+3] = 'b';
            primary_lib_dir[used+4] = '\0';
        }
        search_dirs[1] = primary_lib_dir;
    } else {
        search_dirs[1] = "lib";
    }

    char exe_lib_dir[1024];
    exe_lib_dir[0] = '\0';
    if (g_argv && g_argv[0] && g_argv[0][0] != '\0') {
        strncpy(exe_lib_dir, g_argv[0], sizeof(exe_lib_dir)-1);
        exe_lib_dir[sizeof(exe_lib_dir)-1] = '\0';
        char* last_sep = NULL;
        for (char* q = exe_lib_dir; *q; q++) if (*q == '/' || *q == '\\') last_sep = q;
        if (last_sep) *last_sep = '\0';
        size_t used = strnlen(exe_lib_dir, sizeof(exe_lib_dir));
        if (used + 4 < sizeof(exe_lib_dir)) {
            exe_lib_dir[used] = '/';
            exe_lib_dir[used+1] = 'l';
            exe_lib_dir[used+2] = 'i';
            exe_lib_dir[used+3] = 'b';
            exe_lib_dir[used+4] = '\0';
            search_dirs[2] = exe_lib_dir;
        } else {
            search_dirs[2] = NULL;
        }
    } else {
        search_dirs[2] = NULL;
    }

    for (int sd = 0; sd < 3 && !found_path; sd++) {
        const char* sdir = search_dirs[sd];
        if (!sdir) continue;

        if (snprintf(candidate, sizeof(candidate), "%s/%s", sdir, base) < 0) continue;
        if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
            char initpath[2048];
            if (snprintf(initpath, sizeof(initpath), "%s/%s/init.pre", sdir, base) < 0) continue;
            if (stat(initpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                found_path = strdup(initpath);
                break;
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "IMPORT: package '%s' missing init.pre", modname);
                RUNTIME_ERROR(interp, buf, line, col);
            }
        }

        char filepath[2048];
        if (snprintf(filepath, sizeof(filepath), "%s/%s.pre", sdir, base) < 0) continue;
        if (stat(filepath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
            found_path = strdup(filepath);
            break;
        }
    }

    char* canonical_path = found_path ? canonicalize_existing_path(found_path) : NULL;
    const char* cache_key = canonical_path ? canonical_path : modname;

    /* If we couldn't locate a file for the requested module and there is no
       previously-registered module with this name, report a clear error. */
    if (!found_path) {
        Env* existing = module_env_lookup(interp, cache_key);
        if (!existing) {
            free(found_path);
            free(canonical_path);
            char buf[256];
            snprintf(buf, sizeof(buf), "IMPORT: module '%s' not found", modname);
            RUNTIME_ERROR(interp, buf, line, col);
        }
    }

    /* Attempt to load a companion .prex pointer file next to the resolved
       module file so that any extension libraries listed there are available
       during module execution (e.g. lib/image/init.prex -> win32.dll). */
    if (found_path) {
        char noext[2048];
        strncpy(noext, found_path, sizeof(noext)-1);
        noext[sizeof(noext)-1] = '\0';
        char* dot = strrchr(noext, '.');
        if (dot) *dot = '\0';
        size_t need = strlen(noext) + strlen(".prex") + 1;
        char* companion_prex = malloc(need);
        if (companion_prex) {
            snprintf(companion_prex, need, "%s.prex", noext);
            char* ext_err = NULL;
            int loaded_any = 0;
            if (extensions_load_prex_if_exists(companion_prex, &loaded_any, &ext_err) != 0) {
                if (ext_err) {
                    interp->error = strdup(ext_err);
                    free(ext_err);
                } else {
                    interp->error = strdup("Failed to load companion .prex");
                }
                interp->error_line = line;
                interp->error_col = col;
                free(companion_prex);
                free(found_path);
                free(canonical_path);
                return value_null();
            }
            free(ext_err);
            free(companion_prex);
        }
    }

    Env* mod_env = module_env_lookup(interp, cache_key);
    if (!mod_env) mod_env = module_env_lookup(interp, modname);
    if (!mod_env) {
        if (module_register(interp, cache_key) != 0) {
            free(found_path);
            free(canonical_path);
            RUNTIME_ERROR(interp, "IMPORT failed to register module", line, col);
        }
        mod_env = module_env_lookup(interp, cache_key);
    }
    if (!mod_env) {
        free(found_path);
        free(canonical_path);
        RUNTIME_ERROR(interp, "IMPORT failed to lookup module env", line, col);
    }

    if (strcmp(modname, cache_key) != 0) {
        (void)module_register_alias(interp, modname, mod_env);
    }
    if (found_path && strcmp(found_path, cache_key) != 0) {
        (void)module_register_alias(interp, found_path, mod_env);
    }

    EnvEntry* marker = env_get_entry(mod_env, "__MODULE_LOADED__");
    if ((!marker || !marker->initialized) && found_path) {
        FILE* f = fopen(found_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            srcbuf = malloc((size_t)len + 1);
            if (!srcbuf) {
                fclose(f);
                free(found_path);
                free(canonical_path);
                RUNTIME_ERROR(interp, "Out of memory", line, col);
            }
            if (fread(srcbuf, 1, (size_t)len, f) != (size_t)len) {
                free(srcbuf);
                srcbuf = NULL;
            }
            if (srcbuf) {
                srcbuf[len] = '\0';
                fclose(f);

                env_assign(mod_env, "__MODULE_SOURCE__", value_str(cache_key), TYPE_STR, true);

                Lexer lex;
                lexer_init(&lex, srcbuf, found_path);
                Parser parser;
                parser_init(&parser, &lex);
                Stmt* program = parser_parse(&parser);
                if (parser.had_error) {
                    free(srcbuf);
                    free(found_path);
                    free(canonical_path);
                    interp->error = strdup("IMPORT: parse error");
                    interp->error_line = parser.current_token.line;
                    interp->error_col = parser.current_token.column;
                    return value_null();
                }

                ExecResult res = exec_program_in_env(interp, program, mod_env);
                if (res.status == EXEC_ERROR) {
                    free(srcbuf);
                    free(found_path);
                    free(canonical_path);
                    interp->error = res.error ? strdup(res.error) : strdup("Runtime error in IMPORT");
                    interp->error_line = res.error_line;
                    interp->error_col = res.error_column;
                    free(res.error);
                    return value_null();
                }

                env_assign(mod_env, "__MODULE_LOADED__", value_int(1), TYPE_INT, true);
                free(srcbuf);
            } else {
                fclose(f);
            }
        }
    }

    free(found_path);
    free(canonical_path);

    // Expose module symbols into caller env under alias prefix: alias.name -> value
    size_t alias_len = strlen(alias);

    for (size_t i = 0; i < mod_env->count; i++) {
        EnvEntry* e = &mod_env->entries[i];
        if (!e->initialized) continue;
        if (e->name && e->name[0] == '_' && e->name[1] == '_') continue; // skip magic
        size_t qlen = alias_len + 1 + strlen(e->name) + 1;
        char* qualified = malloc(qlen);
        if (!qualified) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        snprintf(qualified, qlen, "%s.%s", alias, e->name);
        if (!env_assign(env, qualified, e->value, e->decl_type, true)) {
            free(qualified);
            RUNTIME_ERROR(interp, "IMPORT failed to assign qualified name", line, col);
        }
        free(qualified);
    }

    // Ensure the module name itself exists in caller env (avoid undefined identifier errors)
    EnvEntry* alias_entry = env_get_entry(env, alias);
    if (!alias_entry) {
        if (!env_assign(env, alias, value_str("") , TYPE_STR, true)) {
            RUNTIME_ERROR(interp, "IMPORT failed to assign module name", line, col);
        }
    }

    return value_int(0);
}

// TNS operator: two forms
// 1) TNS(STR: string) -> 1-D TNS of STR single-character elements
// 2) TNS(TNS: shape, ANY: value) -> creates tensor with given shape filled with value
static Value builtin_tns(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc == 1) {
        // TNS(STR: string)
        if (args[0].type != VAL_STR) {
            RUNTIME_ERROR(interp, "TNS expects STR or (TNS, value)", line, col);
        }
        const char* s = args[0].as.s ? args[0].as.s : "";
        size_t n = strlen(s);
        if (n == 0) {
            return value_tns_new(TYPE_STR, 1, (const size_t[]){0});
        }
        Value* items = malloc(sizeof(Value) * n);
        if (!items) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        for (size_t i = 0; i < n; i++) {
            char buf[2] = { s[i], '\0' };
            items[i] = value_str(buf);
        }
        size_t shape[1] = { n };
        Value out = value_tns_from_values(TYPE_STR, 1, shape, items, n);
        for (size_t i = 0; i < n; i++) value_free(items[i]);
        free(items);
        return out;
    }

    if (argc == 2) {
        // TNS(TNS: shape, ANY: value)
        if (args[0].type != VAL_TNS) {
            RUNTIME_ERROR(interp, "TNS expects a 1-D TNS shape as first argument", line, col);
        }
        Tensor* shape_t = args[0].as.tns;
        if (!shape_t) {
            RUNTIME_ERROR(interp, "Invalid shape tensor", line, col);
        }
        if (shape_t->ndim != 1) {
            RUNTIME_ERROR(interp, "Shape tensor must be 1-D", line, col);
        }
        if (shape_t->elem_type != TYPE_INT) {
            RUNTIME_ERROR(interp, "Shape tensor must contain INT lengths", line, col);
        }
        // compute total length and build shape array
        size_t ndim = shape_t->shape[0];
        if (ndim == 0) {
            RUNTIME_ERROR(interp, "Shape tensor must have at least one element", line, col);
        }
        size_t* shape = malloc(sizeof(size_t) * ndim);
        if (!shape) { RUNTIME_ERROR(interp, "Out of memory", line, col); }
        size_t total = 1;
        for (size_t i = 0; i < ndim; i++) {
            Value v = shape_t->data[i];
            if (v.type != VAL_INT) { free(shape); RUNTIME_ERROR(interp, "Shape entries must be INT", line, col); }
            if (v.as.i <= 0) { free(shape); RUNTIME_ERROR(interp, "Shape lengths must be positive", line, col); }
            shape[i] = (size_t)v.as.i;
            // check overflow
            if (total > SIZE_MAX / shape[i]) { free(shape); RUNTIME_ERROR(interp, "Shape too large", line, col); }
            total *= shape[i];
        }

        // Prepare items filled with copies of the provided value
        Value* items = malloc(sizeof(Value) * total);
        if (!items) { free(shape); RUNTIME_ERROR(interp, "Out of memory", line, col); }
        for (size_t i = 0; i < total; i++) {
            if (args[1].type == VAL_MAP || args[1].type == VAL_TNS) {
                items[i] = value_deep_copy(args[1]);
            } else {
                items[i] = value_copy(args[1]);
            }
        }

        // Determine element DeclType
        DeclType elem_decl;
        switch (args[1].type) {
            case VAL_INT: elem_decl = TYPE_INT; break;
            case VAL_FLT: elem_decl = TYPE_FLT; break;
            case VAL_STR: elem_decl = TYPE_STR; break;
            case VAL_TNS: elem_decl = TYPE_TNS; break;
            case VAL_FUNC: elem_decl = TYPE_FUNC; break;
            default: elem_decl = TYPE_UNKNOWN; break;
        }

        Value out = value_tns_from_values(elem_decl, ndim, (const size_t*)shape, items, total);
        for (size_t i = 0; i < total; i++) value_free(items[i]);
        free(items);
        free(shape);
        return out;
    }

    RUNTIME_ERROR(interp, "TNS expects STR or (TNS shape, value)", line, col);
}

// ====== Tensor elementwise conversions: TINT, TFLT, TSTR ======
static Value builtin_tint(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "TINT expects TNS argument", line, col);
    }
    Tensor* t = args[0].as.tns;
    size_t n = t->length;
    Value* items = malloc(sizeof(Value) * n);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < n; i++) {
        Value elem = t->data[i];
        // Disallow nested tensors or functions
        if (elem.type == VAL_TNS || elem.type == VAL_FUNC) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "TINT requires scalar tensor elements", line, col);
        }
        Value arg0[1] = { elem };
        Value conv = builtin_int(interp, arg0, 1, NULL, NULL, line, col);
        if (interp->error) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            return value_null();
        }
        items[i] = conv;
    }
    Value out = value_tns_from_values(TYPE_INT, t->ndim, t->shape, items, n);
    for (size_t i = 0; i < n; i++) value_free(items[i]);
    free(items);
    return out;
}

static Value builtin_tflt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "TFLT expects TNS argument", line, col);
    }
    Tensor* t = args[0].as.tns;
    size_t n = t->length;
    Value* items = malloc(sizeof(Value) * n);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < n; i++) {
        Value elem = t->data[i];
        if (elem.type == VAL_TNS || elem.type == VAL_FUNC) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "TFLT requires scalar tensor elements", line, col);
        }
        Value arg0[1] = { elem };
        Value conv = builtin_flt(interp, arg0, 1, NULL, NULL, line, col);
        if (interp->error) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            return value_null();
        }
        items[i] = conv;
    }
    Value out = value_tns_from_values(TYPE_FLT, t->ndim, t->shape, items, n);
    for (size_t i = 0; i < n; i++) value_free(items[i]);
    free(items);
    return out;
}

static Value builtin_tstr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[0].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "TSTR expects TNS argument", line, col);
    }
    Tensor* t = args[0].as.tns;
    size_t n = t->length;
    Value* items = malloc(sizeof(Value) * n);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < n; i++) {
        Value elem = t->data[i];
        if (elem.type == VAL_TNS || elem.type == VAL_FUNC) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "TSTR requires scalar tensor elements", line, col);
        }
        Value arg0[1] = { elem };
        Value conv = builtin_str(interp, arg0, 1, NULL, NULL, line, col);
        if (interp->error) {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            return value_null();
        }
        items[i] = conv;
    }
    Value out = value_tns_from_values(TYPE_STR, t->ndim, t->shape, items, n);
    for (size_t i = 0; i < n; i++) value_free(items[i]);
    free(items);
    return out;
}

// ============ Builtins table ============
// Definitions for ARGV and RUN are placed here so the table can reference them.
// ARGV builtin: returns a 1-D TNS of STR containing process argv in order
static Value builtin_argv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)arg_nodes; (void)env; (void)argc;
    // Create a 1-D tensor of strings with length g_argc
    size_t n = (size_t)g_argc;
    if (n == 0) {
        // Return empty 1-D tensor
        size_t shape[1] = {0};
        return value_tns_new(TYPE_STR, 1, shape);
    }
    Value* items = malloc(sizeof(Value) * n);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);
    for (size_t i = 0; i < n; i++) {
        items[i] = value_str(g_argv[i]);
    }
    size_t shape[1]; shape[0] = n;
    Value out = value_tns_from_values(TYPE_STR, 1, shape, items, n);
    for (size_t i = 0; i < n; i++) value_free(items[i]);
    free(items);
    return out;
}

// RUN(STR: s) - parse and execute a Prefix program string within
// the current interpreter and environment.
static Value builtin_run(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)argc;
    EXPECT_STR(args[0], "RUN", interp, line, col);

    const char* src = args[0].as.s ? args[0].as.s : "";

    // Initialize lexer/parser on the provided string
    Lexer lex;
    lexer_init(&lex, src, "<string>");
    Parser parser;
    parser_init(&parser, &lex);

    Stmt* program = parser_parse(&parser);
    if (parser.had_error) {
        interp->error = strdup("RUN: parse error");
        interp->error_line = parser.current_token.line;
        interp->error_col = parser.current_token.column;
        return value_null();
    }

    // Execute parsed program in the caller's environment
    ExecResult res = exec_program_in_env(interp, program, env);
    if (res.status == EXEC_ERROR) {
        interp->error = res.error ? strdup(res.error) : strdup("Runtime error in RUN");
        interp->error_line = res.error_line;
        interp->error_col = res.error_column;
        free(res.error);
        return value_null();
    }

    return value_null();
}

// AWAIT(THR: thread):THR — block until thread is finished and return handle
static Value builtin_await(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "AWAIT expects 1 argument", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "AWAIT expects THR argument", line, col);
    }
    // Make a local copy of the thread handle to ensure the Thr
    // struct remains alive while we wait/join (prevents a race
    // where the worker could free the Thr between the check and
    // the join).
    Value ret = value_copy(args[0]);
    if (!value_thr_get_started(ret)) {
        return ret;
    }
    // Wait for worker to mark finished; yield while spinning to be cooperative
    while (!value_thr_get_finished(ret)) {
        thrd_yield();
    }
    // Join to reclaim thread resources; ignore join errors
    thrd_join(ret.as.thr->thread, NULL);
    return ret;
}

typedef struct {
    Value thr_val;
    double seconds;
} PauseTimer;

static int pause_timer_worker(void* arg) {
    PauseTimer* pt = (PauseTimer*)arg;
    if (pt->seconds >= 0) {
        time_t sec = (time_t)pt->seconds;
        double frac = pt->seconds - (double)sec;
        if (frac < 0) frac = 0;
        struct timespec ts;
        ts.tv_sec = sec;
        ts.tv_nsec = (long)(frac * 1000000000.0);
        thrd_sleep(&ts, NULL);
    }
    if (pt->thr_val.type == VAL_THR && pt->thr_val.as.thr) {
        value_thr_set_paused(pt->thr_val, 0);
    }
    value_free(pt->thr_val);
    free(pt);
    return 0;
}

// PAUSE(THR: thread, FLT: seconds=-1):THR
static Value builtin_pause(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1 || argc > 2) {
        RUNTIME_ERROR(interp, "PAUSE expects 1 or 2 arguments", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "PAUSE expects THR argument", line, col);
    }
    if (value_thr_get_finished(args[0])) {
        RUNTIME_ERROR(interp, "Cannot pause finished thread", line, col);
    }
    if (value_thr_get_paused(args[0])) {
        RUNTIME_ERROR(interp, "Thread already paused", line, col);
    }

    double seconds = -1.0;
    if (argc == 2) {
        if (args[1].type == VAL_FLT) {
            seconds = args[1].as.f;
        } else if (args[1].type == VAL_INT) {
            seconds = (double)args[1].as.i;
        } else {
            RUNTIME_ERROR(interp, "PAUSE expects FLT seconds", line, col);
        }
    }

    value_thr_set_paused(args[0], 1);

    if (seconds >= 0) {
        PauseTimer* pt = malloc(sizeof(PauseTimer));
        if (!pt) RUNTIME_ERROR(interp, "Out of memory", line, col);
        pt->thr_val = value_copy(args[0]);
        pt->seconds = seconds;
        thrd_t t;
        if (thrd_create(&t, pause_timer_worker, pt) != thrd_success) {
            value_free(pt->thr_val);
            free(pt);
            value_thr_set_paused(args[0], 0);
            RUNTIME_ERROR(interp, "Failed to schedule resume", line, col);
        }
        thrd_detach(t);
    }

    return value_copy(args[0]);
}

// RESUME(THR: thread):THR
static Value builtin_resume(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "RESUME expects 1 argument", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "RESUME expects THR argument", line, col);
    }
    if (!value_thr_get_paused(args[0])) {
        RUNTIME_ERROR(interp, "Thread is not paused", line, col);
    }
    value_thr_set_paused(args[0], 0);
    return value_copy(args[0]);
}

// PAUSED(THR: thread):INT
static Value builtin_paused(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "PAUSED expects 1 argument", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "PAUSED expects THR argument", line, col);
    }
    return value_int(value_thr_get_paused(args[0]) ? 1 : 0);
}

// STOP(THR: thread):THR — cooperatively stop a running thread and mark finished
static Value builtin_stop(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "STOP expects 1 argument", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "STOP expects THR argument", line, col);
    }
    if (value_thr_get_finished(args[0])) {
        return value_copy(args[0]);
    }
    value_thr_set_paused(args[0], 0);
    value_thr_set_finished(args[0], 1);
    return value_copy(args[0]);
}

// RESTART(THR: thread):THR — reinitialize and start executing the thread again
static Value builtin_restart(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "RESTART expects 1 argument", line, col);
    }
    if (args[0].type != VAL_THR || !args[0].as.thr) {
        RUNTIME_ERROR(interp, "RESTART expects THR argument", line, col);
    }
    Thr* th = args[0].as.thr;
    if (!th->body || !th->env) {
        RUNTIME_ERROR(interp, "Cannot restart: no stored thread body/env", line, col);
    }
    if (!value_thr_get_finished(args[0])) {
        RUNTIME_ERROR(interp, "Cannot restart running thread", line, col);
    }
    // Delegate to interpreter helper that knows how to launch thr_worker
    if (interpreter_restart_thread(interp, args[0], line, col) != 0) {
        // interpreter_restart_thread sets interp->error on failure
        RUNTIME_ERROR(interp, interp->error ? interp->error : "Failed to restart thread", line, col);
    }
    return value_copy(args[0]);
}

// PARALLEL(TNS: functions) or PARALLEL(FUNC, FUNC, ...):INT
typedef struct {
    Interpreter* interp;
    struct Func* func;
    char** errors;
    int index;
    int* err_lines;
    int* err_cols;
} ParallelStart;

static int parallel_worker(void* arg) {
    ParallelStart* ps = (ParallelStart*)arg;
    // Create a per-worker interpreter state similar to PARFOR
    Interpreter* thr_interp = ps->interp;

    // Prepare a call environment from the function's closure
    Env* call_env = env_create(ps->func->closure);

    // Execute the function body as a program block
    ExecResult res = exec_program_in_env(thr_interp, ps->func->body, call_env);

    if (res.status == EXEC_ERROR && res.error) {
        ps->errors[ps->index] = res.error; // transfer ownership
        if (ps->err_lines) ps->err_lines[ps->index] = res.error_line;
        if (ps->err_cols) ps->err_cols[ps->index] = res.error_column;
    } else {
        if (res.status == EXEC_RETURN || res.status == EXEC_OK || res.status == EXEC_GOTO) {
            value_free(res.value);
        }
        if (res.status == EXEC_ERROR && res.error) free(res.error);
    }

    env_free(call_env);
    free(thr_interp);
    free(ps);
    return 0;
}

static Value builtin_parallel(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // Gather elements: either a single tensor argument or variadic FUNC args
    Value* elems = NULL;
    size_t n = 0;

    if (argc == 1 && args[0].type == VAL_TNS) {
        Tensor* t = args[0].as.tns;
        n = t->length;
        elems = malloc(sizeof(Value) * n);
        if (!elems) RUNTIME_ERROR(interp, "Out of memory", line, col);
        for (size_t i = 0; i < n; i++) elems[i] = value_copy(t->data[i]);
    } else {
        if (argc < 1) {
            RUNTIME_ERROR(interp, "PARALLEL expects at least 1 argument", line, col);
        }
        n = (size_t)argc;
        elems = malloc(sizeof(Value) * n);
        if (!elems) RUNTIME_ERROR(interp, "Out of memory", line, col);
        for (size_t i = 0; i < n; i++) elems[i] = value_copy(args[i]);
    }

    // Validate all are functions
    for (size_t i = 0; i < n; i++) {
        if (elems[i].type != VAL_FUNC || !elems[i].as.func) {
            for (size_t j = 0; j < n; j++) value_free(elems[j]);
            free(elems);
            RUNTIME_ERROR(interp, "PARALLEL expects functions (either a tensor of FUNC or FUNC arguments)", line, col);
        }
    }

    // Prepare shared error collection and thread handles
    char** errors = calloc(n, sizeof(char*));
    int* err_lines = calloc(n, sizeof(int));
    int* err_cols = calloc(n, sizeof(int));
    thrd_t* threads = malloc(sizeof(thrd_t) * n);
    if (!errors || !err_lines || !err_cols || !threads) {
        if (errors) free(errors);
        if (err_lines) free(err_lines);
        if (err_cols) free(err_cols);
        if (threads) free(threads);
        for (size_t i = 0; i < n; i++) value_free(elems[i]);
        free(elems);
        RUNTIME_ERROR(interp, "Out of memory", line, col);
    }

    for (size_t i = 0; i < n; i++) {
        ParallelStart* ps = malloc(sizeof(ParallelStart));
        Interpreter* thr_interp = malloc(sizeof(Interpreter));
        if (!ps || !thr_interp) {
            if (ps) free(ps);
            if (thr_interp) free(thr_interp);
            errors[i] = strdup("Failed to allocate worker");
            continue;
        }
        *thr_interp = (Interpreter){0};
        thr_interp->global_env = interp->global_env;
        thr_interp->loop_depth = 0;
        thr_interp->error = NULL;
        thr_interp->error_line = 0;
        thr_interp->error_col = 0;
        thr_interp->in_try_block = interp->in_try_block;
        thr_interp->modules = interp->modules;
        thr_interp->shushed = interp->shushed;

        ps->interp = thr_interp;
        ps->func = elems[i].as.func;
        ps->errors = errors;
        ps->index = (int)i;
        ps->err_lines = err_lines;
        ps->err_cols = err_cols;

        if (thrd_create(&threads[i], parallel_worker, ps) != thrd_success) {
            // record failure as error string and clean up
            errors[i] = strdup("Failed to start PARALLEL worker");
            free(thr_interp);
            free(ps);
        }
    }

    // Join threads
    for (size_t i = 0; i < n; i++) {
        thrd_join(threads[i], NULL);
    }

    // Find first error
    char* first_err = NULL;
    int first_line = 0, first_col = 0;
    for (size_t i = 0; i < n; i++) {
        if (errors[i]) { first_err = errors[i]; first_line = err_lines[i]; first_col = err_cols[i]; break; }
    }

    // Cleanup
    for (size_t i = 0; i < n; i++) if (elems[i].type != VAL_NULL) value_free(elems[i]);
    free(elems);
    for (size_t i = 0; i < n; i++) if (errors[i] && errors[i] != first_err) free(errors[i]);
    free(errors);
    free(err_lines);
    free(err_cols);
    free(threads);

    if (first_err) {
        interp->error = strdup(first_err);
        interp->error_line = first_line ? first_line : line;
        interp->error_col = first_col ? first_col : col;
        free(first_err);
        return value_null();
    }

    return value_int(0);
}


static const char* builtin_params_round[] = {"x", "ndigits", "mode"};
static const char* builtin_params_bytes[] = {"x", "endian"};
static const char* builtin_params_split[] = {"s", "delimiter"};
static const char* builtin_params_match[] = {"value", "template", "typing", "recurse", "shape"};
static const char* builtin_params_readfile[] = {"path", "coding"};
static const char* builtin_params_writefile[] = {"data", "path", "coding"};
static const char* builtin_params_pause[] = {"thr", "seconds"};

static BuiltinFunction builtins_table[] = {
    // Arithmetic
    {"ADD", 2, 2, builtin_add},
    {"SUB", 2, 2, builtin_sub},
    {"MUL", 2, 2, builtin_mul},
    {"DIV", 2, 2, builtin_div},
    {"MOD", 2, 2, builtin_mod},
    {"POW", 2, 2, builtin_pow},
    {"NEG", 1, 1, builtin_neg},
    {"ABS", 1, 1, builtin_abs},
    {"ROOT", 2, 2, builtin_root},
    {"IROOT", 2, 2, builtin_iroot},
    {"FROOT", 2, 2, builtin_froot},
    {"LOG", 1, 1, builtin_log},
    {"CLOG", 1, 1, builtin_clog},
    {"GCD", 2, 2, builtin_gcd},
    {"LCM", 2, 2, builtin_lcm},
    {"INV", 1, 1, builtin_inv},
    {"ROUND", 1, 3, builtin_round, builtin_params_round, 3},

    // Coercing arithmetic
    {"IADD", 2, 2, builtin_iadd},
    {"ISUB", 2, 2, builtin_isub},
    {"IMUL", 2, 2, builtin_imul},
    {"IDIV", 2, 2, builtin_idiv},
    {"CDIV", 2, 2, builtin_cdiv},
    {"IPOW", 2, 2, builtin_ipow},
    {"FADD", 2, 2, builtin_fadd},
    {"FSUB", 2, 2, builtin_fsub},
    {"FMUL", 2, 2, builtin_fmul},
    {"FDIV", 2, 2, builtin_fdiv},
    {"FPOW", 2, 2, builtin_fpow},
    // Tensor elementwise operators
    {"TNS", 1, 2, builtin_tns},
    {"TINT", 1, 1, builtin_tint},
    {"TFLT", 1, 1, builtin_tflt},
    {"TSTR", 1, 1, builtin_tstr},
    {"CONV", 2, 2, builtin_conv},
    {"FILL", 2, 2, builtin_fill},
    {"TADD", 2, 2, builtin_tadd},
    {"TSUB", 2, 2, builtin_tsub},
    {"TMUL", 2, 2, builtin_tmul},
    {"TDIV", 2, 2, builtin_tdiv},
    {"TPOW", 2, 2, builtin_tpow},
    {"SHAPE", 1, 1, builtin_shape},
    {"TLEN", 2, 2, builtin_tlen},
    {"TFLIP", 2, 2, builtin_tflip},
    {"SCAT", 3, 3, builtin_scat},
    {"MADD", 2, 2, builtin_madd},
    {"MSUB", 2, 2, builtin_msub},
    {"MMUL", 2, 2, builtin_mmul},
    {"MDIV", 2, 2, builtin_mdiv},
    {"MSUM", 1, -1, builtin_msum},
    {"MPROD", 1, -1, builtin_mprod},

    // Comparison
    {"EQ", 2, 2, builtin_eq},
    {"GT", 2, 2, builtin_gt},
    {"LT", 2, 2, builtin_lt},
    {"GTE", 2, 2, builtin_gte},
    {"LTE", 2, 2, builtin_lte},

    // Logical
    {"AND", 2, 2, builtin_and},
    {"OR", 2, 2, builtin_or},
    {"XOR", 2, 2, builtin_xor},
    {"NOT", 1, 1, builtin_not},
    {"BOOL", 1, 1, builtin_bool},

    // Bitwise
    {"BAND", 2, 2, builtin_band},
    {"BOR", 2, 2, builtin_bor},
    {"BXOR", 2, 2, builtin_bxor},
    {"BNOT", 1, 1, builtin_bnot},
    {"SHL", 2, 2, builtin_shl},
    {"SHR", 2, 2, builtin_shr},

    // Type conversion
    {"INT", 1, 1, builtin_int},
    {"FLT", 1, 1, builtin_flt},
    {"STR", 1, 1, builtin_str},
    {"BYTES", 1, 2, builtin_bytes, builtin_params_bytes, 2},
    {"SER", 1, 1, builtin_ser},
    {"UNSER", 1, 1, builtin_unser},

    // Type checking
    {"ISINT", 1, 1, builtin_isint},
    {"ISFLT", 1, 1, builtin_isflt},
    {"ISSTR", 1, 1, builtin_isstr},
    {"ISTNS", 1, 1, builtin_istns},
    {"TYPE", 1, 1, builtin_type},
    {"SIGNATURE", 1, 1, builtin_signature},

    // String operations
    {"SLEN", 1, 1, builtin_slen},
    {"UPPER", 1, 1, builtin_upper},
    {"LOWER", 1, 1, builtin_lower},
    {"FLIP", 1, 1, builtin_flip},
    {"SLICE", 3, 3, builtin_slice},
    {"REPLACE", 3, 3, builtin_replace},
    {"STRIP", 2, 2, builtin_strip},
    {"JOIN", 1, -1, builtin_join},
    {"SPLIT", 1, 2, builtin_split, builtin_params_split, 2},
    {"IN", 2, 2, builtin_in},
    {"KEYS", 1, 1, builtin_keys},
    {"VALUES", 1, 1, builtin_values},
    {"KEYIN", 2, 2, builtin_keyin},
    {"VALUEIN", 2, 2, builtin_valuein},
    {"MATCH", 2, 5, builtin_match, builtin_params_match, 5},
    {"ILEN", 1, 1, builtin_ilen},
    {"LEN", 0, -1, builtin_len},

    // I/O
    {"PRINT", 0, -1, builtin_print},
    {"INPUT", 0, 1, builtin_input},
    {"SHUSH", 0, 0, builtin_shush},
    {"UNSHUSH", 0, 0, builtin_unshush},
    {"READFILE", 1, 2, builtin_readfile, builtin_params_readfile, 2},
    {"WRITEFILE", 2, 3, builtin_writefile, builtin_params_writefile, 3},
    {"CL", 1, 1, builtin_cl},
    {"EXISTFILE", 1, 1, builtin_existfile},
    {"DELETEFILE", 1, 1, builtin_deletefile},
    {"RUN", 1, 1, builtin_run},
    {"ARGV", 0, 0, builtin_argv},
    {"PARALLEL", 1, -1, builtin_parallel},
    {"AWAIT", 1, 1, builtin_await},
    {"PAUSE", 1, 2, builtin_pause, builtin_params_pause, 2},
    {"RESUME", 1, 1, builtin_resume},
    {"PAUSED", 1, 1, builtin_paused},
    {"STOP", 1, 1, builtin_stop},
    {"RESTART", 1, 1, builtin_restart},

    // Control
    {"ASSERT", 1, 1, builtin_assert},
    {"THROW", 0, -1, builtin_throw},

    // Variables
    {"DEL", 1, 1, builtin_del},
    {"FREEZE", 1, 1, builtin_freeze},
    {"THAW", 1, 1, builtin_thaw},
    {"PERMAFREEZE", 1, 1, builtin_permafreeze},
    {"FROZEN", 1, 1, builtin_frozen},
    {"PERMAFROZEN", 1, 1, builtin_permafrozen},
    {"EXIST", 1, 1, builtin_exist},
    {"COPY", 1, 1, builtin_copy},
    {"DEEPCOPY", 1, 1, builtin_deepcopy},
    {"ASSIGN", 2, 2, builtin_assign},

    // Variadic math
    {"SUM", 1, -1, builtin_sum},
    {"PROD", 1, -1, builtin_prod},
    {"MAX", 1, -1, builtin_max},
    {"MIN", 1, -1, builtin_min},
    {"ANY", 1, -1, builtin_any},
    {"ALL", 1, -1, builtin_all},
    {"ISUM", 1, -1, builtin_isum},
    {"FSUM", 1, -1, builtin_fsum},
    {"IPROD", 1, -1, builtin_iprod},
    {"FPROD", 1, -1, builtin_fprod},

    // System
    {"MAIN", 0, 0, builtin_main},
    {"OS", 0, 0, builtin_os},
    {"EXIT", 0, 1, builtin_exit},
    {"IMPORT", 1, 2, builtin_import},
    {"IMPORT_PATH", 1, 2, builtin_import_path},
    {"EXPORT", 2, 2, builtin_export},

    // Sentinel
    {NULL, 0, 0, NULL}
};

typedef struct DynamicBuiltin {
    BuiltinFunction fn;
    struct DynamicBuiltin* next;
} DynamicBuiltin;

static DynamicBuiltin* g_dynamic_builtins = NULL;

static BuiltinFunction* builtin_lookup_static(const char* name) {
    for (int i = 0; builtins_table[i].name != NULL; i++) {
        if (strcmp(builtins_table[i].name, name) == 0) {
            return &builtins_table[i];
        }
    }
    return NULL;
}

static BuiltinFunction* builtin_lookup_dynamic(const char* name) {
    for (DynamicBuiltin* n = g_dynamic_builtins; n != NULL; n = n->next) {
        if (n->fn.name && strcmp(n->fn.name, name) == 0) {
            return &n->fn;
        }
    }
    return NULL;
}

void builtins_reset_dynamic(void) {
    DynamicBuiltin* n = g_dynamic_builtins;
    while (n) {
        DynamicBuiltin* next = n->next;
        free((char*)n->fn.name);
        if (n->fn.param_names) {
            for (int i = 0; i < n->fn.param_count; i++) {
                free((char*)n->fn.param_names[i]);
            }
            free((void*)n->fn.param_names);
        }
        free(n);
        n = next;
    }
    g_dynamic_builtins = NULL;
}

int builtins_register_operator(const char* name, BuiltinImplFn impl, int min_args, int max_args, const char** param_names, int param_count) {
    if (!name || name[0] == '\0' || !impl) return -1;
    if (min_args < 0) return -1;
    if (max_args >= 0 && max_args < min_args) return -1;
    if (builtin_lookup_static(name) || builtin_lookup_dynamic(name)) {
        return -1;
    }

    DynamicBuiltin* node = calloc(1, sizeof(DynamicBuiltin));
    if (!node) return -1;

    node->fn.name = strdup(name);
    node->fn.min_args = min_args;
    node->fn.max_args = max_args;
    node->fn.impl = impl;
    node->fn.param_names = NULL;
    node->fn.param_count = 0;

    if (!node->fn.name) {
        free(node);
        return -1;
    }

    if (param_names && param_count > 0) {
        const char** copy_names = calloc((size_t)param_count, sizeof(char*));
        if (!copy_names) {
            free((char*)node->fn.name);
            free(node);
            return -1;
        }
        for (int i = 0; i < param_count; i++) {
            if (!param_names[i]) {
                copy_names[i] = strdup("");
            } else {
                copy_names[i] = strdup(param_names[i]);
            }
            if (!copy_names[i]) {
                for (int j = 0; j < i; j++) free((char*)copy_names[j]);
                free((void*)copy_names);
                free((char*)node->fn.name);
                free(node);
                return -1;
            }
        }
        node->fn.param_names = copy_names;
        node->fn.param_count = param_count;
    }

    node->next = g_dynamic_builtins;
    g_dynamic_builtins = node;
    return 0;
}

void builtins_init(void) {
    // Nothing to initialize for now, table is static
}

BuiltinFunction* builtin_lookup(const char* name) {
    BuiltinFunction* b = builtin_lookup_static(name);
    if (b) return b;
    return builtin_lookup_dynamic(name);
}

bool is_builtin(const char* name) {
    return builtin_lookup(name) != NULL;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
void builtins_set_argv(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
}
