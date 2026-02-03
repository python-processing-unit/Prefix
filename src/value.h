#ifndef VALUE_H
#define VALUE_H

#include "ast.h"
#if !defined(__STDC_NO_THREADS__) && !defined(_MSC_VER)
#include <threads.h>
#define PREFIX_HAS_THREADS 1
#else
#define PREFIX_HAS_THREADS 0
#endif

struct EnvEntry; // forward declare for pointer values

typedef enum {
    VAL_NULL,
    VAL_INT,
    VAL_FLT,
    VAL_STR,
    VAL_TNS,
    VAL_MAP,
    VAL_FUNC,
    VAL_THR
} ValueType;

// Forward declaration - Func is defined in interpreter.h
struct Func;
struct Value; // forward declare Value for Tensor.data

typedef struct Thr {
    int finished; // 0 = running, 1 = finished/stopped
    int paused;
    int refcount;
#if PREFIX_HAS_THREADS
    thrd_t thread;
#endif
} Thr;

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
        struct Thr* thr;
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

// Pointer helpers (for lvalue/indexed assignment)
// Returns a pointer to the stored value for key, optionally creating a missing entry with NULL value.
// Returned pointer is owned by the map; do NOT free it.
Value* value_map_get_ptr(Value* mapval, Value key, bool create_if_missing);

// Returns a pointer to a tensor element for full indexing (nidxs must equal ndim).
// Returned pointer is owned by the tensor; do NOT free it.
Value* value_tns_get_ptr(Value t, const size_t* idxs, size_t nidxs);


Value value_null(void);
Value value_int(int64_t v);
Value value_flt(double v);
Value value_str(const char* s);
Value value_func(struct Func* func);
Value value_thr_new(void);
int value_thr_is_running(Value v);
void value_thr_set_finished(Value v, int finished);
// Note: pointer semantics are implemented at the EnvEntry (alias) level; no PTR Value type.

Value value_copy(Value v);
Value value_deep_copy(Value v);
void value_free(Value v);

const char* value_type_name(Value v);

#endif // VALUE_H