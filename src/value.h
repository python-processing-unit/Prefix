#ifndef VALUE_H
#define VALUE_H

#include "ast.h"
#include "win32_shim.h" // Hardware threading shim is required for Windows builds.

struct EnvEntry; // forward declare for pointer values
struct Env;      // forward declare Env

typedef enum { VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_STR, VAL_TENSOR, VAL_MAP, VAL_FUNC, VAL_THREAD } ValueType;

// Forward declaration - Func is defined in interpreter.h
struct Func;
struct Value; // forward declare Value for Tensor.data

typedef struct Thread {
    int finished; // 0 = running, 1 = finished/stopped
    int paused;
    int stop_requested;
    int refcount;
#if 1
    int started;
    struct Stmt *body;
    struct Env *env;
#endif
    mtx_t state_lock;
    thrd_t thread;
} Thread;

typedef struct Tensor {
    DeclType elem_type; // element static type
    size_t ndim;
    size_t *shape;      // length ndim
    size_t *strides;    // length ndim
    size_t length;      // total elements
    struct Value *data; // length elements, contiguous row-major
    int refcount;
    mtx_t lock;
} Tensor;

typedef struct Value {
    ValueType type;
  int num_base;     // 2..64 for numeric values, default 2
  int num_base_nan; // 1 when float has NaN base (INF/NaN literals), else 0
    union {
        bool boolean;
        int64_t i;
        double f;
        char *s;
        struct Func *func;
        struct Tensor *tensor;
        struct Map *map;
        struct Thread *thread;
    } as;
} Value;

typedef struct MapEntry {
    Value key;
    Value value;
    int64_t next_hash;
} MapEntry;

typedef struct Map {
    MapEntry *items;
    size_t count;
    size_t capacity;
    int64_t *buckets;
    size_t bucket_count;
    int refcount;
    mtx_t lock;
} Map;

// Tensor helpers
Value value_tensor_new(DeclType elem_type, size_t ndim, const size_t *shape);
Value value_tensor_from_values(DeclType elem_type, size_t ndim, const size_t *shape, Value *items, size_t item_count);
Value value_tensor_get(Value t, const size_t *idxs, size_t nidxs);
Value value_tensor_slice(Value t, const int64_t *starts, const int64_t *ends, size_t n);

// Map helpers
Value value_map_new(void);
void value_map_set(Value *mapval, Value key, Value val);
Value value_map_get(Value mapval, Value key, int *found);
void value_map_delete(Value *mapval, Value key);

// Set map entry value to an alias pointing to the map itself (SELF semantics)
void value_map_set_self(Value *mapval, Value key);

// Pointer helpers (for lvalue/indexed assignment)
// Returns a pointer to the stored value for key, optionally creating a missing entry with NULL value.
// Returned pointer is owned by the map; do NOT free it.
Value *value_map_get_ptr(Value *mapval, Value key, bool create_if_missing);

// Returns a pointer to a tensor element for full indexing (nidxs must equal ndim).
// Returned pointer is owned by the tensor; do NOT free it.
Value *value_tensor_get_ptr(Value t, const size_t *idxs, size_t nidxs);

Value value_null(void);
Value value_bool(bool v);
Value value_int(int64_t v);
Value value_float(double v);
Value value_int_base(int64_t v, int base);
Value value_float_base(double v, int base);
Value value_float_nan_base(double v);
Value value_str(const char *s);
Value value_func(struct Func *func);
Value value_thread_new(void);
int value_thread_is_running(Value v);
void value_thread_set_finished(Value v, int finished);
int value_thread_get_finished(Value v);
void value_thread_set_paused(Value v, int paused);
int value_thread_get_paused(Value v);
void value_thread_set_stop_requested(Value v, int stop_requested);
int value_thread_get_stop_requested(Value v);
void value_thread_set_started(Value v, int started);
int value_thread_get_started(Value v);
// Note: pointer semantics are implemented at the EnvEntry (alias) level; no PTR Value type.

Value value_copy(Value v);
Value value_alias(Value v);
Value value_deep_copy(Value v);
void value_free(Value v);

// Numeric base helpers for named int/float types
int value_num_base(Value v);
DeclType value_to_decl_type(Value v);
const char *value_type_name(Value v);

// Strict declared-type equality (bases matter for named numbers).
bool decl_type_equal(DeclType a, int base_a, DeclType b, int base_b);

// Whether a declared type accepts a value (parent int/float accept any base).
bool decl_type_accepts_value(DeclType expected, int expected_base, Value value);

// Format a type name including base for int/float. Returns buf.
const char *decl_type_name_base(DeclType type, int base, char *buf, size_t buf_size);

// Validate a map value against a map schema (same semantics as VALIDATE with defaults).
// Returns true if all keys in `templ` exist in `map` and satisfy the validate (typing=0, recurse=0, shape=0).
// Supports "validate" metadata override in the schema.
bool value_map_matches(Value map, Value templ);

#endif // VALUE_H