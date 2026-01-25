#ifndef VALUE_H
#define VALUE_H

#include "ast.h"

typedef enum {
    VAL_NULL,
    VAL_INT,
    VAL_FLT,
    VAL_STR,
    VAL_FUNC
} ValueType;

// Forward declaration - Func is defined in interpreter.h
struct Func;

typedef struct {
    ValueType type;
    union {
        int64_t i;
        double f;
        char* s;
        struct Func* func;
    } as;
} Value;

Value value_null(void);
Value value_int(int64_t v);
Value value_flt(double v);
Value value_str(const char* s);
Value value_func(struct Func* func);

Value value_copy(Value v);
void value_free(Value v);

const char* value_type_name(Value v);

#endif // VALUE_H