#include "builtins.h"
#include "interpreter.h"
#include "ns_buffer.h"
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
#include <stdint.h>
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
static int module_export_bindings(Interpreter* interp, Env* caller_env, Env* mod_env, const char* alias, int line, int col, const char* fail_msg);

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
    /*
     * If this interpreter is running with isolated env writes (PARFOR
     * worker), ensure we create a local binding in the per-iteration
     * environment so writeback does not mutate parent bindings directly.
     */
    if (interp && interp->isolate_env_writes && env && env->parent) {
        /* create a local declaration if absent (use inferred decl type) */
        bool local_found = false;
        if (env->entries) {
            for (size_t __i = 0; __i < env->count; __i++) {
                EnvEntry* __e = &env->entries[__i];
                if (__e->name && strcmp(__e->name, name) == 0) { local_found = true; break; }
            }
        }
        if (!local_found) {
            /* best-effort: create a local (implicit) declaration to shadow parent.
             * Use TYPE_UNKNOWN for implicit builtins-created bindings so the PARFOR
             * merge step can distinguish explicit declarations (non-UNKNOWN)
             * from these ephemeral locals and skip merging them back.
             */
            if (!env_define(env, name, TYPE_UNKNOWN)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s writeback failed", rule);
                interp->error = strdup(buf);
                interp->error_line = line;
                interp->error_col = col;
                return false;
            }
        }
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

static bool writeback_ptr_node(Interpreter* interp, Expr* node, Env* env, Value result, const char* rule, int line, int col) {
    if (!node || node->type != EXPR_PTR) return true;
    const char* name = node->as.ptr_name;
    if (!name) {
        interp->error = strdup("Invalid pointer target");
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    /* See comment in writeback_first_ptr: create a local binding for
     * isolated-per-iteration environments so parent bindings are not
     * modified directly by concurrent iterations.
     */
    if (interp && interp->isolate_env_writes && env && env->parent) {
        bool local_found = false;
        if (env->entries) {
            for (size_t __i = 0; __i < env->count; __i++) {
                EnvEntry* __e = &env->entries[__i];
                if (__e->name && strcmp(__e->name, name) == 0) { local_found = true; break; }
            }
        }
        if (!local_found) {
            /* Create an implicit local entry (TYPE_UNKNOWN) so iterations
             * can write to a per-iteration copy without affecting parent.
             */
            if (!env_define(env, name, TYPE_UNKNOWN)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s writeback failed", rule);
                interp->error = strdup(buf);
                interp->error_line = line;
                interp->error_col = col;
                return false;
            }
        }
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

static bool writeback_ptr_range(Interpreter* interp, Expr** arg_nodes, Env* env, int start_index, int end_index, Value result, const char* rule, int line, int col) {
    if (!arg_nodes) return true;
    for (int i = start_index; i < end_index; ++i) {
        if (!writeback_ptr_node(interp, arg_nodes[i], env, result, rule, line, col)) {
            return false;
        }
    }
    return true;
}

// Checked FLT -> INT coercion helper.
// Returns true and sets *out on success. On failure, sets interp->error
// and returns false (caller should return value_null()).
static bool coerce_flt_to_int_checked(Interpreter* interp, double f, int64_t* out, const char* opname, int line, int col) {
    if (isnan(f) || isinf(f)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s cannot coerce FLT to INT", opname);
        interp->error = strdup(buf);
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    if (f > (double)INT64_MAX || f < (double)INT64_MIN) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s cannot coerce FLT to INT (out of range)", opname);
        interp->error = strdup(buf);
        interp->error_line = line;
        interp->error_col = col;
        return false;
    }
    *out = (int64_t)f;
    return true;
}

// --- Encoding helpers ---
static char* dec_latin1_to_utf8(const unsigned char* buf, size_t sz) {
    size_t outcap = sz * 2 + 1;
    char* out = malloc(outcap);
    if (!out) return NULL;
    size_t op = 0;
    for (size_t i = 0; i < sz; i++) {
        unsigned char b = buf[i];
        if (b < 0x80) {
            out[op++] = (unsigned char)b;
        } else {
            if (op + 2 + 1 > outcap) {
                outcap *= 2;
                char* n = realloc(out, outcap);
                if (!n) { free(out); return NULL; }
                out = n;
            }
            out[op++] = (unsigned char)(0xC0 | (b >> 6));
            out[op++] = (unsigned char)(0x80 | (b & 0x3F));
        }
    }
    out[op] = '\0';
    return out;
}

static const uint32_t cp1252_map[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static char* dec_cp1252_to_utf8(const unsigned char* buf, size_t sz) {
    size_t outcap = sz * 2 + 16;
    char* out = malloc(outcap);
    if (!out) return NULL;
    size_t op = 0;
    for (size_t i = 0; i < sz; i++) {
        uint32_t cp;
        unsigned char b = buf[i];
        if (b < 0x80) cp = b;
        else if (b >= 0xA0) cp = b;
        else cp = cp1252_map[b - 0x80];

        if (cp <= 0x7F) {
            out[op++] = (unsigned char)cp;
        } else if (cp <= 0x7FF) {
            if (op + 2 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xC0 | (cp >> 6));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            if (op + 3 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xE0 | (cp >> 12));
            out[op++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else {
            if (op + 4 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xF0 | (cp >> 18));
            out[op++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[op++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        }
    }
    out[op] = '\0';
    return out;
}

static char* dec_utf16_to_utf8(const unsigned char* buf, size_t sz, int little_endian) {
    size_t outcap = sz * 3 + 16;
    char* out = malloc(outcap);
    if (!out) return NULL;
    size_t op = 0;
    size_t i = 0;
    while (i + 1 < sz) {
        uint16_t cu;
        if (little_endian) cu = (uint16_t)(buf[i] | (buf[i+1] << 8));
        else cu = (uint16_t)((buf[i] << 8) | buf[i+1]);
        i += 2;
        uint32_t cp;
        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 1 < sz) {
                uint16_t cu2;
                if (little_endian) cu2 = (uint16_t)(buf[i] | (buf[i+1] << 8));
                else cu2 = (uint16_t)((buf[i] << 8) | buf[i+1]);
                if (cu2 >= 0xDC00 && cu2 <= 0xDFFF) {
                    i += 2;
                    cp = 0x10000 + (((uint32_t)cu - 0xD800) << 10) + ((uint32_t)cu2 - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cu >= 0xDC00 && cu <= 0xDFFF) {
            cp = 0xFFFD;
        } else {
            cp = cu;
        }

        if (cp <= 0x7F) {
            out[op++] = (unsigned char)cp;
        } else if (cp <= 0x7FF) {
            if (op + 2 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xC0 | (cp >> 6));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            if (op + 3 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xE0 | (cp >> 12));
            out[op++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else {
            if (op + 4 + 1 > outcap) { outcap *= 2; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            out[op++] = (unsigned char)(0xF0 | (cp >> 18));
            out[op++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[op++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[op++] = (unsigned char)(0x80 | (cp & 0x3F));
        }
    }
    if (i < sz) {
        // trailing byte -> replacement
        if (op + 3 + 1 > outcap) { outcap += 16; char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
        out[op++] = (unsigned char)0xEF; out[op++] = (unsigned char)0xBF; out[op++] = (unsigned char)0xBD;
    }
    out[op] = '\0';
    return out;
}

static unsigned char* enc_utf8_to_utf16(const char* s, size_t* out_sz, int little_endian) {
    size_t slen = s ? strlen(s) : 0;
    size_t outcap = (slen + 1) * 4 + 4;
    unsigned char* out = malloc(outcap);
    if (!out) return NULL;
    size_t op = 0;
    size_t i = 0;
    while (i < slen) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        size_t need = 0;
        if (c < 0x80) { cp = c; need = 1; i += 1; }
        else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= slen) { cp = 0xFFFD; i += 1; }
            else { cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F); i += 2; }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= slen) { cp = 0xFFFD; i += 1; }
            else { cp = ((c & 0x0F) << 12) | (((unsigned char)s[i+1] & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F); i += 3; }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= slen) { cp = 0xFFFD; i += 1; }
            else { cp = ((c & 0x07) << 18) | (((unsigned char)s[i+1] & 0x3F) << 12) | (((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F); i += 4; }
        } else { cp = 0xFFFD; i += 1; }

        if (cp <= 0xFFFF) {
            uint16_t cu = (uint16_t)cp;
            if (op + 2 + 2 > outcap) { outcap *= 2; unsigned char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            if (little_endian) { out[op++] = (unsigned char)(cu & 0xFF); out[op++] = (unsigned char)((cu >> 8) & 0xFF); }
            else { out[op++] = (unsigned char)((cu >> 8) & 0xFF); out[op++] = (unsigned char)(cu & 0xFF); }
        } else {
            // encode surrogate pair
            uint32_t v = cp - 0x10000;
            uint16_t hi = 0xD800 | (uint16_t)((v >> 10) & 0x3FF);
            uint16_t lo = 0xDC00 | (uint16_t)(v & 0x3FF);
            if (op + 4 + 2 > outcap) { outcap *= 2; unsigned char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
            if (little_endian) {
                out[op++] = (unsigned char)(hi & 0xFF); out[op++] = (unsigned char)((hi >> 8) & 0xFF);
                out[op++] = (unsigned char)(lo & 0xFF); out[op++] = (unsigned char)((lo >> 8) & 0xFF);
            } else {
                out[op++] = (unsigned char)((hi >> 8) & 0xFF); out[op++] = (unsigned char)(hi & 0xFF);
                out[op++] = (unsigned char)((lo >> 8) & 0xFF); out[op++] = (unsigned char)(lo & 0xFF);
            }
        }
    }
    *out_sz = op;
    return out;
}

/* Count Unicode code points in a UTF-8 string. Returns the number of
 * Unicode code points (characters) represented by the UTF-8 sequence.
 * Invalid sequences are treated as a single replacement character.
 */
static size_t utf8_codepoint_count(const char* s) {
    if (!s) return 0;
    const unsigned char* p = (const unsigned char*)s;
    size_t count = 0;

    while (*p) {
        unsigned char c = *p;
        uint32_t codepoint = 0;
        size_t seq_len = 0;

        if (c < 0x80) {
            codepoint = c; seq_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (p[1] != '\0' && (p[1] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
                seq_len = 2;
                if (codepoint < 0x80) seq_len = 0; /* overlong */
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (p[1] != '\0' && p[2] != '\0' && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
                seq_len = 3;
                if (codepoint < 0x800) seq_len = 0; /* overlong */
            }
        } else if ((c & 0xF8) == 0xF0) {
            if (p[1] != '\0' && p[2] != '\0' && p[3] != '\0' && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
                seq_len = 4;
                if (codepoint < 0x10000 || codepoint > 0x10FFFF) seq_len = 0; /* overlong or out of range */
            }
        }

        if (seq_len == 0) {
            /* invalid sequence -> advance one byte (counts as one character) */
            p++;
        } else {
            /* reject surrogate halves as invalid */
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                p++;
            } else {
                p += seq_len;
            }
        }

        count++;
    }

    return count;
}

static unsigned char* enc_utf8_to_cp1252(const char* s, size_t* out_sz, int is_windows) {
    size_t slen = s ? strlen(s) : 0;
    size_t outcap = slen + 16;
    unsigned char* out = malloc(outcap);
    if (!out) return NULL;
    size_t op = 0;
    size_t i = 0;
    while (i < slen) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; i++; }
        else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= slen) { cp = 0xFFFD; i++; }
            else { cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F); i += 2; }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= slen) { cp = 0xFFFD; i++; }
            else { cp = ((c & 0x0F) << 12) | (((unsigned char)s[i+1] & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F); i += 3; }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= slen) { cp = 0xFFFD; i++; }
            else { cp = ((c & 0x07) << 18) | (((unsigned char)s[i+1] & 0x3F) << 12) | (((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F); i += 4; }
        } else { cp = 0xFFFD; i++; }

        unsigned char outb = 0;
        if (cp <= 0x7F) outb = (unsigned char)cp;
        else if (cp >= 0x00A0 && cp <= 0x00FF) outb = (unsigned char)cp;
        else {
            int found = -1;
            for (int j = 0; j < 32; j++) {
                if (cp1252_map[j] == cp) { found = 0x80 + j; break; }
            }
            if (found >= 0) outb = (unsigned char)found;
            else { free(out); return NULL; }
        }

        if (op + 1 > outcap) { outcap *= 2; unsigned char* n = realloc(out, outcap); if (!n) { free(out); return NULL; } out = n; }
        out[op++] = outb;
    }
    *out_sz = op;
    return out;
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

static char* module_source_dir_dup(Env* env) {
    if (!env) return NULL;
    EnvEntry* src_entry = env_get_entry(env, "__MODULE_SOURCE__");
    if (!src_entry || !src_entry->initialized || src_entry->value.type != VAL_STR || !src_entry->value.as.s) {
        return NULL;
    }

    const char* src = src_entry->value.as.s;
    if (src[0] == '\0' || strcmp(src, "<string>") == 0 || strcmp(src, "<repl>") == 0) {
        return NULL;
    }

    char* dir = strdup(src);
    if (!dir) return NULL;

    char* last_sep = NULL;
    for (char* p = dir; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (last_sep) {
        *last_sep = '\0';
    } else {
        strcpy(dir, ".");
    }
    return dir;
}

static const char* module_scope_name(Env* env) {
    if (!env) return NULL;
    EnvEntry* scope_entry = env_get_entry(env, "__MODULE_SCOPE__");
    if (!scope_entry || !scope_entry->initialized || scope_entry->value.type != VAL_STR || !scope_entry->value.as.s) {
        return NULL;
    }
    return scope_entry->value.as.s[0] != '\0' ? scope_entry->value.as.s : NULL;
}

    // Global argv storage (set by main via builtins_set_argv)
    static int g_argc = 0;
    static char** g_argv = NULL;

    static const char* k_digits64 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+_";
    static const char* k_digits58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    static int is_valid_numeric_base(int base) {
        return base >= 2 && base <= 64;
    }

    static int digit_value_for_base(int base, char c) {
        const char* alphabet = (base == 58) ? k_digits58 : k_digits64;
        int limit = (base == 58) ? 58 : base;
        for (int i = 0; i < limit; i++) if (alphabet[i] == c) return i;
        return -1;
    }

    static const char* base_prefix_str(int base, char* buf, size_t buflen) {
        switch (base) {
            case 2: return "0b";
            case 8: return "0o";
            case 10: return "0d";
            case 16: return "0x";
            case 32: return "0t";
            case 58: return "0c";
            case 64: return "0s";
            default:
                snprintf(buf, buflen, "0r%02d", base);
                return buf;
        }
    }

    static int numeric_base_of(Value v) {
        if (v.type != VAL_INT && v.type != VAL_FLT) return 2;
        if (v.type == VAL_FLT && v.num_base_nan) return 0;
        return is_valid_numeric_base(v.num_base) ? v.num_base : 2;
    }

    static int result_base_from_values(Value a, Value b) {
        int ba = numeric_base_of(a);
        int bb = numeric_base_of(b);
        if (ba <= 0) return bb > 0 ? bb : 2;
        if (bb <= 0) return ba > 0 ? ba : 2;
        return ba > bb ? ba : bb;
    }

    static char* int_to_base_prefixed_str(int64_t val, int base) {
        if (!is_valid_numeric_base(base)) base = 2;
        const char* alphabet = (base == 58) ? k_digits58 : k_digits64;
        int is_negative = val < 0;
        uint64_t uval = is_negative ? (uint64_t)(-val) : (uint64_t)val;

        char digits_buf[160];
        int dpos = (int)sizeof(digits_buf) - 1;
        digits_buf[dpos--] = '\0';
        if (uval == 0) {
            digits_buf[dpos--] = '0';
        } else {
            while (uval > 0 && dpos >= 0) {
                int d = (int)(uval % (uint64_t)base);
                digits_buf[dpos--] = alphabet[d];
                uval /= (uint64_t)base;
            }
        }

        char pbuf[8];
        const char* pref = base_prefix_str(base, pbuf, sizeof(pbuf));
        size_t pref_len = strlen(pref);
        const char* digs = &digits_buf[dpos + 1];
        size_t digs_len = strlen(digs);
        size_t out_len = (is_negative ? 1 : 0) + pref_len + digs_len;
        char* out = malloc(out_len + 1);
        if (!out) { fprintf(stderr, "Out of memory\n"); exit(1); }
        size_t w = 0;
        if (is_negative) out[w++] = '-';
        memcpy(out + w, pref, pref_len); w += pref_len;
        memcpy(out + w, digs, digs_len); w += digs_len;
        out[w] = '\0';
        return out;
    }

    static char* flt_to_base_prefixed_str(double val, int base, int base_is_nan) {
        if (isinf(val)) return strdup(signbit(val) ? "-INF" : "INF");
        if (base_is_nan || isnan(val)) return strdup("NaN");
        if (!is_valid_numeric_base(base)) base = 2;
        const char* alphabet = (base == 58) ? k_digits58 : k_digits64;

        int is_negative = signbit(val) ? 1 : 0;
        double aval = is_negative ? -val : val;
        uint64_t int_part = (uint64_t)floor(aval);
        double frac_part = aval - (double)int_part;

        char int_digits[160];
        int ipos = (int)sizeof(int_digits) - 1;
        int_digits[ipos--] = '\0';
        if (int_part == 0) {
            int_digits[ipos--] = '0';
        } else {
            uint64_t x = int_part;
            while (x > 0 && ipos >= 0) {
                int d = (int)(x % (uint64_t)base);
                int_digits[ipos--] = alphabet[d];
                x /= (uint64_t)base;
            }
        }

        char frac_digits[96];
        int fpos = 0;
        for (int i = 0; i < 32 && frac_part > 0.0; i++) {
            frac_part *= (double)base;
            int d = (int)floor(frac_part + 1e-15);
            if (d < 0) d = 0;
            if (d >= base) d = base - 1;
            frac_digits[fpos++] = alphabet[d];
            frac_part -= (double)d;
            if (frac_part < 0.0) frac_part = 0.0;
        }
        while (fpos > 0 && frac_digits[fpos - 1] == '0') fpos--;
        frac_digits[fpos] = '\0';

        char pbuf[8];
        const char* pref = base_prefix_str(base, pbuf, sizeof(pbuf));
        const char* ints = &int_digits[ipos + 1];
        size_t out_len = (is_negative ? 1 : 0) + strlen(pref) + strlen(ints) + 2 + (fpos > 0 ? (size_t)fpos : 1);
        char* out = malloc(out_len + 1);
        if (!out) { fprintf(stderr, "Out of memory\n"); exit(1); }

        if (fpos == 0) {
            snprintf(out, out_len + 1, "%s%s%s.0", is_negative ? "-" : "", pref, ints);
        } else {
            snprintf(out, out_len + 1, "%s%s%s.%s", is_negative ? "-" : "", pref, ints, frac_digits);
        }
        return out;
    }

    static int parse_numeric_prefix(const char* s, int* out_base, const char** out_digits) {
        if (!s || s[0] != '0') return 0;
        int base = -1;
        int plen = 2;
        switch (s[1]) {
            case 'b': base = 2; break;
            case 'o': base = 8; break;
            case 'd': base = 10; break;
            case 'x': base = 16; break;
            case 't': base = 32; break;
            case 'c': base = 58; break;
            case 's': base = 64; break;
            case 'r':
                if (!isdigit((unsigned char)s[2]) || !isdigit((unsigned char)s[3])) return 0;
                base = (s[2] - '0') * 10 + (s[3] - '0');
                plen = 4;
                break;
            default:
                return 0;
        }
        if (!is_valid_numeric_base(base)) return 0;
        if (out_base) *out_base = base;
        if (out_digits) *out_digits = s + plen;
        return 1;
    }

    static int parse_prefixed_int_string(const char* text, int64_t* out_val, int* out_base) {
        if (!text || !out_val || !out_base) return 0;
        int neg = 0;
        const char* s = text;
        if (*s == '-') { neg = 1; s++; }
        int base = 2;
        const char* digits = NULL;
        if (!parse_numeric_prefix(s, &base, &digits) || !digits || !*digits) return 0;
        if (strchr(digits, '.')) return 0;

        int64_t acc = 0;
        for (const char* p = digits; *p; p++) {
            int dv = digit_value_for_base(base, *p);
            if (dv < 0 || dv >= base) return 0;
            if (acc > (INT64_MAX - dv) / base) return 0;
            acc = acc * base + dv;
        }
        *out_val = neg ? -acc : acc;
        *out_base = base;
        return 1;
    }

    static int parse_prefixed_flt_string(const char* text, double* out_val, int* out_base, int* out_base_is_nan) {
        if (!text || !out_val || !out_base || !out_base_is_nan) return 0;
        if (strcmp(text, "INF") == 0) { *out_val = INFINITY; *out_base = 0; *out_base_is_nan = 1; return 1; }
        if (strcmp(text, "-INF") == 0) { *out_val = -INFINITY; *out_base = 0; *out_base_is_nan = 1; return 1; }
        if (strcmp(text, "NaN") == 0) { *out_val = NAN; *out_base = 0; *out_base_is_nan = 1; return 1; }

        int neg = 0;
        const char* s = text;
        if (*s == '-') { neg = 1; s++; }
        int base = 2;
        const char* digits = NULL;
        if (!parse_numeric_prefix(s, &base, &digits) || !digits || !*digits) return 0;
        const char* dot = strchr(digits, '.');
        if (!dot || dot == digits || *(dot + 1) == '\0') return 0;

        double int_part = 0.0;
        for (const char* p = digits; p < dot; p++) {
            int dv = digit_value_for_base(base, *p);
            if (dv < 0 || dv >= base) return 0;
            int_part = int_part * (double)base + (double)dv;
        }
        double frac_part = 0.0;
        double weight = 1.0 / (double)base;
        for (const char* p = dot + 1; *p; p++) {
            int dv = digit_value_for_base(base, *p);
            if (dv < 0 || dv >= base) return 0;
            frac_part += (double)dv * weight;
            weight /= (double)base;
        }
        *out_val = neg ? -(int_part + frac_part) : (int_part + frac_part);
        *out_base = base;
        *out_base_is_nan = 0;
        return 1;
    }

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
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        unsigned char c = *p;
        /* Common single-byte escapes */
        if (c == '"') { jb_append_str(jb, "\\\""); p++; continue; }
        if (c == '\\') { jb_append_str(jb, "\\\\"); p++; continue; }
        if (c == '\b') { jb_append_str(jb, "\\b"); p++; continue; }
        if (c == '\f') { jb_append_str(jb, "\\f"); p++; continue; }
        if (c == '\n') { jb_append_str(jb, "\\n"); p++; continue; }
        if (c == '\r') { jb_append_str(jb, "\\r"); p++; continue; }
        if (c == '\t') { jb_append_str(jb, "\\t"); p++; continue; }

        /* Control characters must be escaped as \u00xx */
        if (c < 0x20) {
            jb_append_fmt(jb, "\\u%04x", (unsigned int)c);
            p++;
            continue;
        }

        /* Decode UTF-8 sequence into a Unicode code point. On invalid
         * sequences emit U+FFFD. For BMP code points emit \uHHHH, for
         * beyond-BMP emit \UHHHHHHHH per the specification. */
        uint32_t codepoint = 0;
        size_t seq_len = 0;
        if (c < 0x80) {
            codepoint = c;
            seq_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (p[1] != '\0' && (p[1] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
                seq_len = 2;
                if (codepoint < 0x80) seq_len = 0; /* overlong */
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (p[1] != '\0' && p[2] != '\0' && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
                seq_len = 3;
                if (codepoint < 0x800) seq_len = 0; /* overlong */
            }
        } else if ((c & 0xF8) == 0xF0) {
            if (p[1] != '\0' && p[2] != '\0' && p[3] != '\0' && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
                seq_len = 4;
                if (codepoint < 0x10000 || codepoint > 0x10FFFF) seq_len = 0; /* overlong or out of range */
            }
        }

        if (seq_len == 0) {
            /* invalid UTF-8 -> replacement character */
            codepoint = 0xFFFD;
            p++;
        } else {
            /* reject surrogate halves */
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                codepoint = 0xFFFD;
            } else {
                p += seq_len;
            }
        }

        if (codepoint < 0x80) {
            jb_append_char(jb, (char)codepoint);
        } else if (codepoint <= 0xFFFF) {
            jb_append_fmt(jb, "\\u%04x", (unsigned int)codepoint);
        } else {
            jb_append_fmt(jb, "\\U%08x", (unsigned int)codepoint);
        }
    }
    jb_append_char(jb, '"');
}

static const char* decl_type_name(DeclType dt) {
    switch (dt) {
        case TYPE_BOOL: return "BOOL";
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
    if (strcmp(name, "BOOL") == 0) return TYPE_BOOL;
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
                                    jb_append_char(&sb, (unsigned char)code);
                                } else if (code <= 0x7ff) {
                                    jb_append_char(&sb, (unsigned char)(0xC0 | ((code >> 6) & 0x1F)));
                                    jb_append_char(&sb, (unsigned char)(0x80 | (code & 0x3F)));
                                } else {
                                    jb_append_char(&sb, (unsigned char)(0xE0 | ((code >> 12) & 0x0F)));
                                    jb_append_char(&sb, (unsigned char)(0x80 | ((code >> 6) & 0x3F)));
                                    jb_append_char(&sb, (unsigned char)(0x80 | (code & 0x3F)));
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
    int* thr_state; // 0 = none, 1 = in_progress, 2 = done
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
    free(ctx->thr_state);
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

static const char* ser_thr_id(SerCtx* ctx, Thr* thr, int* state) {
    for (size_t i = 0; i < ctx->thr_count; i++) {
        if (ctx->thrs[i] == thr) {
            if (state) *state = ctx->thr_state[i];
            return ctx->thr_ids[i];
        }
    }
    if (ctx->thr_count + 1 > ctx->thr_cap) {
        size_t new_cap = ctx->thr_cap == 0 ? 4 : ctx->thr_cap * 2;
        ctx->thrs = realloc(ctx->thrs, new_cap * sizeof(Thr*));
        ctx->thr_ids = realloc(ctx->thr_ids, new_cap * sizeof(char*));
        ctx->thr_state = realloc(ctx->thr_state, new_cap * sizeof(int));
        if (!ctx->thrs || !ctx->thr_ids || !ctx->thr_state) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->thr_cap = new_cap;
    }
    ctx->next_thr_id++;
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d", ctx->next_thr_id);
    ctx->thrs[ctx->thr_count] = thr;
    ctx->thr_ids[ctx->thr_count] = strdup(buf);
    ctx->thr_state[ctx->thr_count] = 0;
    if (state) *state = 0;
    ctx->thr_count++;
    return ctx->thr_ids[ctx->thr_count - 1];
}

static void json_obj_field(JsonBuf* jb, bool* first, const char* key) {
    if (!*first) jb_append_char(jb, ',');
    *first = false;
    jb_append_json_string(jb, key);
    jb_append_char(jb, ':');
}

typedef struct {
    Thr* target;
    Map** seen_maps;
    size_t seen_map_count;
    size_t seen_map_cap;
    Tensor** seen_tensors;
    size_t seen_tensor_count;
    size_t seen_tensor_cap;
} ThrContainCtx;

static int thr_contain_seen_map(ThrContainCtx* ctx, Map* map) {
    for (size_t i = 0; i < ctx->seen_map_count; i++) {
        if (ctx->seen_maps[i] == map) return 1;
    }
    if (ctx->seen_map_count + 1 > ctx->seen_map_cap) {
        size_t new_cap = ctx->seen_map_cap == 0 ? 4 : ctx->seen_map_cap * 2;
        ctx->seen_maps = realloc(ctx->seen_maps, new_cap * sizeof(Map*));
        if (!ctx->seen_maps) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->seen_map_cap = new_cap;
    }
    ctx->seen_maps[ctx->seen_map_count++] = map;
    return 0;
}

static int thr_contain_seen_tensor(ThrContainCtx* ctx, Tensor* tns) {
    for (size_t i = 0; i < ctx->seen_tensor_count; i++) {
        if (ctx->seen_tensors[i] == tns) return 1;
    }
    if (ctx->seen_tensor_count + 1 > ctx->seen_tensor_cap) {
        size_t new_cap = ctx->seen_tensor_cap == 0 ? 4 : ctx->seen_tensor_cap * 2;
        ctx->seen_tensors = realloc(ctx->seen_tensors, new_cap * sizeof(Tensor*));
        if (!ctx->seen_tensors) { fprintf(stderr, "Out of memory\n"); exit(1); }
        ctx->seen_tensor_cap = new_cap;
    }
    ctx->seen_tensors[ctx->seen_tensor_count++] = tns;
    return 0;
}

static int value_contains_thr_rec(ThrContainCtx* ctx, Value v) {
    if (!ctx || !ctx->target) return 0;
    switch (v.type) {
        case VAL_THR:
            return v.as.thr == ctx->target;
        case VAL_MAP:
            if (!v.as.map) return 0;
            if (thr_contain_seen_map(ctx, v.as.map)) return 0;
            for (size_t i = 0; i < v.as.map->count; i++) {
                if (value_contains_thr_rec(ctx, v.as.map->items[i].key)) return 1;
                if (value_contains_thr_rec(ctx, v.as.map->items[i].value)) return 1;
            }
            return 0;
        case VAL_TNS:
            if (!v.as.tns) return 0;
            if (thr_contain_seen_tensor(ctx, v.as.tns)) return 0;
            for (size_t i = 0; i < v.as.tns->length; i++) {
                if (value_contains_thr_rec(ctx, v.as.tns->data[i])) return 1;
            }
            return 0;
        default:
            return 0;
    }
}

static int value_contains_thr(Value v, Thr* target) {
    ThrContainCtx ctx = {0};
    ctx.target = target;
    int found = value_contains_thr_rec(&ctx, v);
    free(ctx.seen_maps);
    free(ctx.seen_tensors);
    return found;
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

static void ser_env(JsonBuf* jb, SerCtx* ctx, Interpreter* interp, Env* env, Thr* omit_thr) {
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
        if (omit_thr && entry->initialized && !entry->alias_target && value_contains_thr(entry->value, omit_thr)) {
            continue;
        }
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
            ser_env(jb, ctx, interp, owner ? owner : env, omit_thr);
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
    ser_env(jb, ctx, interp, env->parent, omit_thr);

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
        case EXPR_BOOL: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Literal");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "value");
            jb_append_str(jb, expr->as.bool_value ? "true" : "false");
            json_obj_field(jb, &first, "literal_type");
            jb_append_json_string(jb, "BOOL");
            jb_append_char(jb, '}');
            return;
        }
        case EXPR_INT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "Literal");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "value");
            jb_append_fmt(jb, "%lld", (long long)expr->as.int_value.value);
            json_obj_field(jb, &first, "base");
            jb_append_fmt(jb, "%d", expr->as.int_value.base);
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
            if (isnan(expr->as.flt_value.value)) {
                jb_append_json_string(jb, "NaN");
            } else if (isinf(expr->as.flt_value.value)) {
                if (signbit(expr->as.flt_value.value)) jb_append_json_string(jb, "-INF");
                else jb_append_json_string(jb, "INF");
            } else {
                jb_append_fmt(jb, "%.17g", expr->as.flt_value.value);
            }
            json_obj_field(jb, &first, "base");
            if (expr->as.flt_value.base_is_nan) jb_append_str(jb, "null");
            else jb_append_fmt(jb, "%d", expr->as.flt_value.base);
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
        case EXPR_TYPED_IDENT: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "n");
            jb_append_json_string(jb, "TypedIdentifier");
            json_obj_field(jb, &first, "loc");
            ser_loc(jb, expr->line, expr->column);
            json_obj_field(jb, &first, "decl_type");
            jb_append_json_string(jb, decl_type_name(expr->as.typed_ident.decl_type));
            json_obj_field(jb, &first, "name");
            jb_append_json_string(jb, expr->as.typed_ident.name ? expr->as.typed_ident.name : "");
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
            jb_append_str(jb, expr->as.index.is_map ? "true" : "false");
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
                json_obj_field(jb, &pf, "coerced");
                jb_append_str(jb, p->coerced ? "true" : "false");
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
        case VAL_BOOL: {
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "BOOL");
            json_obj_field(jb, &first, "v");
            jb_append_str(jb, v.as.boolean ? "true" : "false");
            jb_append_char(jb, '}');
            return;
        }
        case VAL_INT: {
            char* s = int_to_base_prefixed_str(v.as.i, numeric_base_of(v));
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
            char* fs = flt_to_base_prefixed_str(v.as.f, numeric_base_of(v), v.num_base_nan);
            jb_append_char(jb, '{');
            bool first = true;
            json_obj_field(jb, &first, "t");
            jb_append_json_string(jb, "FLT");
            json_obj_field(jb, &first, "v");
            jb_append_json_string(jb, fs);
            jb_append_char(jb, '}');
            free(fs);
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
                json_obj_field(jb, &pf, "coerced");
                jb_append_str(jb, p->coerced ? "true" : "false");
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
                json_obj_field(jb, &pf, "coerced");
                jb_append_str(jb, p->coerced ? "true" : "false");
                json_obj_field(jb, &pf, "default");
                if (p->default_value) ser_expr(jb, ctx, interp, p->default_value);
                else jb_append_str(jb, "null");
                jb_append_char(jb, '}');
            }
            jb_append_char(jb, ']');
            json_obj_field(jb, &df, "body");
            ser_stmt(jb, ctx, interp, fn->body);
            json_obj_field(jb, &df, "closure");
            ser_env(jb, ctx, interp, fn->closure, NULL);
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
            int state = 0;
            const char* id = ser_thr_id(ctx, th, &state);
            (void)state;
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
            jb_append_str(jb, value_thr_get_stop_requested(thv) ? "true" : "false");
            json_obj_field(jb, &first, "env");
            ser_env(jb, ctx, interp, th->env, th);
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
    if (v.type == VAL_BOOL) return expr_bool(v.as.boolean, 1, 1);
    if (v.type == VAL_INT) return expr_int(v.as.i, v.num_base, 1, 1);
    if (v.type == VAL_FLT) return expr_flt(v.as.f, v.num_base, v.num_base_nan, 1, 1);
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
        if (strcmp(lt, "BOOL") == 0) {
                if (!val) { *err = "UNSER: invalid BOOL value"; return NULL; }
                if (val->type == JSON_BOOL) return expr_bool(val->as.boolean != 0, line, col);
                if (val->type == JSON_STR) {
                    if (strcmp(val->as.str, "TRUE") == 0 || strcmp(val->as.str, "true") == 0) return expr_bool(true, line, col);
                    if (strcmp(val->as.str, "FALSE") == 0 || strcmp(val->as.str, "false") == 0) return expr_bool(false, line, col);
                }
                *err = "UNSER: invalid BOOL value";
                return NULL;
        }
        if (strcmp(lt, "INT") == 0) {
            int64_t i = 0;
            if (val && val->type == JSON_NUM) i = (int64_t)val->as.num;
            JsonValue* basev = json_obj_get(obj, "base");
            int base = (basev && basev->type == JSON_NUM) ? (int)basev->as.num : 2;
            return expr_int(i, base, line, col);
        }
        if (strcmp(lt, "FLT") == 0) {
            double f = 0.0;
            if (val && val->type == JSON_NUM) f = val->as.num;
            else if (val && val->type == JSON_STR) f = strtod(val->as.str, NULL);
            JsonValue* basev = json_obj_get(obj, "base");
            if (!basev || basev->type == JSON_NULL) {
                return expr_flt(f, 0, 1, line, col);
            }
            int base = (basev->type == JSON_NUM) ? (int)basev->as.num : 2;
            return expr_flt(f, base, 0, line, col);
        }
        if (strcmp(lt, "STR") == 0) {
            const char* s = (val && val->type == JSON_STR) ? val->as.str : "";
            return expr_str(strdup(s), line, col);
        }
        return expr_int(0, 2, line, col);
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
    if (strcmp(name, "TypedIdentifier") == 0) {
        JsonValue* typev = json_obj_get(obj, "decl_type");
        JsonValue* nm = json_obj_get(obj, "name");
        const char* t = (typev && typev->type == JSON_STR) ? typev->as.str : "";
        const char* s = (nm && nm->type == JSON_STR) ? nm->as.str : "";
        return expr_typed_ident(decl_type_from_name(t), strdup(s), line, col);
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
        JsonValue* is_map_v = json_obj_get(obj, "is_map");
        bool is_map = false;
        if (is_map_v && is_map_v->type == JSON_BOOL) is_map = is_map_v->as.boolean ? true : false;
        Expr* idx = expr_index(base, line, col, is_map);
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
                JsonValue* pcoerced = json_obj_get(p, "coerced");
                JsonValue* pdef = json_obj_get(p, "default");
                Param pr;
                pr.name = strdup((pname && pname->type == JSON_STR) ? pname->as.str : "");
                pr.type = decl_type_from_name((ptype && ptype->type == JSON_STR) ? ptype->as.str : NULL);
                pr.coerced = (pcoerced && pcoerced->type == JSON_BOOL) ? (pcoerced->as.boolean != 0) : false;
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
        int64_t val = 0;
        int base = 2;
        if (!parse_prefixed_int_string(s, &val, &base)) {
            *err = "UNSER: invalid INT value";
            return value_null();
        }
        return value_int_base(val, base);
    }
    if (strcmp(tp, "BOOL") == 0) {
        JsonValue* v = json_obj_get(obj, "v");
        if (!v) { *err = "UNSER: invalid BOOL value"; return value_null(); }
        if (v->type == JSON_BOOL) return value_bool(v->as.boolean != 0);
        if (v->type == JSON_STR) {
            if (strcmp(v->as.str, "TRUE") == 0 || strcmp(v->as.str, "true") == 0) return value_bool(true);
            if (strcmp(v->as.str, "FALSE") == 0 || strcmp(v->as.str, "false") == 0) return value_bool(false);
        }
        *err = "UNSER: invalid BOOL value";
        return value_null();
    }
    if (strcmp(tp, "FLT") == 0) {
        JsonValue* v = json_obj_get(obj, "v");
        const char* s = (v && v->type == JSON_STR) ? v->as.str : "0.0";
        double f = 0.0;
        int base = 2;
        int base_is_nan = 0;
        if (!parse_prefixed_flt_string(s, &f, &base, &base_is_nan)) {
            *err = "UNSER: invalid FLT value";
            return value_null();
        }
        if (base_is_nan) return value_flt_nan_base(f);
        return value_flt_base(f, base);
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
        if (ndim == 0) { *err = "UNSER: invalid TNS shape"; return value_null(); }
        // Compute expected element count and validate dims
        size_t expected_total = 1;
        for (size_t i = 0; i < ndim; i++) {
            JsonValue* it = shape->as.arr.items[i];
            if (!it || it->type != JSON_NUM) { *err = "UNSER: invalid TNS shape"; return value_null(); }
            size_t sv = (size_t)it->as.num;
            if (sv == 0) { *err = "UNSER: invalid TNS shape"; return value_null(); }
            if (expected_total > 0 && sv > 0 && expected_total > (SIZE_MAX / sv)) { *err = "UNSER: TNS size overflow"; return value_null(); }
            expected_total *= sv;
        }
        size_t total = flat->as.arr.count;
        if (expected_total != total) { *err = "UNSER: invalid TNS element count"; return value_null(); }
        size_t* shp = malloc(sizeof(size_t) * ndim);
        if (!shp) { *err = "Out of memory"; return value_null(); }
        for (size_t i = 0; i < ndim; i++) shp[i] = (size_t)shape->as.arr.items[i]->as.num;
        Value* items = malloc(sizeof(Value) * (total > 0 ? total : 1));
        if (!items) { free(shp); *err = "Out of memory"; return value_null(); }
        DeclType elem_type = TYPE_UNKNOWN;
        for (size_t i = 0; i < total; i++) {
            items[i] = deser_val(flat->as.arr.items[i], ctx, interp, err);
            if (*err) { free(shp); free(items); return value_null(); }
            DeclType dt = TYPE_UNKNOWN;
            if (items[i].type == VAL_BOOL) dt = TYPE_BOOL;
            else if (items[i].type == VAL_INT) dt = TYPE_INT;
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
                    JsonValue* pc = json_obj_get(p, "coerced");
                    JsonValue* pd = json_obj_get(p, "default");
                    Param pr;
                    pr.name = strdup((pn && pn->type == JSON_STR) ? pn->as.str : "");
                    pr.type = decl_type_from_name((pt && pt->type == JSON_STR) ? pt->as.str : NULL);
                    pr.coerced = (pc && pc->type == JSON_BOOL) ? (pc->as.boolean != 0) : false;
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
        // Set lifecycle flags from serialized form. Default to not finished/not stopped/not started.
        JsonValue* finished_j = json_obj_get(obj, "finished");
        JsonValue* stop_j = json_obj_get(obj, "stop");
        JsonValue* paused_j = json_obj_get(obj, "paused");
        int finished_flag = (finished_j && finished_j->type == JSON_BOOL) ? finished_j->as.boolean : 0;
        int stop_flag = (stop_j && stop_j->type == JSON_BOOL) ? stop_j->as.boolean : 0;
        int paused_flag = (paused_j && paused_j->type == JSON_BOOL) ? paused_j->as.boolean : 0;
        value_thr_set_finished(thr, finished_flag);
        value_thr_set_stop_requested(thr, stop_flag);
        value_thr_set_paused(thr, paused_flag);
        value_thr_set_started(thr, 0);
        thr.as.thr->body = NULL;
        thr.as.thr->env = NULL;
        if (id) unser_thr_set(ctx, id, thr.as.thr);
        JsonValue* blk = json_obj_get(obj, "block");
        JsonValue* envv = json_obj_get(obj, "env");
        if (blk && blk->type == JSON_OBJ) thr.as.thr->body = deser_stmt(blk, ctx, interp, err);
        if (envv && envv->type == JSON_OBJ) thr.as.thr->env = deser_env(envv, ctx, interp, err);
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
    int out_base = result_base_from_values(args[0], args[1]);
    if (args[0].type == VAL_INT) {
        result = value_int_base(args[0].as.i + args[1].as.i, out_base);
    } else {
        result = value_flt_base(args[0].as.f + args[1].as.f, out_base);
    }
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "ADD", line, col)) {
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
    int out_base = result_base_from_values(args[0], args[1]);
    if (args[0].type == VAL_INT) {
        result = value_int_base(args[0].as.i - args[1].as.i, out_base);
    } else {
        result = value_flt_base(args[0].as.f - args[1].as.f, out_base);
    }
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "SUB", line, col)) {
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
    
    int out_base = result_base_from_values(args[0], args[1]);
    if (args[0].type == VAL_INT) {
        Value result = value_int_base(args[0].as.i * args[1].as.i, out_base);
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "MUL", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    }
    Value result = value_flt_base(args[0].as.f * args[1].as.f, out_base);
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "MUL", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_div(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "DIV", interp, line, col);
    EXPECT_NUM(args[1], "DIV", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "DIV cannot mix INT and FLT", line, col);
    }
    
    int out_base = result_base_from_values(args[0], args[1]);
    if (args[0].type == VAL_INT) {
        if (args[1].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        Value result = value_int_base(args[0].as.i / args[1].as.i, out_base);
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "DIV", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    }
    if (args[1].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    Value result = value_flt_base(args[0].as.f / args[1].as.f, out_base);
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "DIV", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

// CDIV: ceiling division (supports INT and FLT; returns same numeric type)
static Value builtin_cdiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "CDIV", interp, line, col);
    EXPECT_NUM(args[1], "CDIV", interp, line, col);

    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "CDIV cannot mix INT and FLT", line, col);
    }

    int out_base = result_base_from_values(args[0], args[1]);

    if (args[0].type == VAL_INT) {
        int64_t a = args[0].as.i;
        int64_t b = args[1].as.i;
        if (b == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        int64_t q = a / b;
        int64_t r = a % b;
        if (r != 0 && ((a > 0) == (b > 0))) q += 1;
        Value result = value_int_base(q, out_base);
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "CDIV", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    } else {
        if (args[1].as.f == 0.0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        double res = ceil(args[0].as.f / args[1].as.f);
        Value result = value_flt_base(res, out_base);
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "CDIV", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    }
}

static Value builtin_mod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)env;
    EXPECT_NUM(args[0], "MOD", interp, line, col);
    EXPECT_NUM(args[1], "MOD", interp, line, col);

    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "MOD cannot mix INT and FLT", line, col);
    }

    int out_base = result_base_from_values(args[0], args[1]);
    if (args[0].type == VAL_INT) {
        if (args[1].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        int64_t b = args[1].as.i < 0 ? -args[1].as.i : args[1].as.i;
        Value result = value_int_base(args[0].as.i % b, out_base);

        bool ok = true;
        if (arg_nodes && arg_nodes[0] && arg_nodes[0]->type == EXPR_PTR) {
            if (!writeback_ptr_node(interp, arg_nodes[0], env, result, "MOD", line, col)) ok = false;
        }
        /* Only write back into the divisor (arg_nodes[1]) when the dividend
           (arg_nodes[0]) is also a pointer literal — matching the spec and
           tests that expect MOD(a2, @b2) to NOT overwrite b2. */
        if (ok && arg_nodes && arg_nodes[1] && arg_nodes[1]->type == EXPR_PTR && arg_nodes[0] && arg_nodes[0]->type == EXPR_PTR) {
            if (!writeback_ptr_node(interp, arg_nodes[1], env, result, "MOD", line, col)) ok = false;
        }

        if (!ok) {
            value_free(result);
            return value_null();
        }
        return result;
    }

    if (args[1].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    double b = args[1].as.f < 0 ? -args[1].as.f : args[1].as.f;
    Value result = value_flt_base(fmod(args[0].as.f, b), out_base);

    bool ok = true;
    if (arg_nodes && arg_nodes[0] && arg_nodes[0]->type == EXPR_PTR) {
        if (!writeback_ptr_node(interp, arg_nodes[0], env, result, "MOD", line, col)) ok = false;
    }
    if (ok && arg_nodes && arg_nodes[1] && arg_nodes[1]->type == EXPR_PTR && arg_nodes[0] && arg_nodes[0]->type == EXPR_PTR) {
        if (!writeback_ptr_node(interp, arg_nodes[1], env, result, "MOD", line, col)) ok = false;
    }

    if (!ok) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_pow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "POW", interp, line, col);
    EXPECT_NUM(args[1], "POW", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "POW cannot mix INT and FLT", line, col);
    }
    
    int out_base = result_base_from_values(args[0], args[1]);
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
        Value out = value_int_base(result, out_base);
        if (!writeback_first_ptr(interp, arg_nodes, env, out, "POW", line, col)) {
            value_free(out);
            return value_null();
        }
        return out;
    }
    Value out = value_flt_base(pow(args[0].as.f, args[1].as.f), out_base);
    if (!writeback_first_ptr(interp, arg_nodes, env, out, "POW", line, col)) {
        value_free(out);
        return value_null();
    }
    return out;
}

static Value builtin_neg(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "NEG", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        Value result = value_int_base(-args[0].as.i, numeric_base_of(args[0]));
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 1, result, "NEG", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    }
    Value result = value_flt_base(-args[0].as.f, numeric_base_of(args[0]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 1, result, "NEG", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_abs(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ABS", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        Value result = value_int_base(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i, numeric_base_of(args[0]));
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 1, result, "ABS", line, col)) {
            value_free(result);
            return value_null();
        }
        return result;
    }
    Value result = value_flt_base(args[0].as.f < 0 ? -args[0].as.f : args[0].as.f, numeric_base_of(args[0]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 1, result, "ABS", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

// Coercing variants
static Value builtin_iadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IADD", interp, line, col);
    EXPECT_NUM(args[1], "IADD", interp, line, col);
    
    int64_t a;
    if (args[0].type == VAL_INT) {
        a = args[0].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &a, "IADD", line, col)) return value_null();
    }
    int64_t b;
    if (args[1].type == VAL_INT) {
        b = args[1].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[1].as.f, &b, "IADD", line, col)) return value_null();
    }
    Value result = value_int_base(a + b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "IADD", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_isub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ISUB", interp, line, col);
    EXPECT_NUM(args[1], "ISUB", interp, line, col);
    
    int64_t a;
    if (args[0].type == VAL_INT) {
        a = args[0].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &a, "ISUB", line, col)) return value_null();
    }
    int64_t b;
    if (args[1].type == VAL_INT) {
        b = args[1].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[1].as.f, &b, "ISUB", line, col)) return value_null();
    }
    Value result = value_int_base(a - b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "ISUB", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_imul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IMUL", interp, line, col);
    EXPECT_NUM(args[1], "IMUL", interp, line, col);
    
    int64_t a;
    if (args[0].type == VAL_INT) {
        a = args[0].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &a, "IMUL", line, col)) return value_null();
    }
    int64_t b;
    if (args[1].type == VAL_INT) {
        b = args[1].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[1].as.f, &b, "IMUL", line, col)) return value_null();
    }
    Value result = value_int_base(a * b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "IMUL", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_idiv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IDIV", interp, line, col);
    EXPECT_NUM(args[1], "IDIV", interp, line, col);
    
    int64_t a;
    if (args[0].type == VAL_INT) {
        a = args[0].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &a, "IDIV", line, col)) return value_null();
    }
    int64_t b;
    if (args[1].type == VAL_INT) {
        b = args[1].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[1].as.f, &b, "IDIV", line, col)) return value_null();
    }
    if (b == 0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    Value result = value_int_base(a / b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "IDIV", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_fadd(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
    EXPECT_NUM(args[0], "FADD", interp, line, col);
    EXPECT_NUM(args[1], "FADD", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    Value result = value_flt_base(a + b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "FADD", line, col)) {
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
    Value result = value_flt_base(a - b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "FSUB", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_fmul(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FMUL", interp, line, col);
    EXPECT_NUM(args[1], "FMUL", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    Value result = value_flt_base(a * b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "FMUL", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
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
    Value result = value_flt_base(a / b, result_base_from_values(args[0], args[1]));
    if (!writeback_ptr_range(interp, arg_nodes, env, 0, 2, result, "FDIV", line, col)) {
        value_free(result);
        return value_null();
    }
    return result;
}

static Value builtin_ipow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "IPOW", interp, line, col);
    EXPECT_NUM(args[1], "IPOW", interp, line, col);
    
    int64_t base;
    if (args[0].type == VAL_INT) {
        base = args[0].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &base, "IPOW", line, col)) return value_null();
    }
    int64_t exp;
    if (args[1].type == VAL_INT) {
        exp = args[1].as.i;
    } else {
        if (!coerce_flt_to_int_checked(interp, args[1].as.f, &exp, "IPOW", line, col)) return value_null();
    }
    if (exp < 0) {
        RUNTIME_ERROR(interp, "Negative exponent not supported", line, col);
    }
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    Value out = value_int_base(result, result_base_from_values(args[0], args[1]));
    if (!writeback_first_ptr(interp, arg_nodes, env, out, "IPOW", line, col)) {
        value_free(out);
        return value_null();
    }
    return out;
}

static Value builtin_fpow(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FPOW", interp, line, col);
    EXPECT_NUM(args[1], "FPOW", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    Value out = value_flt_base(pow(a, b), result_base_from_values(args[0], args[1]));
    if (!writeback_first_ptr(interp, arg_nodes, env, out, "FPOW", line, col)) {
        value_free(out);
        return value_null();
    }
    return out;
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
    (void)arg_nodes; (void)env;
    if (args[0].type != VAL_TNS || args[1].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "CONV expects (TNS, TNS)", line, col);
    }
    Tensor* x = args[0].as.tns;
    Tensor* k = args[1].as.tns;

    // Extended 2-D multi-output form triggered when more than two arguments provided
    if (argc > 2) {
        if (x->ndim != 3) {
            RUNTIME_ERROR(interp, "CONV extended form requires input rank 3", line, col);
        }
        if (k->ndim != 4) {
            RUNTIME_ERROR(interp, "CONV extended form requires kernel rank 4", line, col);
        }

        size_t in_w = x->shape[0];
        size_t in_h = x->shape[1];
        size_t in_c = x->shape[2];
        size_t kw = k->shape[0];
        size_t kh = k->shape[1];
        size_t k_in_c = k->shape[2];
        size_t out_c = k->shape[3];

        if (k_in_c != in_c) {
            RUNTIME_ERROR(interp, "CONV kernel input channels must match x channels", line, col);
        }

        // Element types must be numeric
        if (!((x->elem_type == TYPE_INT || x->elem_type == TYPE_FLT) && (k->elem_type == TYPE_INT || k->elem_type == TYPE_FLT))) {
            RUNTIME_ERROR(interp, "CONV only supports INT or FLT element types", line, col);
        }

        // Parse optional args: stride_w, stride_h, pad_w, pad_h, bias
        int64_t stride_w = 1, stride_h = 1, pad_w = 0, pad_h = 0;
        if (argc > 2 && args[2].type != VAL_NULL) { EXPECT_INT(args[2], "CONV", interp, line, col); stride_w = args[2].as.i; }
        if (argc > 3 && args[3].type != VAL_NULL) { EXPECT_INT(args[3], "CONV", interp, line, col); stride_h = args[3].as.i; }
        if (argc > 4 && args[4].type != VAL_NULL) { EXPECT_INT(args[4], "CONV", interp, line, col); pad_w = args[4].as.i; }
        if (argc > 5 && args[5].type != VAL_NULL) { EXPECT_INT(args[5], "CONV", interp, line, col); pad_h = args[5].as.i; }

        if (stride_w <= 0 || stride_h <= 0 || pad_w < 0 || pad_h < 0) {
            RUNTIME_ERROR(interp, "CONV invalid stride/pad", line, col);
        }

        bool bias_present = false;
        Tensor* bias_t = NULL;
        if (argc > 6 && args[6].type != VAL_NULL) {
            if (args[6].type != VAL_TNS) {
                RUNTIME_ERROR(interp, "CONV bias must be TNS", line, col);
            }
            bias_present = true;
            bias_t = args[6].as.tns;
            if ((bias_t->ndim != 1 && bias_t->length != 0) || (bias_t->length != 0 && bias_t->shape[0] != out_c)) {
                RUNTIME_ERROR(interp, "CONV bias size mismatch", line, col);
            }
        }

        // Output typing
        DeclType out_decl = (x->elem_type == TYPE_INT && k->elem_type == TYPE_INT) ? TYPE_INT : TYPE_FLT;

        // Compute output shape
        int64_t out_w_i = ((int64_t)in_w + 2 * pad_w - (int64_t)kw) / stride_w + 1;
        int64_t out_h_i = ((int64_t)in_h + 2 * pad_h - (int64_t)kh) / stride_h + 1;
        if (out_w_i <= 0 || out_h_i <= 0) {
            size_t out_shape_zero[3] = {0, 0, out_c};
            return value_tns_new(out_decl, 3, out_shape_zero);
        }
        size_t out_w = (size_t)out_w_i;
        size_t out_h = (size_t)out_h_i;

        size_t out_shape[3]; out_shape[0] = out_w; out_shape[1] = out_h; out_shape[2] = out_c;
        Value out = value_tns_new(out_decl, 3, out_shape);
        Tensor* ot = out.as.tns;

        // Perform convolution: output indices order [w,h,oc]
        for (size_t ow = 0; ow < out_w; ow++) {
            for (size_t oh = 0; oh < out_h; oh++) {
                for (size_t oc = 0; oc < out_c; oc++) {
                    if (out_decl == TYPE_INT) {
                        int64_t acc = 0;
                        for (size_t kx = 0; kx < kw; kx++) {
                            for (size_t ky = 0; ky < kh; ky++) {
                                for (size_t ic = 0; ic < in_c; ic++) {
                                    int64_t in_x = (int64_t)ow * stride_w + (int64_t)kx - pad_w;
                                    int64_t in_y = (int64_t)oh * stride_h + (int64_t)ky - pad_h;
                                    if (in_x < 0 || in_y < 0 || (size_t)in_x >= in_w || (size_t)in_y >= in_h) continue; // zero pad
                                    size_t in_off = (size_t)in_x * x->strides[0] + (size_t)in_y * x->strides[1] + ic * x->strides[2];
                                    size_t k_off = kx * k->strides[0] + ky * k->strides[1] + ic * k->strides[2] + oc * k->strides[3];
                                    Value vx = x->data[in_off];
                                    Value vk = k->data[k_off];
                                    if (vx.type != VAL_INT || vk.type != VAL_INT) { value_free(out); RUNTIME_ERROR(interp, "CONV integer-mode requires INT elements", line, col); }
                                    acc += vx.as.i * vk.as.i;
                                }
                            }
                        }
                        if (bias_present && bias_t->length > 0) {
                            Value bv = bias_t->data[oc];
                            if (bv.type == VAL_INT) acc += bv.as.i;
                            else if (bv.type == VAL_FLT) {
                                int64_t tmp;
                                if (!coerce_flt_to_int_checked(interp, bv.as.f, &tmp, "CONV", line, col)) { value_free(out); return value_null(); }
                                acc += tmp;
                            } else { value_free(out); RUNTIME_ERROR(interp, "CONV bias must be numeric", line, col); }
                        }
                        ot->data[ow * ot->strides[0] + oh * ot->strides[1] + oc * ot->strides[2]] = value_int(acc);
                    } else {
                        double acc = 0.0;
                        for (size_t kx = 0; kx < kw; kx++) {
                            for (size_t ky = 0; ky < kh; ky++) {
                                for (size_t ic = 0; ic < in_c; ic++) {
                                    int64_t in_x = (int64_t)ow * stride_w + (int64_t)kx - pad_w;
                                    int64_t in_y = (int64_t)oh * stride_h + (int64_t)ky - pad_h;
                                    if (in_x < 0 || in_y < 0 || (size_t)in_x >= in_w || (size_t)in_y >= in_h) continue;
                                    size_t in_off = (size_t)in_x * x->strides[0] + (size_t)in_y * x->strides[1] + ic * x->strides[2];
                                    size_t k_off = kx * k->strides[0] + ky * k->strides[1] + ic * k->strides[2] + oc * k->strides[3];
                                    Value vx = x->data[in_off];
                                    Value vk = k->data[k_off];
                                    double aval = (vx.type == VAL_FLT) ? vx.as.f : (double)vx.as.i;
                                    double kval = (vk.type == VAL_FLT) ? vk.as.f : (double)vk.as.i;
                                    acc += aval * kval;
                                }
                            }
                        }
                        if (bias_present && bias_t->length > 0) {
                            Value bv = bias_t->data[oc];
                            double bval = (bv.type == VAL_FLT) ? bv.as.f : (double)bv.as.i;
                            acc += bval;
                        }
                        ot->data[ow * ot->strides[0] + oh * ot->strides[1] + oc * ot->strides[2]] = value_flt(acc);
                    }
                }
            }
        }
        return out;
    }

    // Legacy two-argument N-D convolution (backward-compatible)
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

// APPEND: append a single element to a 1-D tensor and return a new tensor
static Value builtin_append(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[1].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "APPEND expects (ANY, TNS)", line, col);
    }
    Tensor* t = args[1].as.tns;
    if (!t) {
        RUNTIME_ERROR(interp, "Invalid target tensor", line, col);
    }
    if (t->ndim != 1) {
        RUNTIME_ERROR(interp, "APPEND target must be 1-D TNS", line, col);
    }

    size_t old_len = t->shape[0];
    size_t new_len = old_len + 1;
    size_t shape[1] = { new_len };
    /* Determine resulting element DeclType. If the target tensor's
       static element type differs from the appended element's DeclType,
       the result must be TYPE_UNKNOWN (heterogeneous). If the target
       is unknown but empty, adopt the appended element's DeclType. */
    DeclType appended_decl;
    switch (args[0].type) {
        case VAL_BOOL: appended_decl = TYPE_BOOL; break;
        case VAL_INT: appended_decl = TYPE_INT; break;
        case VAL_FLT: appended_decl = TYPE_FLT; break;
        case VAL_STR: appended_decl = TYPE_STR; break;
        case VAL_TNS: appended_decl = TYPE_TNS; break;
        case VAL_FUNC: appended_decl = TYPE_FUNC; break;
        default: appended_decl = TYPE_UNKNOWN; break;
    }

    DeclType out_elem_type = t->elem_type;
    if (out_elem_type == TYPE_UNKNOWN) {
        if (old_len == 0) out_elem_type = appended_decl;
    } else {
        if (appended_decl != out_elem_type) out_elem_type = TYPE_UNKNOWN;
    }

    Value out = value_tns_new(out_elem_type, 1, shape);
    Tensor* ot = out.as.tns;

    // copy existing elements
    for (size_t i = 0; i < old_len; i++) {
        ot->data[i] = value_copy(t->data[i]);
    }

    // append the provided element (deep-copy containers to avoid shared mutation)
    if (args[0].type == VAL_TNS || args[0].type == VAL_MAP) {
        ot->data[new_len - 1] = value_deep_copy(args[0]);
    } else {
        ot->data[new_len - 1] = value_copy(args[0]);
    }

    if (!writeback_ptr_node(interp, arg_nodes ? arg_nodes[1] : NULL, env, out, "APPEND", line, col)) {
        value_free(out);
        return value_null();
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

    int out_base = result_base_from_values(args[0], args[1]);
    
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
            return value_int_base(x, out_base);
        }
        if (n == 1) return value_int_base(x, out_base);
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
            return value_int_base(lo, out_base);
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
            return value_int_base(-lo, out_base);
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
        double res = -pow(-x, 1.0 / n);
        double rintv = round(res);
        double tol = 1e-12 * (fabs(rintv) + 1.0);
        if (fabs(res - rintv) <= tol) res = rintv;
        return value_flt_base(res, out_base);
    }

    double res = pow(x, 1.0 / n);
    double rintv = round(res);
    double tol = 1e-12 * (fabs(rintv) + 1.0);
    if (fabs(res - rintv) <= tol) res = rintv;
    return value_flt_base(res, out_base);
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
    
    int out_base = result_base_from_values(args[0], args[1]);

    if (args[0].type == VAL_INT) {
        return value_int_base(gcd_int(args[0].as.i, args[1].as.i), out_base);
    }

    double a = args[0].as.f;
    double b = args[1].as.f;
    if (floor(a) != a || floor(b) != b) {
        RUNTIME_ERROR(interp, "GCD expects integer-valued floats", line, col);
    }
    return value_flt_base((double)gcd_int((int64_t)a, (int64_t)b), out_base);
}

// LCM
static Value builtin_lcm(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LCM", interp, line, col);
    EXPECT_NUM(args[1], "LCM", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LCM cannot mix INT and FLT", line, col);
    }
    int out_base = result_base_from_values(args[0], args[1]);

    if (args[0].type == VAL_INT) {
        int64_t a = args[0].as.i;
        int64_t b = args[1].as.i;
        if (a == 0 || b == 0) return value_int_base(0, out_base);
        int64_t g = gcd_int(a, b);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        return value_int_base((a / g) * b, out_base);
    }

    double a = args[0].as.f;
    double b = args[1].as.f;
    if (floor(a) != a || floor(b) != b) {
        RUNTIME_ERROR(interp, "LCM expects integer-valued floats", line, col);
    }
    int64_t ai = (int64_t)a;
    int64_t bi = (int64_t)b;
    if (ai == 0 || bi == 0) return value_flt_base(0.0, out_base);
    int64_t g = gcd_int(ai, bi);
    if (ai < 0) ai = -ai;
    if (bi < 0) bi = -bi;
    return value_flt_base((double)((ai / g) * bi), out_base);
}

// ============ Comparison operators ============

typedef struct {
    const void* a;
    const void* b;
    ValueType type;
} EqSeenPair;

typedef struct {
    EqSeenPair* items;
    size_t count;
    size_t cap;
} EqSeenSet;

static int eq_seen_contains(EqSeenSet* seen, const void* a, const void* b, ValueType type) {
    if (!seen) return 0;
    for (size_t i = 0; i < seen->count; i++) {
        EqSeenPair* p = &seen->items[i];
        if (p->type != type) continue;
        if ((p->a == a && p->b == b) || (p->a == b && p->b == a)) return 1;
    }
    return 0;
}

static void eq_seen_add(EqSeenSet* seen, const void* a, const void* b, ValueType type) {
    if (!seen) return;
    if (seen->count + 1 > seen->cap) {
        size_t new_cap = seen->cap == 0 ? 16 : seen->cap * 2;
        EqSeenPair* grown = realloc(seen->items, sizeof(EqSeenPair) * new_cap);
        if (!grown) { fprintf(stderr, "Out of memory\n"); exit(1); }
        seen->items = grown;
        seen->cap = new_cap;
    }
    seen->items[seen->count].a = a;
    seen->items[seen->count].b = b;
    seen->items[seen->count].type = type;
    seen->count++;
}

static int value_deep_eq_impl(Value a, Value b, EqSeenSet* seen) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_BOOL:
            return a.as.boolean == b.as.boolean ? 1 : 0;
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
            if (ta == tb) return 1;
            if (eq_seen_contains(seen, ta, tb, VAL_TNS)) return 1;
            eq_seen_add(seen, ta, tb, VAL_TNS);
            if (ta->elem_type != tb->elem_type) return 0;
            if (ta->ndim != tb->ndim) return 0;
            for (size_t i = 0; i < ta->ndim; i++) {
                if (ta->shape[i] != tb->shape[i]) return 0;
            }
            if (ta->length != tb->length) return 0;
            for (size_t i = 0; i < ta->length; i++) {
                if (!value_deep_eq_impl(ta->data[i], tb->data[i], seen)) return 0;
            }
            return 1;
        }
        case VAL_MAP: {
            Map* ma = a.as.map;
            Map* mb = b.as.map;
            if (ma == NULL || mb == NULL) return (ma == mb) ? 1 : 0;
            if (ma == mb) return 1;
            if (eq_seen_contains(seen, ma, mb, VAL_MAP)) return 1;
            eq_seen_add(seen, ma, mb, VAL_MAP);
            if (ma->count != mb->count) return 0;
            for (size_t i = 0; i < ma->count; i++) {
                int found = 0;
                for (size_t j = 0; j < mb->count; j++) {
                    if (value_deep_eq_impl(ma->items[i].key, mb->items[j].key, seen)) {
                        if (!value_deep_eq_impl(ma->items[i].value, mb->items[j].value, seen)) return 0;
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

// Recursive deep equality helper for Values (returns 1 if equal, 0 otherwise)
static int value_deep_eq(Value a, Value b) {
    EqSeenSet seen = {0};
    int out = value_deep_eq_impl(a, b, &seen);
    free(seen.items);
    return out;
}

static Value builtin_eq(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp;

    // If types differ, not equal
    if (args[0].type != args[1].type) {
        return value_bool(false);
    }

    return value_bool(value_deep_eq(args[0], args[1]) != 0);
}

static Value builtin_neq(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp;

    /* If types differ, they are not equal -> NEQ should be true (1) */
    if (args[0].type != args[1].type) {
        return value_bool(true);
    }

    return value_bool(value_deep_eq(args[0], args[1]) == 0);
}

static Value builtin_gt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "GT", interp, line, col);
    EXPECT_NUM(args[1], "GT", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "GT cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_bool(args[0].as.i > args[1].as.i);
    }
    return value_bool(args[0].as.f > args[1].as.f);
}

static Value builtin_lt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LT", interp, line, col);
    EXPECT_NUM(args[1], "LT", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LT cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_bool(args[0].as.i < args[1].as.i);
    }
    return value_bool(args[0].as.f < args[1].as.f);
}

static Value builtin_gte(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "GTE", interp, line, col);
    EXPECT_NUM(args[1], "GTE", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "GTE cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_bool(args[0].as.i >= args[1].as.i);
    }
    return value_bool(args[0].as.f >= args[1].as.f);
}

