#ifndef VALUE_H
#define VALUE_H

#include "ast.h"

struct EnvEntry; // forward declare for pointer values

typedef enum {
    VAL_NULL,
    VAL_INT,
    VAL_FLT,
    VAL_STR,
    VAL_TNS,
    VAL_MAP,
    VAL_FUNC
} ValueType;

// Forward declaration - Func is defined in interpreter.h
struct Func;
struct Value; // forward declare Value for Tensor.data

typedef struct Tensor {
    DeclType elem_type; // element static type
    size_t ndim;
    size_t* shape;    // length ndim
    size_t* strides;  // length ndim
    size_t length;    // total elements
    struct Value* data;      // length elements, contiguous row-major
    int refcount;
} Tensor;

typedef struct Value {
    ValueType type;
    union {
        int64_t i;
        double f;
        char* s;
        struct Func* func;
        struct Tensor* tns;
        struct Map* map;
    } as;
} Value;

typedef struct MapEntry {
    Value key;
    Value value;
} MapEntry;

typedef struct Map {
    MapEntry* items;
    size_t count;
    size_t capacity;
    int refcount;
} Map;

// Tensor helpers
Value value_tns_new(DeclType elem_type, size_t ndim, const size_t* shape);
Value value_tns_from_values(DeclType elem_type, size_t ndim, const size_t* shape, Value* items, size_t item_count);
Value value_tns_get(Value t, const size_t* idxs, size_t nidxs);
Value value_tns_slice(Value t, const int64_t* starts, const int64_t* ends, size_t n);

// Map helpers
Value value_map_new(void);
void value_map_set(Value* mapval, Value key, Value val);
Value value_map_get(Value mapval, Value key, int* found);
void value_map_delete(Value* mapval, Value key);


Value value_null(void);
Value value_int(int64_t v);
Value value_flt(double v);
Value value_str(const char* s);
Value value_func(struct Func* func);
// Note: pointer semantics are implemented at the EnvEntry (alias) level; no PTR Value type.

Value value_copy(Value v);
Value value_deep_copy(Value v);
void value_free(Value v);

const char* value_type_name(Value v);

#endif // VALUE_H