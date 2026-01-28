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
    
    // Type checking
    {"ISINT", 1, 1, builtin_isint},
    {"ISFLT", 1, 1, builtin_isflt},
    {"ISSTR", 1, 1, builtin_isstr},
    {"TYPE", 1, 1, builtin_type},
    
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