static Value builtin_lte(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "LTE", interp, line, col);
    EXPECT_NUM(args[1], "LTE", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "LTE cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_bool(args[0].as.i <= args[1].as.i);
    }
    return value_bool(args[0].as.f <= args[1].as.f);
}

// ============ Logical operators ============

static Value builtin_and(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(value_truthiness(args[0]) && value_truthiness(args[1]));
}

static Value builtin_or(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(value_truthiness(args[0]) || value_truthiness(args[1]));
}

static Value builtin_xor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    int a = value_truthiness(args[0]) ? 1 : 0;
    int b = value_truthiness(args[1]) ? 1 : 0;
    return value_bool((a ^ b) != 0);
}

static Value builtin_not(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(!value_truthiness(args[0]));
}

static Value builtin_bool(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(value_truthiness(args[0]));
}

// ============ Bitwise operators ============

static Value builtin_band(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BAND", interp, line, col);
    EXPECT_INT(args[1], "BAND", interp, line, col);
    if (numeric_base_of(args[0]) != 2 || numeric_base_of(args[1]) != 2) {
        RUNTIME_ERROR(interp, "BAND requires binary INT operands", line, col);
    }
    return value_int_base(args[0].as.i & args[1].as.i, 2);
}

static Value builtin_bor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BOR", interp, line, col);
    EXPECT_INT(args[1], "BOR", interp, line, col);
    if (numeric_base_of(args[0]) != 2 || numeric_base_of(args[1]) != 2) {
        RUNTIME_ERROR(interp, "BOR requires binary INT operands", line, col);
    }
    return value_int_base(args[0].as.i | args[1].as.i, 2);
}

