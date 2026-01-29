#include "builtins.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
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

    // Register module using the canonical path string as name to ensure uniqueness
    if (module_register(interp, inpath) != 0) {
        if (alias_dup) free(alias_dup);
        RUNTIME_ERROR(interp, "IMPORT_PATH failed to register module", line, col);
    }

    Env* mod_env = module_env_lookup(interp, inpath);
    if (!mod_env) {
        if (alias_dup) free(alias_dup);
        RUNTIME_ERROR(interp, "IMPORT_PATH failed to lookup module env", line, col);
    }

    // If not already loaded, attempt to load file or directory
    EnvEntry* marker = env_get_entry(mod_env, "__MODULE_LOADED__");
    if (!marker || !marker->initialized) {
        // Determine if path is file or directory
        struct stat st;
        char candidate[2048];
        char* found_path = NULL;

        if (stat(inpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
            // directory -> require init.pre
            if (snprintf(candidate, sizeof(candidate), "%s/init.pre", inpath) >= 0) {
                if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                    found_path = strdup(candidate);
                } else {
                    if (alias_dup) free(alias_dup);
                    RUNTIME_ERROR(interp, "IMPORT_PATH: package missing init.pre", line, col);
                }
            }
        } else {
            // try file as given
            if (stat(inpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                found_path = strdup(inpath);
            } else {
                // try appending .pre
                if (snprintf(candidate, sizeof(candidate), "%s.pre", inpath) >= 0) {
                    if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                        found_path = strdup(candidate);
                    }
                }
            }
        }

        if (found_path) {
            FILE* f = fopen(found_path, "rb");
            char* srcbuf = NULL;
            if (f) {
                fseek(f, 0, SEEK_END);
                long len = ftell(f);
                fseek(f, 0, SEEK_SET);
                srcbuf = malloc((size_t)len + 1);
                if (!srcbuf) { fclose(f); free(found_path); if (alias_dup) free(alias_dup); RUNTIME_ERROR(interp, "Out of memory", line, col); }
                if (fread(srcbuf, 1, (size_t)len, f) != (size_t)len) { free(srcbuf); fclose(f); free(found_path); srcbuf = NULL; }
                if (srcbuf) {
                    srcbuf[len] = '\0';
                    fclose(f);
                    // Set module source so nested IMPORTs prefer this dir
                    env_assign(mod_env, "__MODULE_SOURCE__", value_str(found_path), TYPE_STR, true);

                    Lexer lex;
                    lexer_init(&lex, srcbuf, found_path);
                    Parser parser;
                    parser_init(&parser, &lex);
                    Stmt* program = parser_parse(&parser);
                    if (parser.had_error) {
                        free(srcbuf);
                        free(found_path);
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
                        if (res.error) interp->error = strdup(res.error);
                        interp->error_line = res.error_line;
                        interp->error_col = res.error_column;
                        free(res.error);
                        if (alias_dup) free(alias_dup);
                        return value_null();
                    }

                    // mark loaded
                    env_assign(mod_env, "__MODULE_LOADED__", value_int(1), TYPE_INT, true);

                    free(srcbuf);
                    free(found_path);
                }
            }
        }
        // If not found, allow module env to be populated by extensions
    }

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
    // SLICE can operate on STR or TNS for now. Syntax: SLICE(target, start, end)
    if (args[0].type == VAL_STR) {
        EXPECT_INT(args[1], "SLICE", interp, line, col);
        EXPECT_INT(args[2], "SLICE", interp, line, col);
        const char* s = args[0].as.s;
        size_t len = strlen(s);
        int64_t start = args[1].as.i;
        int64_t end = args[2].as.i;
        if (start < 0) start = (int64_t)len + start + 1;
        if (end < 0) end = (int64_t)len + end + 1;
        start--;
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
    } else if (args[0].type == VAL_TNS) {
        // slice along first axis using 1-based inclusive indices
        EXPECT_INT(args[1], "SLICE", interp, line, col);
        EXPECT_INT(args[2], "SLICE", interp, line, col);
        int64_t starts[1] = { args[1].as.i };
        int64_t ends[1] = { args[2].as.i };
        return value_tns_slice(args[0], starts, ends, 1);
    }
    RUNTIME_ERROR(interp, "SLICE expects STR or TNS", line, col);
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
            int hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
            int lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) { fclose(f); RUNTIME_ERROR(interp, "WRITEFILE(hex) expects valid hex digits", line, col); }
            unsigned char byte = (unsigned char)((hi << 4) | lo);
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
                    case TYPE_FUNC: tname = "FUNC"; break;
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
                case TYPE_FUNC: rname = "FUNC"; break;
                default: rname = "ANY"; break;
            }
            strcat(buf, rname);
            Value out = value_str(buf);
            free(buf);
            return out;
        }
    }

    // If not in environment or not a function there, check the interpreter function table
    Func* ff = NULL;
    if (interp && interp->functions) ff = func_table_lookup(interp->functions, name);
    if (ff != NULL) {
        struct Func* f = ff;
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
                case TYPE_FUNC: tname = "FUNC"; break;
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
            case TYPE_FUNC: rname = "FUNC"; break;
            default: rname = "ANY"; break;
        }
        strcat(buf, rname);
        Value out = value_str(buf);
        free(buf);
        return out;
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
        case TYPE_FUNC: tname = "FUNC"; break;
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
        // recurse: if true and both are maps, apply recursively
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

    // Ensure module registry entry exists
    if (module_register(interp, modname) != 0) {
        RUNTIME_ERROR(interp, "IMPORT failed to register module", line, col);
    }

    // Lookup module env
    Env* mod_env = module_env_lookup(interp, modname);
    if (!mod_env) {
        RUNTIME_ERROR(interp, "IMPORT failed to lookup module env", line, col);
    }

    // If not already loaded, attempt to locate and load a .pre file
    EnvEntry* marker = env_get_entry(mod_env, "__MODULE_LOADED__");
    if (!marker || !marker->initialized) {
        // Determine referring directory from caller env's __MODULE_SOURCE__ if present
        const char* referer_source = NULL;
        EnvEntry* src_entry = env_get_entry(env, "__MODULE_SOURCE__");
        if (src_entry && src_entry->initialized && src_entry->value.type == VAL_STR) {
            referer_source = src_entry->value.as.s;
        }

        char referer_dir[1024] = {0};
        if (referer_source && referer_source[0] != '\0') {
            // Extract directory portion
            strncpy(referer_dir, referer_source, sizeof(referer_dir)-1);
            char* last_sep = NULL;
            for (char* p = referer_dir; *p; p++) if (*p == '/' || *p == '\\') last_sep = p;
            if (last_sep) *last_sep = '\0';
        } else {
            // Empty string => use current working directory (.)
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

        // Helper to test file/dir existence
        struct stat st;
        char candidate[2048];
        char* found_path = NULL;
        char* srcbuf = NULL;

        // Search locations: referring dir, then lib/
        const char* search_dirs[2];
        search_dirs[0] = referer_dir;
        search_dirs[1] = "lib";

        for (int sd = 0; sd < 2 && !found_path; sd++) {
            const char* sdir = search_dirs[sd];
            if (!sdir) continue;

            // For package-name (single component) and for multi-component both
            // we use package precedence: if directory exists, require init.pre; else try .pre file
            if (snprintf(candidate, sizeof(candidate), "%s/%s", sdir, base) < 0) continue;
            if (stat(candidate, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
                // directory exists -> require init.pre inside
                char initpath[2048];
                if (snprintf(initpath, sizeof(initpath), "%s/%s/init.pre", sdir, base) < 0) continue;
                if (stat(initpath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                    found_path = strdup(initpath);
                    break;
                } else {
                    // Directory exists but no init.pre -> error per spec
                    char buf[256];
                    snprintf(buf, sizeof(buf), "IMPORT: package '%s' missing init.pre", modname);
                    RUNTIME_ERROR(interp, buf, line, col);
                }
            }

            // Otherwise try file sdir/base.pre
            char filepath[2048];
            if (snprintf(filepath, sizeof(filepath), "%s/%s.pre", sdir, base) < 0) continue;
            if (stat(filepath, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
                found_path = strdup(filepath);
                break;
            }
        }

        if (found_path) {
            FILE* f = fopen(found_path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long len = ftell(f);
                fseek(f, 0, SEEK_SET);
                srcbuf = malloc((size_t)len + 1);
                if (!srcbuf) { fclose(f); free(found_path); RUNTIME_ERROR(interp, "Out of memory", line, col); }
                if (fread(srcbuf, 1, (size_t)len, f) != (size_t)len) { free(srcbuf); fclose(f); free(found_path); srcbuf = NULL; }
                if (srcbuf) {
                    srcbuf[len] = '\0';
                    fclose(f);
                    // Set module source so nested IMPORTs prefer this dir
                    env_assign(mod_env, "__MODULE_SOURCE__", value_str(found_path), TYPE_STR, true);

                    // Parse and execute in module env
                    Lexer lex;
                    lexer_init(&lex, srcbuf, found_path);
                    Parser parser;
                    parser_init(&parser, &lex);
                    Stmt* program = parser_parse(&parser);
                    if (parser.had_error) {
                        free(srcbuf);
                        free(found_path);
                        interp->error = strdup("IMPORT: parse error");
                        interp->error_line = parser.current_token.line;
                        interp->error_col = parser.current_token.column;
                        return value_null();
                    }

                    ExecResult res = exec_program_in_env(interp, program, mod_env);
                    if (res.status == EXEC_ERROR) {
                        free(srcbuf);
                        free(found_path);
                        interp->error = res.error ? strdup(res.error) : strdup("Runtime error in IMPORT");
                        interp->error_line = res.error_line;
                        interp->error_col = res.error_column;
                        free(res.error);
                        return value_null();
                    }

                    // mark loaded
                    env_assign(mod_env, "__MODULE_LOADED__", value_int(1), TYPE_INT, true);

                    free(srcbuf);
                    srcbuf = NULL;
                    free(found_path);
                }
            }
        }
        // If not found, do nothing (module env may be populated by extensions)
    }

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
            items[i] = value_copy(args[1]);
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

// Forward declare ARGV builtin so it can be referenced in the table
static Value builtin_argv(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);
// Forward declare RUN builtin so it can be referenced in the table
static Value builtin_run(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);
// Forward declare SHUSH/UNSHUSH so they can be referenced in the table
static Value builtin_shush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);
static Value builtin_unshush(Interpreter* interp, Value* args, int argc, Expr** arg_nodes, Env* env, int line, int col);

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
    {"BYTES", 1, 2, builtin_bytes},

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
    {"SPLIT", 1, 2, builtin_split},
    {"IN", 2, 2, builtin_in},
    {"KEYS", 1, 1, builtin_keys},
    {"VALUES", 1, 1, builtin_values},
    {"KEYIN", 2, 2, builtin_keyin},
    {"VALUEIN", 2, 2, builtin_valuein},
    {"MATCH", 2, 5, builtin_match},
    {"ILEN", 1, 1, builtin_ilen},
    {"LEN", 0, -1, builtin_len},

    // I/O
    {"PRINT", 0, -1, builtin_print},
    {"INPUT", 0, 1, builtin_input},
    {"SHUSH", 0, 0, builtin_shush},
    {"UNSHUSH", 0, 0, builtin_unshush},
    {"READFILE", 1, 2, builtin_readfile},
    {"WRITEFILE", 2, 3, builtin_writefile},
    {"CL", 1, 1, builtin_cl},
    {"EXISTFILE", 1, 1, builtin_existfile},
    {"DELETEFILE", 1, 1, builtin_deletefile},
    {"RUN", 1, 1, builtin_run},
    {"ARGV", 0, 0, builtin_argv},

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

// Global argv storage for ARGV builtin
static int g_argc = 0;
static char** g_argv = NULL;

void builtins_set_argv(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;
}

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
