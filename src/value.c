#include "value.h"
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

Value value_null(void) {
    Value v; v.type = VAL_NULL; return v;
}

Value value_int(int64_t v) {
    Value val; val.type = VAL_INT; val.as.i = v; return val;
}

Value value_flt(double v) {
    Value val; val.type = VAL_FLT; val.as.f = v; return val;
}

Value value_str(const char* s) {
    Value val; val.type = VAL_STR; val.as.s = s ? strdup(s) : strdup(""); return val;
}

Value value_func(struct Func* func) {
    Value val; val.type = VAL_FUNC; val.as.func = func; return val;
}

Value value_copy(Value v) {
    Value out = v;
    if (v.type == VAL_STR && v.as.s) {
        out.as.s = strdup(v.as.s);
    }
    return out;
}

void value_free(Value v) {
    if (v.type == VAL_STR) {
        free(v.as.s);
    }
}

const char* value_type_name(Value v) {
    switch (v.type) {
        case VAL_INT: return "INT";
        case VAL_FLT: return "FLT";
        case VAL_STR: return "STR";
        case VAL_FUNC: return "FUNC";
        default: return "NULL";
    }
}