static Value builtin_bxor(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BXOR", interp, line, col);
    EXPECT_INT(args[1], "BXOR", interp, line, col);
    if (numeric_base_of(args[0]) != 2 || numeric_base_of(args[1]) != 2) {
        RUNTIME_ERROR(interp, "BXOR requires binary INT operands", line, col);
    }
    return value_int_base(args[0].as.i ^ args[1].as.i, 2);
}

static Value builtin_bnot(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "BNOT", interp, line, col);
    if (numeric_base_of(args[0]) != 2) {
        RUNTIME_ERROR(interp, "BNOT requires binary INT operands", line, col);
    }
    return value_int_base(~args[0].as.i, 2);
}

static Value builtin_shl(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "SHL", interp, line, col);
    EXPECT_INT(args[1], "SHL", interp, line, col);
    if (numeric_base_of(args[0]) != 2 || numeric_base_of(args[1]) != 2) {
        RUNTIME_ERROR(interp, "SHL requires binary INT operands", line, col);
    }
    if (args[1].as.i < 0) {
        RUNTIME_ERROR(interp, "SHL amount must be non-negative", line, col);
    }
    return value_int_base(args[0].as.i << args[1].as.i, 2);
}

static Value builtin_shr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_INT(args[0], "SHR", interp, line, col);
    EXPECT_INT(args[1], "SHR", interp, line, col);
    if (numeric_base_of(args[0]) != 2 || numeric_base_of(args[1]) != 2) {
        RUNTIME_ERROR(interp, "SHR requires binary INT operands", line, col);
    }
    if (args[1].as.i < 0) {
        RUNTIME_ERROR(interp, "SHR amount must be non-negative", line, col);
    }
    return value_int_base(args[0].as.i >> args[1].as.i, 2);
}

