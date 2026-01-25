#include "builtins.h"
#include "interpreter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

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

// ============ Arithmetic operators ============

static Value builtin_add(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "ADD", interp, line, col);
    EXPECT_NUM(args[1], "ADD", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "ADD cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i + args[1].as.i);
    }
    return value_flt(args[0].as.f + args[1].as.f);
}

static Value builtin_sub(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "SUB", interp, line, col);
    EXPECT_NUM(args[1], "SUB", interp, line, col);
    
    if (args[0].type != args[1].type) {
        RUNTIME_ERROR(interp, "SUB cannot mix INT and FLT", line, col);
    }
    
    if (args[0].type == VAL_INT) {
        return value_int(args[0].as.i - args[1].as.i);
    }
    return value_flt(args[0].as.f - args[1].as.f);
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
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "FADD", interp, line, col);
    EXPECT_NUM(args[1], "FADD", interp, line, col);
    
    double a = args[0].type == VAL_FLT ? args[0].as.f : (double)args[0].as.i;
    double b = args[1].type == VAL_FLT ? args[1].as.f : (double)args[1].as.i;
    return value_flt(a + b);
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

static Value builtin_eq(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp;
    
    if (args[0].type != args[1].type) {
        return value_int(0);
    }
    
    switch (args[0].type) {
        case VAL_INT:
            return value_int(args[0].as.i == args[1].as.i ? 1 : 0);
        case VAL_FLT:
            return value_int(args[0].as.f == args[1].as.f ? 1 : 0);
        case VAL_STR:
            return value_int(strcmp(args[0].as.s, args[1].as.s) == 0 ? 1 : 0);
        case VAL_FUNC:
            return value_int(args[0].as.func == args[1].as.func ? 1 : 0);
        default:
            return value_int(0);
    }
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

static Value builtin_join(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // JOIN(sep, str1, str2, ...) or variadic
    if (argc < 1) {
        RUNTIME_ERROR(interp, "JOIN requires at least 1 argument", line, col);
    }
    EXPECT_STR(args[0], "JOIN", interp, line, col);
    
    const char* sep = args[0].as.s;
    size_t sep_len = strlen(sep);
    
    // Calculate total length
    size_t total = 0;
    for (int i = 1; i < argc; i++) {
        EXPECT_STR(args[i], "JOIN", interp, line, col);
        total += strlen(args[i].as.s);
        if (i > 1) total += sep_len;
    }
    
    char* result = malloc(total + 1);
    result[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(result, sep);
        strcat(result, args[i].as.s);
    }
    
    Value v = value_str(result);
    free(result);
    return v;
}

static Value builtin_split(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    // SPLIT returns a tensor in Python but for now we can't return tensors
    // For Stage 3, we'll skip tensor-returning operations
    RUNTIME_ERROR(interp, "SPLIT requires TNS support (Stage 4)", line, col);
}

static Value builtin_slice(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_STR(args[0], "SLICE", interp, line, col);
    EXPECT_INT(args[1], "SLICE", interp, line, col);
    EXPECT_INT(args[2], "SLICE", interp, line, col);
    
    const char* s = args[0].as.s;
    size_t len = strlen(s);
    int64_t start = args[1].as.i;
    int64_t end = args[2].as.i;
    
    // Convert 1-based to 0-based, handle negative indices
    if (start < 0) start = (int64_t)len + start + 1;
    if (end < 0) end = (int64_t)len + end + 1;
    start--; // Convert to 0-based
    
    if (start < 0) start = 0;
    if (end > (int64_t)len) end = (int64_t)len;
    if (start >= end) return value_str("");
    
    size_t result_len = (size_t)(end - start);
    char* result = malloc(result_len + 1);
    memcpy(result, s + start, result_len);
    result[result_len] = '\0';
    
    Value v = value_str(result);
    free(result);
    return v;
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
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        switch (args[i].type) {
            case VAL_INT: {
                char* s = int_to_binary_str(args[i].as.i);
                printf("%s", s);
                free(s);
                break;
            }
            case VAL_FLT: {
                char* s = flt_to_binary_str(args[i].as.f);
                printf("%s", s);
                free(s);
                break;
            }
            case VAL_STR:
                printf("%s", args[i].as.s);
                break;
            case VAL_FUNC:
                printf("<func %p>", (void*)args[i].as.func);
                break;
            default:
                printf("<null>");
                break;
        }
    }
    printf("\n");
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

static Value builtin_type(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_str(value_type_name(args[0]));
}

// ============ Variable management ============

static Value builtin_del(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args;
    
    if (argc != 1 || arg_nodes[0]->type != EXPR_IDENT) {
        RUNTIME_ERROR(interp, "DEL expects an identifier", line, col);
    }
    
    const char* name = arg_nodes[0]->as.ident;
    if (!env_delete(env, name)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Cannot delete undefined identifier '%s'", name);
        RUNTIME_ERROR(interp, buf, line, col);
    }
    return value_int(0);
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
    
    int64_t places = 0;
    if (argc >= 2) {
        EXPECT_INT(args[1], "ROUND", interp, line, col);
        places = args[1].as.i;
    }
    
    if (args[0].type == VAL_INT) {
        if (places >= 0) {
            return value_int(args[0].as.i);
        }
        // Negative places: round to that power of 2
        int64_t factor = 1LL << (-places);
        return value_int((args[0].as.i / factor) * factor);
    }
    
    double val = args[0].as.f;
    if (places >= 0) {
        double factor = (double)(1LL << places);
        return value_flt(round(val * factor) / factor);
    } else {
        double factor = (double)(1LL << (-places));
        return value_flt(round(val / factor) * factor);
    }
}

// INV (1/x)
static Value builtin_inv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env;
    EXPECT_NUM(args[0], "INV", interp, line, col);
    
    if (args[0].type == VAL_INT) {
        if (args[0].as.i == 0) {
            RUNTIME_ERROR(interp, "Division by zero", line, col);
        }
        if (args[0].as.i == 1) return value_int(1);
        if (args[0].as.i == -1) return value_int(-1);
        return value_int(0);  // Integer inverse of anything else is 0 (floor)
    }
    
    if (args[0].as.f == 0.0) {
        RUNTIME_ERROR(interp, "Division by zero", line, col);
    }
    return value_flt(1.0 / args[0].as.f);
}