// ============ Type conversion ============

static Value builtin_int(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (args[0].type == VAL_BOOL) {
        return value_int_base(args[0].as.boolean ? 1 : 0, 2);
    }
    if (args[0].type == VAL_INT) {
        return value_int_base(args[0].as.i, numeric_base_of(args[0]));
    }
    if (args[0].type == VAL_FLT) {
        int b = numeric_base_of(args[0]);
        if (b <= 0) b = 2;
        int64_t tmp;
        if (!coerce_flt_to_int_checked(interp, args[0].as.f, &tmp, "INT", line, col)) return value_null();
        return value_int_base(tmp, b);
    }
    if (args[0].type == VAL_STR) {
        int64_t val = 0;
        int base = 2;
        if (!args[0].as.s || !*args[0].as.s) return value_int_base(0, 2);
        if (!parse_prefixed_int_string(args[0].as.s, &val, &base)) {
            return value_int_base(1, 2);
        }
        return value_int_base(val, base);
    }
    RUNTIME_ERROR(interp, "INT expects BOOL, INT, FLT, or STR argument", line, col);
}

static Value builtin_flt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (args[0].type == VAL_BOOL) {
        return value_flt_base(args[0].as.boolean ? 1.0 : 0.0, 2);
    }
    if (args[0].type == VAL_FLT) {
        if (args[0].num_base_nan) return value_flt_nan_base(args[0].as.f);
        return value_flt_base(args[0].as.f, numeric_base_of(args[0]));
    }
    if (args[0].type == VAL_INT) {
        return value_flt_base((double)args[0].as.i, numeric_base_of(args[0]));
    }
    if (args[0].type == VAL_STR) {
        double fv = 0.0;
        int base = 2;
        int base_is_nan = 0;
        if (!args[0].as.s || !*args[0].as.s) return value_flt_base(0.0, 2);
        if (!parse_prefixed_flt_string(args[0].as.s, &fv, &base, &base_is_nan)) {
            RUNTIME_ERROR(interp, "FLT string must be a base-prefixed number", line, col);
        }
        if (base_is_nan) return value_flt_nan_base(fv);
        return value_flt_base(fv, base);
    }
    RUNTIME_ERROR(interp, "FLT expects BOOL, INT, FLT, or STR argument", line, col);
}

// CONVERT(num, base): change numeric base of a value (INT or FLT)
static Value builtin_convert(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc; (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "CONVERT", interp, line, col);
    EXPECT_INT(args[1], "CONVERT", interp, line, col);
    int64_t base = args[1].as.i;
    if (base < 2 || base > 64) {
        RUNTIME_ERROR(interp, "CONVERT base must be between 2 and 64", line, col);
    }
    if (args[0].type == VAL_INT) return value_int_base(args[0].as.i, (int)base);
    if (args[0].num_base_nan) return value_flt_nan_base(args[0].as.f);
    return value_flt_base(args[0].as.f, (int)base);
}

// BASE(num): return the numeric base of a value (INT or FLT). Error on NaN-base FLT.
static Value builtin_base(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc; (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "BASE", interp, line, col);
    if (args[0].type == VAL_FLT && args[0].num_base_nan) {
        char* sval = NULL;
        if (isnan(args[0].as.f)) {
            sval = strdup("NaN");
        } else if (isinf(args[0].as.f)) {
            sval = strdup(signbit(args[0].as.f) ? "-INF" : "INF");
        } else {
            sval = flt_to_base_prefixed_str(args[0].as.f, args[0].num_base, 0);
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "BASE is undefined for %s", sval);
        free(sval);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int_base((int64_t)numeric_base_of(args[0]), 10);
}

static Value builtin_str(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;

    if (args[0].type == VAL_STR) {
        return value_str(args[0].as.s);
    }
    if (args[0].type == VAL_BOOL) {
        return value_str(args[0].as.boolean ? "TRUE" : "FALSE");
    }
    if (args[0].type == VAL_INT) {
        char* s = int_to_base_prefixed_str(args[0].as.i, numeric_base_of(args[0]));
        Value v = value_str(s);
        free(s);
        return v;
    }
    if (args[0].type == VAL_FLT) {
        char* s = flt_to_base_prefixed_str(args[0].as.f, numeric_base_of(args[0]), args[0].num_base_nan);
        Value v = value_str(s);
        free(s);
        return v;
    }

    /* Per SPEC section 9.1.2, STR conversion only accepts BOOL, STR, INT, FLT. */
    RUNTIME_ERROR(interp, "STR expects BOOL, STR, INT, or FLT", line, col);
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
    return value_int((int64_t)utf8_codepoint_count(args[0].as.s));
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
        bool separator_mode = (first_len == 1 && argc >= 3);
        if (separator_mode) {
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
            for (int i = 1; i < argc; ++i) {
                if (!writeback_ptr_node(interp, arg_nodes ? arg_nodes[i] : NULL, env, v, "JOIN", line, col)) {
                    value_free(v);
                    return value_null();
                }
            }
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
            for (int i = 0; i < argc; ++i) {
                if (!writeback_ptr_node(interp, arg_nodes ? arg_nodes[i] : NULL, env, v, "JOIN", line, col)) {
                    value_free(v);
                    return value_null();
                }
            }
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
    Value v = value_int(result);
    for (int i = 0; i < argc; ++i) {
        if (!writeback_ptr_node(interp, arg_nodes ? arg_nodes[i] : NULL, env, v, "JOIN", line, col)) {
            value_free(v);
            return value_null();
        }
    }
    return v;
}

static Value builtin_split(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // SPLIT(str, sep?) -> 1-D TNS of STR
    EXPECT_STR(args[0], "SPLIT", interp, line, col);
    const char* sep = NULL;
    if (argc >= 2) {
        EXPECT_STR(args[1], "SPLIT", interp, line, col);
        sep = args[1].as.s;
        if (sep[0] == '\0') {
            RUNTIME_ERROR(interp, "SPLIT expects a non-empty delimiter", line, col);
        }
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
        // last piece, including empty trailing fields
        size_t len = strlen(cur);
        char* piece = malloc(len + 1);
        memcpy(piece, cur, len);
        piece[len] = '\0';
        if (count + 1 > cap) { cap *= 2; items = realloc(items, sizeof(Value) * cap); }
        items[count++] = value_str(piece);
        free(piece);
        free(copy);
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

    // Container must be a tensor; otherwise this is a runtime error per spec
    if (args[1].type != VAL_TNS) {
        RUNTIME_ERROR(interp, "IN expects TNS as second argument", line, col);
    }

    Tensor* t = args[1].as.tns;
    if (!t || t->length == 0) return value_bool(false);

    for (size_t i = 0; i < t->length; i++) {
        if (value_deep_eq(args[0], t->data[i])) return value_bool(true);
    }
    return value_bool(false);
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

    EnvEntry* scope_entry = env_get_entry(mod_env, "__MODULE_SCOPE__");
    if (!scope_entry || !scope_entry->initialized || scope_entry->value.type != VAL_STR) {
        env_assign(mod_env, "__MODULE_SCOPE__", value_str(alias), TYPE_STR, true);
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

    if (module_export_bindings(interp, env, mod_env, alias, line, col, "IMPORT_PATH failed to assign qualified name") != 0) {
        if (alias_dup) free(alias_dup);
        return value_null();
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
    return value_bool(false);
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
        RUNTIME_ERROR(interp, "REPLACE expects non-empty old substring", line, col);
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
    
    const char* haystack = args[0].as.s;
    const char* needle = args[1].as.s;

    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);

    if (needle_len == 0) {
        RUNTIME_ERROR(interp, "STRIP expects non-empty remove substring", line, col);
    }

    // Count non-overlapping occurrences of needle
    size_t count = 0;
    const char* p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }

    if (count == 0) {
        return value_str(haystack);
    }

    // New length after removing all occurrences (replacement = "")
    size_t result_len = haystack_len - count * needle_len;
    char* result = malloc(result_len + 1);
    char* dst = result;
    p = haystack;
    const char* prev = haystack;

    while ((p = strstr(prev, needle)) != NULL) {
        size_t before = (size_t)(p - prev);
        memcpy(dst, prev, before);
        dst += before;
        prev = p + needle_len;
    }
    strcpy(dst, prev);

    Value v = value_str(result);
    free(result);
    return v;
}

// ============ I/O operations ============

static Value builtin_print(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)line; (void)col;

    if (argc == 0) {
        RUNTIME_ERROR(interp, "PRINT expects at least one argument", line, col);
    }

    int forward = !(interp && interp->shushed);

    for (int i = 0; i < argc; i++) {
        switch (args[i].type) {
            case VAL_BOOL: {
                if (forward) printf("%s", args[i].as.boolean ? "TRUE" : "FALSE");
                break;
            }
            case VAL_INT: {
                char* s = int_to_base_prefixed_str(args[i].as.i, numeric_base_of(args[i]));
                if (forward) printf("%s", s);
                free(s);
                break;
            }
            case VAL_FLT: {
                char* s = flt_to_base_prefixed_str(args[i].as.f, numeric_base_of(args[i]), args[i].num_base_nan);
                if (forward) printf("%s", s);
                free(s);
                break;
            }
            case VAL_STR:
                if (forward) printf("%s", args[i].as.s);
                break;
            default:
                RUNTIME_ERROR(interp, "PRINT expects BOOL, INT, FLT, or STR argument", line, col);
                break;
        }
    }
    if (forward) printf("\n");
    return value_bool(false);
}

static Value builtin_warn(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)line; (void)col;
    if (!interp) return value_bool(false);
    if (argc < 1) {
        RUNTIME_ERROR(interp, "WARN expects at least one argument", line, col);
    }

    int forward = (interp->verbose && !interp->shushed);

    /* Build concatenated warning message for validation even when not forwarded. */
    size_t cap = 256;
    char* out = malloc(cap);
    if (!out) RUNTIME_ERROR(interp, "Out of memory", line, col);
    size_t out_len = 0;
    out[0] = '\0';

    for (int i = 0; i < argc; i++) {
        char* tmp = NULL;
        const char* piece = NULL;

        switch (args[i].type) {
            case VAL_BOOL:
                piece = args[i].as.boolean ? "TRUE" : "FALSE";
                break;
            case VAL_INT:
                tmp = int_to_base_prefixed_str(args[i].as.i, numeric_base_of(args[i]));
                piece = tmp;
                break;
            case VAL_FLT:
                tmp = flt_to_base_prefixed_str(args[i].as.f, numeric_base_of(args[i]), args[i].num_base_nan);
                piece = tmp;
                break;
            case VAL_STR:
                piece = args[i].as.s ? args[i].as.s : "";
                break;
            default: {
                char buf[128];
                snprintf(buf, sizeof(buf), "WARN expects BOOL, INT, FLT, or STR argument, got %s", value_type_name(args[i]));
                free(out);
                RUNTIME_ERROR(interp, buf, line, col);
                break;
            }
        }

        size_t piece_len = strlen(piece);
        if (out_len + piece_len + 1 > cap) {
            size_t newcap = cap;
            while (out_len + piece_len + 1 > newcap) newcap *= 2;
            char* nout = realloc(out, newcap);
            if (!nout) {
                if (tmp) free(tmp);
                free(out);
                RUNTIME_ERROR(interp, "Out of memory", line, col);
            }
            out = nout;
            cap = newcap;
        }
        memcpy(out + out_len, piece, piece_len);
        out_len += piece_len;
        out[out_len] = '\0';

        if (tmp) { free(tmp); tmp = NULL; }
    }

    if (forward) printf("WARNING: %s\n", out);
    free(out);
    return value_bool(forward != 0);
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

// SHUSH():BOOL - suppress forwarding of console output
static Value builtin_shush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    if (!interp) return value_bool(false);
    interp->shushed = 1;
    return value_bool(false);
}

// UNSHUSH():BOOL - re-enable forwarding of console output
static Value builtin_unshush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)line; (void)col;
    if (!interp) return value_bool(false);
    interp->shushed = 0;
    return value_bool(false);
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

    // Text modes: handle UTF-8 and other supported encodings
    // UTF-8 and UTF-8 BOM: return UTF-8 string (strip BOM if present)
    if (strcmp(codelb, "utf-8") == 0 || strcmp(codelb, "utf8") == 0 || strcmp(codelb, "utf-8-bom") == 0 || strcmp(codelb, "utf-8 bom") == 0) {
        size_t start = 0;
        if (sz >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) start = 3;
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

    // UTF-16 (LE/BE)
    if (strstr(codelb, "utf-16") != NULL) {
        int little = 1;
        size_t start = 0;
        if (sz >= 2) {
            if (buf[0] == 0xFF && buf[1] == 0xFE) { little = 1; start = 2; }
            else if (buf[0] == 0xFE && buf[1] == 0xFF) { little = 0; start = 2; }
        }
        if (strstr(codelb, "be") != NULL) little = 0;
        if (strstr(codelb, "le") != NULL) little = 1;
        char* out = dec_utf16_to_utf8(buf + start, (size_t)sz - start, little);
        free(buf);
        if (!out) RUNTIME_ERROR(interp, "Out of memory", line, col);
        Value v = value_str(out);
        free(out);
        return v;
    }

    // ANSI -> cp1252 on Windows, Latin-1 elsewhere
    if (strcmp(codelb, "ansi") == 0) {
#ifdef _WIN32
        char* out = dec_cp1252_to_utf8(buf, (size_t)sz);
#else
        char* out = dec_latin1_to_utf8(buf, (size_t)sz);
#endif
        free(buf);
        if (!out) RUNTIME_ERROR(interp, "Out of memory", line, col);
        Value v = value_str(out);
        free(out);
        return v;
    }

    // Unknown/unsupported coding: error
    free(buf);
    RUNTIME_ERROR(interp, "READFILE: unsupported coding", line, col);
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
            return value_bool(false);
        }
        for (size_t i = 0; i < blen; i += 8) {
            unsigned char byte = 0;
            for (int b = 0; b < 8; b++) {
                char c = blob[i+b];
                if (c != '0' && c != '1') { fclose(f); RUNTIME_ERROR(interp, "WRITEFILE(binary) expects only 0/1 characters", line, col); }
                byte = (byte << 1) | (unsigned char)(c - '0');
            }
            if (fwrite(&byte, 1, 1, f) != 1) { fclose(f); return value_bool(false); }
        }
        fclose(f);
        return value_bool(true);
    }

    // hex
    if (strcmp(codelb, "hex") == 0 || strcmp(codelb, "hexadecimal") == 0) {
        size_t blen = strlen(blob);
        if (blen % 2 != 0) RUNTIME_ERROR(interp, "WRITEFILE(hex) expects even-length hex string", line, col);
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_bool(false);
        }
        for (size_t i = 0; i < blen; i += 2) {
            char a = blob[i]; char b = blob[i+1];
            int start = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
            int lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
            if (start < 0 || lo < 0) { fclose(f); RUNTIME_ERROR(interp, "WRITEFILE(hex) expects valid hex digits", line, col); }
            unsigned char byte = (unsigned char)((start << 4) | lo);
            if (fwrite(&byte, 1, 1, f) != 1) { fclose(f); return value_bool(false); }
        }
        fclose(f);
        return value_bool(true);
    }

    // Text encodings: handle UTF-8, UTF-16, ANSI/Latin-1
    if (strstr(codelb, "utf-16") != NULL) {
        int little = 1;
        if (strstr(codelb, "be") != NULL) little = 0;
        if (strstr(codelb, "le") != NULL) little = 1;
        size_t outlen = 0;
        unsigned char* outbuf = enc_utf8_to_utf16(blob, &outlen, little);
        if (!outbuf) RUNTIME_ERROR(interp, "WRITEFILE: encoding failed", line, col);
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            free(outbuf);
            return value_bool(false);
        }
        if (outlen > 0) {
            if (fwrite(outbuf, 1, outlen, f) != outlen) { fclose(f); free(outbuf); return value_bool(false); }
        }
        fclose(f);
        free(outbuf);
        return value_bool(true);
    }

    if (strcmp(codelb, "ansi") == 0) {
        size_t outlen = 0;
        unsigned char* outbuf = enc_utf8_to_cp1252(blob, &outlen,
#ifdef _WIN32
            1
#else
            0
#endif
        );
        if (!outbuf) RUNTIME_ERROR(interp, "WRITEFILE: data contains characters not representable in ANSI", line, col);
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            free(outbuf);
            return value_bool(false);
        }
        if (outlen > 0) {
            if (fwrite(outbuf, 1, outlen, f) != outlen) { fclose(f); free(outbuf); return value_bool(false); }
        }
        fclose(f);
        free(outbuf);
        return value_bool(true);
    }

    // UTF-8 (with optional BOM)
    if (strcmp(codelb, "utf-8-bom") == 0 || strcmp(codelb, "utf-8 bom") == 0) {
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_bool(false);
        }
        unsigned char bom[3] = {0xEF,0xBB,0xBF};
        if (fwrite(bom, 1, 3, f) != 3) { fclose(f); return value_bool(false); }
        size_t towrite = strlen(blob);
        if (towrite > 0) {
            if (fwrite(blob, 1, towrite, f) != towrite) { fclose(f); return value_bool(false); }
        }
        fclose(f);
        return value_bool(true);
    }

    if (strcmp(codelb, "utf-8") == 0 || strcmp(codelb, "utf8") == 0) {
        FILE* f = fopen(args[1].as.s, "wb");
        if (!f) {
            fprintf(stderr, "WRITEFILE: cannot open '%s' for writing: %s\n", args[1].as.s, strerror(errno));
            return value_bool(false);
        }
        size_t towrite = strlen(blob);
        if (towrite > 0) {
            if (fwrite(blob, 1, towrite, f) != towrite) { fclose(f); return value_bool(false); }
        }
        fclose(f);
        return value_bool(true);
    }

    RUNTIME_ERROR(interp, "WRITEFILE: unsupported coding", line, col);
}