// COPY (shallow copy for scalars)
static Value builtin_copy(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    return value_copy(args[0]);
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
    RUNTIME_ERROR(interp, "LEN requires TNS support for non-string arguments (Stage 4)", line, col);
}

// Main, OS
static Value builtin_main(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
    // Return 1 if this is the main module
    return value_int(1);
}

static Value builtin_os(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col) {
    (void)args; (void)argc; (void)arg_nodes; (void)env; (void)interp; (void)line; (void)col;
#ifdef _WIN32
    return value_str("Windows");
#elif defined(__APPLE__)
    return value_str("Darwin");
#elif defined(__linux__)
    return value_str("Linux");
#else
    return value_str("Unknown");
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
    (void)args; (void)argc; (void)arg_nodes; (void)env;
    // For now, just acknowledge import but do nothing
    return value_int(0);
}

// ============ Builtins table ============

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
    {"ROUND", 1, 3, builtin_round},
    
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
    
    // Type checking
    {"ISINT", 1, 1, builtin_isint},
    {"ISFLT", 1, 1, builtin_isflt},
    {"ISSTR", 1, 1, builtin_isstr},
    {"TYPE", 1, 1, builtin_type},
    
    // String operations
    {"SLEN", 1, 1, builtin_slen},
    {"UPPER", 1, 1, builtin_upper},
    {"LOWER", 1, 1, builtin_lower},
    {"SLICE", 3, 3, builtin_slice},
    {"REPLACE", 3, 3, builtin_replace},
    {"STRIP", 2, 2, builtin_strip},
    {"JOIN", 1, -1, builtin_join},
    {"SPLIT", 1, 2, builtin_split},
    {"ILEN", 1, 1, builtin_ilen},
    {"LEN", 0, -1, builtin_len},
    
    // I/O
    {"PRINT", 0, -1, builtin_print},
    {"INPUT", 0, 1, builtin_input},
    
    // Control
    {"ASSERT", 1, 1, builtin_assert},
    {"THROW", 0, -1, builtin_throw},
    
    // Variables
    {"DEL", 1, 1, builtin_del},
    {"EXIST", 1, 1, builtin_exist},
    {"COPY", 1, 1, builtin_copy},
    
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
    
    // Sentinel
    {NULL, 0, 0, NULL}
};

void builtins_init(void) {
    // Nothing to initialize for now, table is static
}

BuiltinFunction* builtin_lookup(const char* name) {
    for (int i = 0; builtins_table[i].name != NULL; i++) {
        if (strcmp(builtins_table[i].name, name) == 0) {
            return &builtins_table[i];
        }
    }
    return NULL;
}

bool is_builtin(const char* name) {
    return builtin_lookup(name) != NULL;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