// EXISTFILE(STR: path):INT
static Value builtin_existfile(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    if (argc < 1) {
        RUNTIME_ERROR(interp, "EXISTFILE expects 1 argument", line, col);
    }
    EXPECT_STR(args[0], "EXISTFILE", interp, line, col);
    FILE* f = fopen(args[0].as.s, "rb");
    if (f) { fclose(f); return value_bool(true); }
    return value_bool(false);
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
    return value_bool(true);
}

// ============ Control flow helpers ============

static Value builtin_assert(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (!value_truthiness(args[0])) {
        RUNTIME_ERROR(interp, "Assertion failed", line, col);
    }
    return value_bool(true);
}

// REFUTE(~BOOL: cond):BOOL
static Value builtin_refute(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;

    if (value_truthiness(args[0])) {
        RUNTIME_ERROR(interp, "Refutation failed", line, col);
    }
    return value_bool(true);
}

static Value builtin_throw(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "Exception thrown", line, col);
    }

    /* Build error message by concatenating rendered args (same rules as PRINT),
     * but do not append a trailing newline. Ownership of the resulting buffer
     * is transferred to interp->error (it will be freed by interpreter cleanup).
     */
    JsonBuf jb;
    jb_init(&jb);

    for (int i = 0; i < argc; i++) {
        switch (args[i].type) {
            case VAL_BOOL:
                jb_append_str(&jb, args[i].as.boolean ? "TRUE" : "FALSE");
                break;
            case VAL_INT: {
                char* s = int_to_base_prefixed_str(args[i].as.i, numeric_base_of(args[i]));
                jb_append_str(&jb, s);
                free(s);
                break;
            }
            case VAL_FLT: {
                char* s = flt_to_base_prefixed_str(args[i].as.f, numeric_base_of(args[i]), args[i].num_base_nan);
                jb_append_str(&jb, s);
                free(s);
                break;
            }
            case VAL_STR:
                jb_append_str(&jb, args[i].as.s ? args[i].as.s : "");
                break;
            case VAL_FUNC:
                jb_append_fmt(&jb, "<func %p>", (void*)args[i].as.func);
                break;
            default:
                jb_append_str(&jb, "<null>");
                break;
        }
    }

    if (jb.data && jb.len > 0) {
        /* Transfer ownership of jb.data to interp->error */
        interp->error = jb.data;
    } else {
        if (jb.data) free(jb.data);
        interp->error = strdup("Exception thrown");
    }
    interp->error_line = line;
    interp->error_col = col;
    return value_null();
}

// ============ Type checking ============

static Value builtin_isint(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_INT);
}

static Value builtin_isbool(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_BOOL);
}

static Value builtin_isflt(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_FLT);
}

static Value builtin_isstr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_STR);
}

static Value builtin_istns(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_TNS);
}

static Value builtin_ismap(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_MAP);
}

static Value builtin_isfunc(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_FUNC);
}

static Value builtin_isthr(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_bool(args[0].type == VAL_THR);
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
            // Build signature in the canonical form: "R name(T1 arg1, ...)"
            size_t cap = 512;
            char* buf = malloc(cap);
            if (!buf) RUNTIME_ERROR(interp, "Out of memory", line, col);
            buf[0] = '\0';
            const char* rname = decl_type_name(f->return_type);
            if (strcmp(rname, "UNKNOWN") == 0) rname = "ANY";
            strcat(buf, rname);
            strcat(buf, " ");
            strcat(buf, f->name ? f->name : name);
            strcat(buf, "(");
            for (size_t i = 0; i < f->params.count; i++) {
                Param p = f->params.items[i];
                const char* tname = decl_type_name(p.type);
                if (strcmp(tname, "UNKNOWN") == 0) tname = "ANY";
                if (i > 0) strcat(buf, ", ");
                if (p.coerced) strcat(buf, "~");
                strcat(buf, tname);
                strcat(buf, " ");
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
                        char* s = int_to_base_prefixed_str(dv.as.i, numeric_base_of(dv));
                        size_t need = strlen(buf) + strlen(s) + 2;
                        if (need > cap) { cap = need * 2; buf = realloc(buf, cap); }
                        strcat(buf, s);
                        free(s);
                    } else if (dv.type == VAL_FLT) {
                        char* s = flt_to_base_prefixed_str(dv.as.f, numeric_base_of(dv), dv.num_base_nan);
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
            strcat(buf, ")");
            Value out = value_str(buf);
            free(buf);
            return out;
        }
    }

    // Non-function: return "TYPE name" using declared type if available
    if (!entry) {
        RUNTIME_ERROR(interp, "SIGNATURE: identifier not found or uninitialized", line, col);
    }
    const char* tname = "UNKNOWN";
    switch (entry->decl_type) {
        case TYPE_BOOL: tname = "BOOL"; break;
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
    snprintf(res, len + 1, "%s %s", tname, name);
    Value out = value_str(res);
    free(res);
    return out;
}

// ============ Variable management ============

static Value builtin_del(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1) {
        RUNTIME_ERROR(interp, "DEL expects an identifier", line, col);
    }

    Expr* target = arg_nodes[0];

    /* Case 1: plain identifier – delete the binding */
    if (target->type == EXPR_IDENT) {
        const char* name = target->as.ident;
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
        return value_bool(false);
    }

    /* Case 2: indexed expression – support deleting map entries like DEL(m<k>) or DEL(m<k1,k2>) */
    if (target->type == EXPR_INDEX) {
        /* collect chain of index nodes (possibly nested) */
        size_t chain_len = 0;
        Expr* walker = target;
        while (walker && walker->type == EXPR_INDEX) {
            chain_len++;
            walker = walker->as.index.target;
        }
        if (!walker || walker->type != EXPR_IDENT) {
            RUNTIME_ERROR(interp, "DEL expects an identifier or indexed identifier", line, col);
        }

        const char* base_name = walker->as.ident;
        Value base_val = value_null();
        DeclType base_type = TYPE_UNKNOWN;
        bool base_initialized = false;
        if (!env_get(env, base_name, &base_val, &base_type, &base_initialized) || !base_initialized) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Cannot delete mapping from undefined identifier '%s'", base_name);
            RUNTIME_ERROR(interp, buf, line, col);
        }

        Expr** nodes = malloc(sizeof(Expr*) * (chain_len ? chain_len : 1));
        if (!nodes) {
            value_free(base_val);
            RUNTIME_ERROR(interp, "Out of memory", line, col);
        }
        walker = target;
        for (size_t i = 0; i < chain_len; i++) {
            nodes[i] = walker;
            walker = walker->as.index.target;
        }

        /* operate on the in-memory copy and write back at end */
        Value* cur = &base_val;

        for (int ni = (int)chain_len - 1; ni >= 0; ni--) {
            Expr* node = nodes[ni];
            ExprList* indices = &node->as.index.indices;
            if (indices->count == 0) {
                free(nodes);
                value_free(base_val);
                RUNTIME_ERROR(interp, "Empty index list", line, col);
            }

            if (cur->type != VAL_MAP) {
                free(nodes);
                value_free(base_val);
                RUNTIME_ERROR(interp, "Attempted map deletion on non-map value", node->line, node->column);
            }

            for (size_t i = 0; i < indices->count; i++) {
                Expr* it = indices->items[i];
                Value key = eval_expr(interp, it, env);
                if (interp->error) {
                    /* propagate evaluation error */
                    char* em = interp->error;
                    int el = interp->error_line;
                    int ec = interp->error_col;
                    clear_error(interp);
                    free(nodes);
                    value_free(base_val);
                    RUNTIME_ERROR(interp, em, el, ec);
                }
                if (!(key.type == VAL_INT || key.type == VAL_STR || key.type == VAL_FLT)) {
                    value_free(key);
                    free(nodes);
                    value_free(base_val);
                    RUNTIME_ERROR(interp, "Map index must be INT, FLT or STR", it->line, it->column);
                }

                bool last_key_in_node = (i + 1 == indices->count);
                bool last_node_in_chain = (ni == 0);

                if (last_node_in_chain && last_key_in_node) {
                    /* final key: perform deletion on current map */
                    value_map_delete(cur, key);
                    value_free(key);
                    /* write back modified base into environment */
                    if (!env_assign(env, base_name, base_val, TYPE_UNKNOWN, false)) {
                        free(nodes);
                        value_free(base_val);
                        RUNTIME_ERROR(interp, "Cannot write back to identifier (frozen?)", line, col);
                    }
                    free(nodes);
                    value_free(base_val);
                    return value_bool(false);
                }

                /* descend into child slot without creating missing entries */
                Value* slot = value_map_get_ptr(cur, key, false);
                value_free(key);
                if (!slot) {
                    /* intermediate missing -> nothing to delete (no-op) */
                    free(nodes);
                    value_free(base_val);
                    return value_bool(false);
                }
                if (slot->type != VAL_MAP) {
                    free(nodes);
                    value_free(base_val);
                    RUNTIME_ERROR(interp, "Attempted nested map indexing on non-map value", it->line, it->column);
                }
                /* descend */
                cur = slot;
            }
        }

        /* unreachable, but keep cleanup */
        free(nodes);
        value_free(base_val);
        return value_bool(false);
    }

    RUNTIME_ERROR(interp, "DEL expects an identifier", line, col);
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
    return value_bool(false);
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
    return value_bool(false);
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
    return value_bool(false);
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

    /* Materialize qualified bindings for every registered alias that
       references the same module environment. This ensures EXPORT updates
       sibling aliases that point at the same module (per spec). */
    size_t alias_count = 0;
    char** aliases = module_list_aliases(interp, mod_env, &alias_count);
    if (aliases) {
        for (size_t ai = 0; ai < alias_count; ai++) {
            if (module_export_bindings(interp, env, mod_env, aliases[ai], line, col, "EXPORT failed to assign qualified name") != 0) {
                for (size_t j = 0; j < alias_count; j++) free(aliases[j]);
                free(aliases);
                RUNTIME_ERROR(interp, "EXPORT failed to assign qualified name", line, col);
            }
            free(aliases[ai]);
        }
        free(aliases);
    } else {
        /* Fallback to previous behavior if alias enumeration fails */
        size_t len = strlen(module) + 1 + strlen(sym) + 1;
        char* qualified = malloc(len);
        if (!qualified) RUNTIME_ERROR(interp, "Out of memory", line, col);
        snprintf(qualified, len, "%s.%s", module, sym);
        if (!env_assign(env, qualified, entry->value, entry->decl_type, true)) {
            free(qualified);
            RUNTIME_ERROR(interp, "EXPORT failed to assign qualified name", line, col);
        }
        free(qualified);
    }

    return value_bool(false);
}

static Value builtin_frozen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "FROZEN expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int st = env_frozen_state(env, name);
    return value_bool(st != 0);
}

static Value builtin_permafrozen(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "PERMAFROZEN expects an identifier", line, col);
    }
    const char* name = arg_nodes[0]->as.ident;
    int p = env_permafrozen(env, name);
    return value_bool(p != 0);
}

static Value builtin_exist(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)interp; (void)line; (void)col;
    
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        return value_bool(false);
    }
    
    const char* name = arg_nodes[0]->as.ident;
    return value_bool(env_exists(env, name));
}

// ============ Variadic math ============

static Value builtin_sum(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "SUM requires at least one argument", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        int64_t sum = 0;
        int out_base = numeric_base_of(args[0]);
        for (int i = 0; i < argc; i++) {
            EXPECT_INT(args[i], "SUM", interp, line, col);
            sum += args[i].as.i;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_int_base(sum, out_base);
    }
    if (args[0].type == VAL_FLT) {
        double sum = 0.0;
        int out_base = numeric_base_of(args[0]);
        for (int i = 0; i < argc; i++) {
            EXPECT_FLT(args[i], "SUM", interp, line, col);
            sum += args[i].as.f;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_flt_base(sum, out_base);
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
        int out_base = numeric_base_of(args[0]);
        for (int i = 0; i < argc; i++) {
            EXPECT_INT(args[i], "PROD", interp, line, col);
            prod *= args[i].as.i;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_int_base(prod, out_base);
    }
    if (args[0].type == VAL_FLT) {
        double prod = 1.0;
        int out_base = numeric_base_of(args[0]);
        for (int i = 0; i < argc; i++) {
            EXPECT_FLT(args[i], "PROD", interp, line, col);
            prod *= args[i].as.f;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_flt_base(prod, out_base);
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
        int out_base = numeric_base_of(args[0]);
        for (int i = 1; i < argc; i++) {
            EXPECT_INT(args[i], "MAX", interp, line, col);
            if (args[i].as.i > max) max = args[i].as.i;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_int_base(max, out_base);
    }
    if (args[0].type == VAL_FLT) {
        double max = args[0].as.f;
        int out_base = numeric_base_of(args[0]);
        for (int i = 1; i < argc; i++) {
            EXPECT_FLT(args[i], "MAX", interp, line, col);
            if (args[i].as.f > max) max = args[i].as.f;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_flt_base(max, out_base);
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
                if (etype == TYPE_INT && v.type == VAL_INT) { best = value_int_base(v.as.i, numeric_base_of(v)); seeded = true; break; }
                if (etype == TYPE_FLT && v.type == VAL_FLT) { best = value_flt_base(v.as.f, numeric_base_of(v)); seeded = true; break; }
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
                    if (v.as.i > best.as.i) { value_free(best); best = value_int_base(v.as.i, numeric_base_of(v)); }
                } else if (etype == TYPE_FLT) {
                    EXPECT_FLT(v, "MAX", interp, line, col);
                    if (v.as.f > best.as.f) { value_free(best); best = value_flt_base(v.as.f, numeric_base_of(v)); }
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
        int out_base = numeric_base_of(args[0]);
        for (int i = 1; i < argc; i++) {
            EXPECT_INT(args[i], "MIN", interp, line, col);
            if (args[i].as.i < min) min = args[i].as.i;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_int_base(min, out_base);
    }
    if (args[0].type == VAL_FLT) {
        double min = args[0].as.f;
        int out_base = numeric_base_of(args[0]);
        for (int i = 1; i < argc; i++) {
            EXPECT_FLT(args[i], "MIN", interp, line, col);
            if (args[i].as.f < min) min = args[i].as.f;
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        }
        return value_flt_base(min, out_base);
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
                if (etype == TYPE_INT && v.type == VAL_INT) { best = value_int_base(v.as.i, numeric_base_of(v)); seeded = true; break; }
                if (etype == TYPE_FLT && v.type == VAL_FLT) { best = value_flt_base(v.as.f, numeric_base_of(v)); seeded = true; break; }
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
                    if (v.as.i < best.as.i) { value_free(best); best = value_int_base(v.as.i, numeric_base_of(v)); }
                } else if (etype == TYPE_FLT) {
                    EXPECT_FLT(v, "MIN", interp, line, col);
                    if (v.as.f < best.as.f) { value_free(best); best = value_flt_base(v.as.f, numeric_base_of(v)); }
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
        if (value_truthiness(args[i])) return value_bool(true);
    }
    return value_bool(false);
}

static Value builtin_all(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    for (int i = 0; i < argc; i++) {
        if (!value_truthiness(args[i])) return value_bool(false);
    }
    return value_bool(true);
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
        if (args[i].type == VAL_INT) {
            sum += args[i].as.i;
        } else {
            int64_t tmp;
            if (!coerce_flt_to_int_checked(interp, args[i].as.f, &tmp, "ISUM", line, col)) return value_null();
            sum += tmp;
        }
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
        if (args[i].type == VAL_INT) {
            prod *= args[i].as.i;
        } else {
            int64_t tmp;
            if (!coerce_flt_to_int_checked(interp, args[i].as.f, &tmp, "IPROD", line, col)) return value_null();
            prod *= tmp;
        }
    }
    return value_int(prod);
}

static Value builtin_fprod(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    
    if (argc == 0) {
        RUNTIME_ERROR(interp, "FPROD requires at least one argument", line, col);
    }
    
    double prod = 1.0;
    int out_base = numeric_base_of(args[0]);
    for (int i = 0; i < argc; i++) {
        EXPECT_NUM(args[i], "FPROD", interp, line, col);
        prod *= args[i].type == VAL_FLT ? args[i].as.f : (double)args[i].as.i;
        int bi = numeric_base_of(args[i]);
        if (bi > out_base) out_base = bi;
    }
    return value_flt_base(prod, out_base);
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

// INV (map inversion)
static Value builtin_inv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)argc;
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
        if (!writeback_ptr_range(interp, arg_nodes, env, 0, 1, out, "INV", line, col)) {
            value_free(out);
            return value_null();
        }
        return out;
    }

    RUNTIME_ERROR(interp, "INV expects MAP argument", line, col);
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
    // determine element DeclType for keys; allow mixed types (use TYPE_UNKNOWN)
    Value* items = malloc(sizeof(Value) * count);
    if (!items) RUNTIME_ERROR(interp, "Out of memory", line, col);

    DeclType elem_type = TYPE_UNKNOWN;
    for (size_t i = 0; i < count; i++) {
        ValueType kt = m->items[i].key.type;
        DeclType cur_dt = TYPE_UNKNOWN;
        if (kt == VAL_INT) cur_dt = TYPE_INT;
        else if (kt == VAL_FLT) cur_dt = TYPE_FLT;
        else if (kt == VAL_STR) cur_dt = TYPE_STR;
        else {
            for (size_t j = 0; j < i; j++) value_free(items[j]);
            free(items);
            RUNTIME_ERROR(interp, "KEYS: unsupported key type", line, col);
        }
        items[i] = value_copy(m->items[i].key);
        if (i == 0) elem_type = cur_dt;
        else if (elem_type != cur_dt) elem_type = TYPE_UNKNOWN;
    }

    size_t shape[1] = { count };
    Value out = value_tns_from_values(elem_type, 1, shape, items, count);
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
    if (vt == VAL_BOOL) dt = TYPE_BOOL;
    else if (vt == VAL_INT) dt = TYPE_INT;
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
        if (cur == VAL_BOOL) cur_dt = TYPE_BOOL;
        else if (cur == VAL_INT) cur_dt = TYPE_INT;
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
    if (args[0].type != VAL_INT && args[0].type != VAL_FLT && args[0].type != VAL_STR) {
        RUNTIME_ERROR(interp, "KEYIN expects INT, FLT or STR as first argument", line, col);
    }
    if (args[1].type != VAL_MAP) RUNTIME_ERROR(interp, "KEYIN expects MAP as second argument", line, col);
    int found = 0;
    Value res = value_map_get(args[1], args[0], &found);
    if (found) value_free(res);
    return value_bool(found != 0);
}

// VALUEIN(value, map):INT - returns 1 if any stored value equals the provided value
static Value builtin_valuein(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)argc;
    if (args[1].type != VAL_MAP) RUNTIME_ERROR(interp, "VALUEIN expects MAP as second argument", line, col);
    Map* m = args[1].as.map;
    if (!m) return value_bool(false);
    for (size_t i = 0; i < m->count; i++) {
        if (value_deep_eq(args[0], m->items[i].value)) return value_bool(true);
    }
    return value_bool(false);
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
    if (argc >= 3 && args[2].type != VAL_NULL) { EXPECT_INT(args[2], "MATCH", interp, line, col); typing = args[2].as.i ? 1 : 0; }
    if (argc >= 4 && args[3].type != VAL_NULL) { EXPECT_INT(args[3], "MATCH", interp, line, col); recurse = args[3].as.i ? 1 : 0; }
    if (argc >= 5 && args[4].type != VAL_NULL) { EXPECT_INT(args[4], "MATCH", interp, line, col); shape = args[4].as.i ? 1 : 0; }
    Map* m = args[0].as.map;
    Map* tpl = args[1].as.map;
    int ok = match_map_internal(m, tpl, typing, recurse, shape);
    return value_bool(ok != 0);
}

// COPY (shallow copy for scalars)
static Value builtin_copy(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    /* Preserve existing COPY operator behavior (shallow/aliasing). */
    return value_alias(args[0]);
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

    if (target->type == EXPR_TYPED_IDENT) {
        const char* name = target->as.typed_ident.name;
        DeclType expected = target->as.typed_ident.decl_type;
        DeclType actual;

        switch (rhs.type) {
            case VAL_BOOL: actual = TYPE_BOOL; break;
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
            char buf[128];
            snprintf(buf, sizeof(buf), "Type mismatch: expected %s but got %s",
                     decl_type_name(expected), value_type_name(rhs));
            RUNTIME_ERROR(interp, buf, line, col);
        }

        EnvEntry* existing = env_get_entry(env, name);
        if (existing && existing->decl_type != expected) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Type mismatch: expected %s but got %s",
                     decl_type_name(existing->decl_type), decl_type_name(expected));
            RUNTIME_ERROR(interp, buf, line, col);
        }

        Env* assign_env = env;
        if (!interp->isolate_env_writes && !existing && env->parent) {
            assign_env = env->parent;
        }
        if (!existing) {
            env_define(assign_env, name, expected);
        }
        if (!env_assign(assign_env, name, rhs, expected, true)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ASSIGN: cannot assign to target '%s'", name);
            RUNTIME_ERROR(interp, buf, line, col);
        }
        return value_copy(rhs);
    }

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
                case VAL_BOOL: actual = TYPE_BOOL; break;
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
        ExecResult res;
        if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
            char* buffered_error = NULL;
            int buffered_line = 0;
            int buffered_col = 0;
            if (!ns_buffer_assign_index(interp, env, target, rhs, line, col,
                                        &buffered_error, &buffered_line, &buffered_col)) {
                res.status = EXEC_ERROR;
                res.value = value_null();
                res.break_count = 0;
                res.jump_index = -1;
                res.error = buffered_error ? strdup(buffered_error) : strdup("Indexed assignment failed");
                res.error_line = buffered_line ? buffered_line : line;
                res.error_column = buffered_col ? buffered_col : col;
            } else {
                res.status = EXEC_OK;
                res.value = value_null();
                res.break_count = 0;
                res.jump_index = -1;
                res.error = NULL;
                res.error_line = 0;
                res.error_column = 0;
            }
            free(buffered_error);
        } else {
            res = assign_index_chain(interp, env, target, rhs, line, col);
        }
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
    int out_base = numeric_base_of(args[0]);

    int64_t v = args[0].as.i;
    if (v < 0) v = -v;
    if (v == 0) return value_int_base(1, out_base);

    int64_t len = 0;
    while (v > 0) {
        len++;
        v >>= 1;
    }
    return value_int_base(len, out_base);
}

// LEN: per specification, returns the number of supplied INT or STR arguments.
// Passing TNS or any other unsupported type is a runtime error.
static Value builtin_len(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;

    int out_base = 10; // default to decimal when no INT args present
    for (int i = 0; i < argc; ++i) {
        if (args[i].type == VAL_INT) {
            int bi = numeric_base_of(args[i]);
            if (bi > out_base) out_base = bi;
        } else if (args[i].type == VAL_STR) {
            /* allowed */
        } else {
            RUNTIME_ERROR(interp, "LEN expects INT or STR arguments", line, col);
        }
    }

    return value_int_base((int64_t)argc, out_base);
}

// Main, OS
static Value builtin_main(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)line; (void)col;
    // Determine module source for this call site (from env) and compare to interpreter primary source
    EnvEntry* call_src = env_get_entry(env, "__MODULE_SOURCE__");
    EnvEntry* primary_src = interp && interp->global_env ? env_get_entry(interp->global_env, "__MODULE_SOURCE__") : NULL;
    if (!primary_src || !primary_src->initialized) {
        // No recorded primary source -> treat as main
        return value_bool(true);
    }
    if (!call_src || !call_src->initialized) {
        // Call site has no source recorded; treat as main if equal to primary (unlikely) else main
        return value_bool(true);
    }
    if (call_src->value.type == VAL_STR && primary_src->value.type == VAL_STR && call_src->value.as.s && primary_src->value.as.s) {
        return value_bool(strcmp(call_src->value.as.s, primary_src->value.as.s) == 0);
    }
    return value_bool(true);
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

static Value builtin_extend(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    if (argc < 1 || !arg_nodes || !arg_nodes[0]) {
        RUNTIME_ERROR(interp, "EXTEND expects EXTENSION: name", line, col);
    }

    const char* spec = NULL;
    Expr* spec_node = arg_nodes[0];

    if (spec_node->type == EXPR_TYPED_IDENT) {
        spec = spec_node->as.typed_ident.name;
    } else if (spec_node->type == EXPR_IDENT) {
        spec = spec_node->as.ident;
    } else if (args && args[0].type == VAL_STR) {
        spec = args[0].as.s;
    }

    if (!spec || spec[0] == '\0') {
        RUNTIME_ERROR(interp, "EXTEND expects a non-empty extension specifier", line, col);
    }

    char* base_dir = module_source_dir_dup(env);
    const char* scope_name = module_scope_name(env);

    char* loaded_name = NULL;
    char* ext_err = NULL;
    int rc = extensions_load_named(spec, base_dir, scope_name, &loaded_name, &ext_err);
    free(base_dir);

    if (rc != 0) {
        if (ext_err) {
            interp->error = strdup(ext_err);
            free(ext_err);
        } else {
            interp->error = strdup("EXTEND failed to load extension");
        }
        interp->error_line = line;
        interp->error_col = col;
        free(loaded_name);
        return value_null();
    }

    // Expose the extension namespace symbol in the current module environment.
    if (loaded_name && loaded_name[0] != '\0') {
        (void)env_assign(env, loaded_name, value_str(""), TYPE_STR, true);

        EnvEntry* namespaces_entry = env_get_entry(env, "__EXTEND_NAMES__");
        const char* existing_namespaces = (namespaces_entry && namespaces_entry->initialized && namespaces_entry->value.type == VAL_STR && namespaces_entry->value.as.s)
            ? namespaces_entry->value.as.s
            : "";
        size_t loaded_len = strlen(loaded_name);
        int already_present = 0;
        const char* cursor = existing_namespaces;
        while (cursor && *cursor != '\0') {
            while (*cursor == '|') cursor++;
            if (*cursor == '\0') break;
            const char* start = cursor;
            while (*cursor != '\0' && *cursor != '|') cursor++;
            size_t token_len = (size_t)(cursor - start);
            if (token_len == loaded_len && strncmp(start, loaded_name, loaded_len) == 0) {
                already_present = 1;
                break;
            }
        }
        if (!already_present) {
            size_t existing_len = strlen(existing_namespaces);
            size_t combined_len = existing_len + loaded_len + 2;
            char* combined = malloc(combined_len + 1);
            if (!combined) {
                if (interp->error) free(interp->error);
                interp->error = strdup("Out of memory");
                interp->error_line = line;
                interp->error_col = col;
                free(loaded_name);
                return value_null();
            }
            if (existing_len > 0) {
                memcpy(combined, existing_namespaces, existing_len);
                combined[existing_len] = '|';
                memcpy(combined + existing_len + 1, loaded_name, loaded_len);
                combined[existing_len + loaded_len + 1] = '|';
                combined[existing_len + loaded_len + 2] = '\0';
            } else {
                combined[0] = '|';
                memcpy(combined + 1, loaded_name, loaded_len);
                combined[loaded_len + 1] = '|';
                combined[loaded_len + 2] = '\0';
            }
            (void)env_assign(env, "__EXTEND_NAMES__", value_str(combined), TYPE_STR, true);
            free(combined);
        }
    }

    free(loaded_name);
    free(ext_err);
    return value_bool(false);
}

static int module_export_bindings(Interpreter* interp, Env* caller_env, Env* mod_env, const char* alias, int line, int col, const char* fail_msg) {
    if (!interp || !caller_env || !mod_env || !alias || alias[0] == '\0') return -1;

    size_t alias_len = strlen(alias);

    for (size_t i = 0; i < mod_env->count; i++) {
        EnvEntry* e = &mod_env->entries[i];
        if (!e->initialized) continue;
        if (e->name && e->name[0] == '_' && e->name[1] == '_') continue;
        size_t qlen = alias_len + 1 + strlen(e->name) + 1;
        char* qualified = malloc(qlen);
        if (!qualified) return -1;
        snprintf(qualified, qlen, "%s.%s", alias, e->name);
        if (!env_assign(caller_env, qualified, e->value, e->decl_type, true)) {
            free(qualified);
            if (interp->error) free(interp->error);
            interp->error = strdup(fail_msg);
            interp->error_line = line;
            interp->error_col = col;
            return -1;
        }
        free(qualified);
    }

    EnvEntry* namespaces_entry = env_get_entry(mod_env, "__EXTEND_NAMES__");
    if (namespaces_entry && namespaces_entry->initialized && namespaces_entry->value.type == VAL_STR && namespaces_entry->value.as.s && namespaces_entry->value.as.s[0] != '\0') {
        const char* namespaces = namespaces_entry->value.as.s;
        const char* cursor = namespaces;
        while (cursor && *cursor != '\0') {
            while (*cursor == '|') cursor++;
            if (*cursor == '\0') break;

            const char* start = cursor;
            while (*cursor != '\0' && *cursor != '|') cursor++;
            size_t ns_len = (size_t)(cursor - start);
            if (ns_len == 0) continue;

                char* ns = malloc(ns_len + 1);
                if (!ns) return -1;
                memcpy(ns, start, ns_len);
                ns[ns_len] = '\0';

                /* Ensure any module-restricted extension operators registered
                    by the loaded extension are exposed under the importing
                    module's alias so callers can call alias.ext.NAME. */
                char* ext_err = NULL;
                (void)extensions_expose_named(ns, alias, &ext_err);
                if (ext_err) { free(ext_err); ext_err = NULL; }

            size_t ns_qual_len = alias_len + 1 + ns_len + 1;
            char* ns_qualified = malloc(ns_qual_len);
            if (!ns_qualified) { free(ns); return -1; }
            snprintf(ns_qualified, ns_qual_len, "%s.%s", alias, ns);
            if (!env_get_entry(caller_env, ns_qualified)) {
                if (!env_assign(caller_env, ns_qualified, value_str(""), TYPE_STR, true)) {
                    free(ns_qualified);
                    free(ns);
                    if (interp->error) free(interp->error);
                    interp->error = strdup(fail_msg);
                    interp->error_line = line;
                    interp->error_col = col;
                    return -1;
                }
            }

            for (size_t j = 0; j < mod_env->count; j++) {
                EnvEntry* e = &mod_env->entries[j];
                if (!e->initialized) continue;
                if (e->name && e->name[0] == '_' && e->name[1] == '_') continue;
                size_t qlen = ns_qual_len + 1 + strlen(e->name) + 1;
                char* qualified = malloc(qlen);
                if (!qualified) { free(ns_qualified); free(ns); return -1; }
                snprintf(qualified, qlen, "%s.%s", ns_qualified, e->name);
                if (!env_assign(caller_env, qualified, e->value, e->decl_type, true)) {
                    free(qualified);
                    free(ns_qualified);
                    free(ns);
                    if (interp->error) free(interp->error);
                    interp->error = strdup(fail_msg);
                    interp->error_line = line;
                    interp->error_col = col;
                    return -1;
                }
                free(qualified);
            }

            free(ns_qualified);
            free(ns);
        }
    }

    return 0;
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

    /* Search locations: referring dir, then primary-source lib/std, primary-source lib/usr,
       then executable lib/std, executable lib/usr. This keeps bundled modules ahead
       of user-installed modules while preserving local-directory precedence. */
    const char* search_dirs[5];
    search_dirs[0] = referer_dir;

    EnvEntry* primary_src_entry = interp && interp->global_env ? env_get_entry(interp->global_env, "__MODULE_SOURCE__") : NULL;
    char primary_program_dir[1024];
    char primary_std_dir[1024];
    char primary_usr_dir[1024];
    primary_program_dir[0] = '\0';
    primary_std_dir[0] = '\0';
    primary_usr_dir[0] = '\0';
    if (primary_src_entry && primary_src_entry->initialized && primary_src_entry->value.type == VAL_STR && primary_src_entry->value.as.s && primary_src_entry->value.as.s[0] != '\0') {
        strncpy(primary_program_dir, primary_src_entry->value.as.s, sizeof(primary_program_dir)-1);
        primary_program_dir[sizeof(primary_program_dir)-1] = '\0';
        char* last_sep = NULL;
        for (char* q = primary_program_dir; *q; q++) if (*q == '/' || *q == '\\') last_sep = q;
        if (last_sep) *last_sep = '\0';
        if (snprintf(primary_std_dir, sizeof(primary_std_dir), "%s/lib/std", primary_program_dir) >= 0) {
            search_dirs[1] = primary_std_dir;
        } else {
            search_dirs[1] = "lib/std";
        }
        if (snprintf(primary_usr_dir, sizeof(primary_usr_dir), "%s/lib/usr", primary_program_dir) >= 0) {
            search_dirs[2] = primary_usr_dir;
        } else {
            search_dirs[2] = "lib/usr";
        }
    } else {
        search_dirs[1] = "lib/std";
        search_dirs[2] = "lib/usr";
    }

    char exe_program_dir[1024];
    char exe_std_dir[1024];
    char exe_usr_dir[1024];
    exe_program_dir[0] = '\0';
    exe_std_dir[0] = '\0';
    exe_usr_dir[0] = '\0';
    if (g_argv && g_argv[0] && g_argv[0][0] != '\0') {
        strncpy(exe_program_dir, g_argv[0], sizeof(exe_program_dir)-1);
        exe_program_dir[sizeof(exe_program_dir)-1] = '\0';
        char* last_sep = NULL;
        for (char* q = exe_program_dir; *q; q++) if (*q == '/' || *q == '\\') last_sep = q;
        if (last_sep) *last_sep = '\0';
        if (snprintf(exe_std_dir, sizeof(exe_std_dir), "%s/lib/std", exe_program_dir) >= 0) {
            search_dirs[3] = exe_std_dir;
        } else {
            search_dirs[3] = "lib/std";
        }
        if (snprintf(exe_usr_dir, sizeof(exe_usr_dir), "%s/lib/usr", exe_program_dir) >= 0) {
            search_dirs[4] = exe_usr_dir;
        } else {
            search_dirs[4] = "lib/usr";
        }
    } else {
        search_dirs[3] = "lib/std";
        search_dirs[4] = "lib/usr";
    }

    for (int sd = 0; sd < 5 && !found_path; sd++) {
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
    /* Also register the caller-provided alias (if different) so callers can
       refer to the module by that identifier. IMPORT_PATH does this earlier;
       ensure builtin IMPORT behaves the same. */
    if (alias && strcmp(alias, cache_key) != 0) {
        (void)module_register_alias(interp, alias, mod_env);
    }

    EnvEntry* scope_entry = env_get_entry(mod_env, "__MODULE_SCOPE__");
    if (!scope_entry || !scope_entry->initialized || scope_entry->value.type != VAL_STR) {
        env_assign(mod_env, "__MODULE_SCOPE__", value_str(modname), TYPE_STR, true);
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

    if (module_export_bindings(interp, env, mod_env, alias, line, col, "IMPORT failed to assign qualified name") != 0) {
        return value_null();
    }

    // Ensure the module name itself exists in caller env (avoid undefined identifier errors)
    EnvEntry* alias_entry = env_get_entry(env, alias);
    if (!alias_entry) {
        if (!env_assign(env, alias, value_str("") , TYPE_STR, true)) {
            RUNTIME_ERROR(interp, "IMPORT failed to assign module name", line, col);
        }
    }

    return value_bool(false);
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
            case VAL_BOOL: elem_decl = TYPE_BOOL; break;
            case VAL_INT: elem_decl = TYPE_INT; break;
            case VAL_FLT: elem_decl = TYPE_FLT; break;
            case VAL_STR: elem_decl = TYPE_STR; break;
            case VAL_TNS: elem_decl = TYPE_TNS; break;
            case VAL_MAP: elem_decl = TYPE_MAP; break;
            case VAL_FUNC: elem_decl = TYPE_FUNC; break;
            case VAL_THR: elem_decl = TYPE_THR; break;
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
    return value_bool(value_thr_get_paused(args[0]) != 0);
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

// Local copy of interpreter helper to map ValueType -> DeclType
static DeclType value_type_to_decl(ValueType vt) {
    switch (vt) {
        case VAL_BOOL: return TYPE_BOOL;
        case VAL_INT: return TYPE_INT;
        case VAL_FLT: return TYPE_FLT;
        case VAL_STR: return TYPE_STR;
        case VAL_TNS: return TYPE_TNS;
        case VAL_MAP: return TYPE_MAP;
        case VAL_FUNC: return TYPE_FUNC;
        case VAL_THR: return TYPE_THR;
        default: return TYPE_UNKNOWN;
    }
}

// Local coercion helper (mirrors interpreter.c behavior but is available
// in this translation unit). Returns true on success and sets *out_value.
static bool coerce_value_to_decl_type(Interpreter* interp,
                                      Value input,
                                      DeclType target,
                                      Env* env,
                                      int line,
                                      int col,
                                      Value* out_value) {
    if (!out_value) return false;
    *out_value = value_null();

    if (value_type_to_decl(input.type) == target) {
        *out_value = value_copy(input);
        return true;
    }

    const char* builtin_name = NULL;
    switch (target) {
        case TYPE_BOOL: builtin_name = "BOOL"; break;
        case TYPE_INT: builtin_name = "INT"; break;
        case TYPE_FLT: builtin_name = "FLT"; break;
        case TYPE_STR: builtin_name = "STR"; break;
        case TYPE_TNS: builtin_name = "TNS"; break;
        default:
            return false;
    }

    BuiltinFunction* builtin = builtin_lookup(builtin_name);
    if (!builtin || !builtin->impl) return false;

    Value args[1];
    args[0] = input;
    Value converted = builtin->impl(interp, args, 1, NULL, env, line, col);
    if (interp->error) {
        return false;
    }
    if (value_type_to_decl(converted.type) != target) {
        value_free(converted);
        return false;
    }

    *out_value = converted;
    return true;
}

static int parallel_worker(void* arg) {
    ParallelStart* ps = (ParallelStart*)arg;
    // Create a per-worker interpreter state similar to PARFOR
    Interpreter* thr_interp = ps->interp;

    // Create a call environment from the function's closure and
    // perform normal function parameter binding (no-arg call).
    Env* call_env = env_create(ps->func->closure);

    // Bind parameters as the interpreter would for a user-call with
    // zero positional/keyword arguments. This ensures missing
    // required parameters and default-evaluation errors become
    // runtime errors (as mandated by the spec).
    for (size_t i = 0; i < ps->func->params.count; i++) {
        Param* param = &ps->func->params.items[i];
        Value arg_val = value_null();
        bool provided = false;

        if (param->default_value) {
            arg_val = eval_expr(thr_interp, param->default_value, call_env);
            if (thr_interp->error) {
                // transfer ownership of interpreter error into shared slot
                ps->errors[ps->index] = thr_interp->error;
                if (ps->err_lines) ps->err_lines[ps->index] = thr_interp->error_line;
                if (ps->err_cols) ps->err_cols[ps->index] = thr_interp->error_col;
                thr_interp->error = NULL;
                // cleanup
                env_free(call_env);
                free(thr_interp);
                free(ps);
                return 0;
            }
            provided = true;
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "Missing argument for parameter '%s'", param->name);
            ps->errors[ps->index] = strdup(buf);
            if (ps->err_lines) ps->err_lines[ps->index] = 0;
            if (ps->err_cols) ps->err_cols[ps->index] = 0;
            env_free(call_env);
            free(thr_interp);
            free(ps);
            return 0;
        }

        Value bind_val = arg_val;
        bool used_coercion = false;
        if (value_type_to_decl(bind_val.type) != param->type && param->coerced) {
            Value coerced = value_null();
            if (coerce_value_to_decl_type(thr_interp, arg_val, param->type, call_env, 0, 0, &coerced)) {
                bind_val = coerced;
                used_coercion = true;
            }
        }

        if (value_type_to_decl(bind_val.type) != param->type) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Type mismatch for parameter '%s'", param->name);
            ps->errors[ps->index] = strdup(buf);
            if (ps->err_lines) ps->err_lines[ps->index] = 0;
            if (ps->err_cols) ps->err_cols[ps->index] = 0;
            if (used_coercion) value_free(bind_val);
            value_free(arg_val);
            env_free(call_env);
            free(thr_interp);
            free(ps);
            return 0;
        }

        env_define(call_env, param->name, param->type);
        if (!env_assign(call_env, param->name, bind_val, param->type, true)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Cannot assign to frozen identifier '%s'", param->name);
            ps->errors[ps->index] = strdup(buf);
            if (ps->err_lines) ps->err_lines[ps->index] = 0;
            if (ps->err_cols) ps->err_cols[ps->index] = 0;
            if (used_coercion) value_free(bind_val);
            value_free(arg_val);
            env_free(call_env);
            free(thr_interp);
            free(ps);
            return 0;
        }

        // Release temporaries (env_assign copied value)
        value_free(arg_val);
        if (used_coercion) value_free(bind_val);
    }

    // Execute the function body as a proper call frame so RETURN is allowed
    ExecResult res = exec_program_in_env_as_function(thr_interp, ps->func->body, call_env, ps->func->name);

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

    return value_bool(false);
}


static const char* builtin_params_round[] = {"x", "ndigits", "mode"};
static const char* builtin_params_bytes[] = {"x", "endian"};
static const char* builtin_params_split[] = {"s", "delimiter"};
static const char* builtin_params_match[] = {"value", "template", "typing", "recurse", "shape"};
static const char* builtin_params_readfile[] = {"path", "coding"};
static const char* builtin_params_writefile[] = {"data", "path", "coding"};
static const char* builtin_params_pause[] = {"thr", "seconds"};
static const char* builtin_params_conv[] = {"x", "kernel", "stride_w", "stride_h", "pad_w", "pad_h", "bias"};

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
    {"CONV", 2, 7, builtin_conv, builtin_params_conv, 7},
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
    {"APPEND", 2, 2, builtin_append},
    {"MADD", 2, 2, builtin_madd},
    {"MSUB", 2, 2, builtin_msub},
    {"MMUL", 2, 2, builtin_mmul},
    {"MDIV", 2, 2, builtin_mdiv},
    {"MSUM", 1, -1, builtin_msum},
    {"MPROD", 1, -1, builtin_mprod},

    // Comparison
    {"EQ", 2, 2, builtin_eq},
    {"NEQ", 2, 2, builtin_neq},
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
    {"CONVERT", 2, 2, builtin_convert},
    {"BASE", 1, 1, builtin_base},
    {"BYTES", 1, 2, builtin_bytes, builtin_params_bytes, 2},
    {"SER", 1, 1, builtin_ser},
    {"UNSER", 1, 1, builtin_unser},

    // Type checking
    {"ISBOOL", 1, 1, builtin_isbool},
    {"ISINT", 1, 1, builtin_isint},
    {"ISFLT", 1, 1, builtin_isflt},
    {"ISSTR", 1, 1, builtin_isstr},
    {"ISTNS", 1, 1, builtin_istns},
    {"ISMAP", 1, 1, builtin_ismap},
    {"ISFUNC", 1, 1, builtin_isfunc},
    {"ISTHR", 1, 1, builtin_isthr},
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
    {"WARN", 0, -1, builtin_warn},
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
    {"REFUTE", 1, 1, builtin_refute},
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
    {"EXTEND", 1, 1, builtin_extend},
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
