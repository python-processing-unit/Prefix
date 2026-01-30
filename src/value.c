#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

// Create a pointer value referring to a binding name
// Pointer values removed: aliasing is handled at EnvEntry level


static size_t compute_strides(const size_t* shape, size_t ndim, size_t* out_strides) {
    size_t len = 1;
    for (size_t i = ndim; i-- > 0;) {
        out_strides[i] = len;
        len *= shape[i];
    }
    return len;
}

Value value_tns_new(DeclType elem_type, size_t ndim, const size_t* shape) {
    Value v;
    v.type = VAL_TNS;
    Tensor* t = malloc(sizeof(Tensor));
    if (!t) { fprintf(stderr, "Out of memory\n"); exit(1); }
    t->elem_type = elem_type;
    t->ndim = ndim;
    t->shape = malloc(sizeof(size_t) * ndim);
    t->strides = malloc(sizeof(size_t) * ndim);
    for (size_t i = 0; i < ndim; i++) t->shape[i] = shape[i];
    t->length = compute_strides(shape, ndim, t->strides);
    t->data = malloc(sizeof(Value) * (t->length));
    for (size_t i = 0; i < t->length; i++) t->data[i] = value_null();
    t->refcount = 1;
    v.as.tns = t;
    return v;
}

Value value_tns_from_values(DeclType elem_type, size_t ndim, const size_t* shape, Value* items, size_t item_count) {
    Value tval = value_tns_new(elem_type, ndim, shape);
    Tensor* t = tval.as.tns;
    if (item_count != t->length) {
        // Fill with nulls if mismatched
        size_t to_copy = item_count < t->length ? item_count : t->length;
        for (size_t i = 0; i < to_copy; i++) t->data[i] = value_copy(items[i]);
    } else {
        for (size_t i = 0; i < t->length; i++) t->data[i] = value_copy(items[i]);
    }
    return tval;
}

Value value_tns_get(Value v, const size_t* idxs, size_t nidxs) {
    if (v.type != VAL_TNS) return value_null();
    Tensor* t = v.as.tns;
    assert(nidxs <= t->ndim);
    size_t offset = 0;
    for (size_t i = 0; i < nidxs; i++) {
        size_t idx = idxs[i];
        if (idx >= t->shape[i]) {
            return value_null();
        }
        offset += idx * t->strides[i];
    }
    // If full indexing (nidxs == ndim) return element, else return a view (slice) as a new tensor
    if (nidxs == t->ndim) {
        return value_copy(t->data[offset]);
    } else {
        // Build shape for sub-tensor
        size_t new_ndim = t->ndim - nidxs;
        size_t* new_shape = malloc(sizeof(size_t) * new_ndim);
        for (size_t i = 0; i < new_ndim; i++) new_shape[i] = t->shape[nidxs + i];
        // Create new tensor and copy data
        Value out = value_tns_new(t->elem_type, new_ndim, new_shape);
        Tensor* ot = out.as.tns;
        // Copy contiguous block segments if lower dimensions match contiguous layout
        size_t copy_count = ot->length;
        for (size_t i = 0; i < copy_count; i++) {
            // compute source index in original
            size_t src = offset + i; // works because original is row-major and subarray is contiguous
            ot->data[i] = value_copy(t->data[src]);
        }
        free(new_shape);
        return out;
    }
}

Value value_tns_slice(Value v, const int64_t* starts, const int64_t* ends, size_t n) {
    // starts/ends are 1-based inclusive per spec; negative values handled as Python-style
    if (v.type != VAL_TNS) return value_null();
    Tensor* t = v.as.tns;
    size_t use_n = n < t->ndim ? n : t->ndim;

    // normalized starts/ends in 1-based inclusive form -> we'll keep int64_t
    int64_t* nstarts = malloc(sizeof(int64_t) * t->ndim);
    int64_t* nends = malloc(sizeof(int64_t) * t->ndim);
    for (size_t i = 0; i < t->ndim; i++) {
        if (i < use_n) {
            int64_t s = starts[i];
            int64_t e = ends[i];
            int64_t dim = (int64_t)t->shape[i];
            if (s < 0) s = dim + s + 1;
            if (e < 0) e = dim + e + 1;
            if (s < 1) s = 1;
            if (e > dim) e = dim;
            if (s > e) {
                nstarts[i] = 1; nends[i] = 0; // empty
            } else {
                nstarts[i] = s;
                nends[i] = e;
            }
        } else {
            nstarts[i] = 1;
            nends[i] = (int64_t)t->shape[i];
        }
    }

    // Determine output shape by removing dimensions that are fixed to a single element
    size_t new_ndim = 0;
    int* orig_to_out = malloc(sizeof(int) * t->ndim);
    for (size_t i = 0; i < t->ndim; i++) {
        size_t len = (nends[i] >= nstarts[i]) ? (size_t)(nends[i] - nstarts[i] + 1) : 0;
        if (len <= 1) {
            orig_to_out[i] = -1; // fixed
        } else {
            orig_to_out[i] = (int)new_ndim++;
        }
    }

    if (new_ndim == 0) {
        // All dimensions fixed -> return single element
        size_t src_offset = 0;
        for (size_t i = 0; i < t->ndim; i++) {
            size_t pos = (nends[i] >= nstarts[i]) ? (size_t)(nstarts[i] - 1) : 0;
            src_offset += pos * t->strides[i];
        }
        Value out = value_copy(t->data[src_offset]);
        free(nstarts); free(nends); free(orig_to_out);
        return out;
    }

    size_t* new_shape = malloc(sizeof(size_t) * new_ndim);
    for (size_t i = 0; i < t->ndim; i++) {
        if (orig_to_out[i] >= 0) {
            new_shape[orig_to_out[i]] = (size_t)(nends[i] - nstarts[i] + 1);
        }
    }

    Value out = value_tns_new(t->elem_type, new_ndim, new_shape);
    Tensor* ot = out.as.tns;

    // iterate over output positions and copy corresponding element
    for (size_t out_idx = 0; out_idx < ot->length; out_idx++) {
        // compute multi-index for out
        size_t rem = out_idx;
        size_t src_offset = 0;
        for (size_t d = 0; d < new_ndim; d++) {
            size_t pos = rem / ot->strides[d];
            rem = rem % ot->strides[d];
            // find corresponding original dimension
            // scan orig_to_out to find index with value == d
            size_t orig = 0;
            for (size_t k = 0; k < t->ndim; k++) {
                if (orig_to_out[k] == (int)d) { orig = k; break; }
            }
            size_t src_pos = pos + (size_t)(nstarts[orig] - 1);
            src_offset += src_pos * t->strides[orig];
        }
        // add fixed-dimension offsets
        for (size_t k = 0; k < t->ndim; k++) {
            if (orig_to_out[k] == -1) {
                size_t pos = (nends[k] >= nstarts[k]) ? (size_t)(nstarts[k] - 1) : 0;
                src_offset += pos * t->strides[k];
            }
        }
        ot->data[out_idx] = value_copy(t->data[src_offset]);
    }

    free(new_shape);
    free(nstarts);
    free(nends);
    free(orig_to_out);
    return out;
}

// Map implementation
Value value_map_new(void) {
    Value v; v.type = VAL_MAP;
    Map* m = malloc(sizeof(Map));
    if (!m) { fprintf(stderr, "Out of memory\n"); exit(1); }
    m->items = NULL;
    m->count = 0;
    m->capacity = 0;
    m->refcount = 1;
    v.as.map = m;
    return v;
}

static int map_find_index(Map* m, Value key) {
    for (size_t i = 0; i < m->count; i++) {
        MapEntry* e = &m->items[i];
        if (e->key.type == key.type) {
            if (key.type == VAL_INT && e->key.as.i == key.as.i) return (int)i;
            if (key.type == VAL_STR && e->key.as.s && key.as.s && strcmp(e->key.as.s, key.as.s) == 0) return (int)i;
            if (key.type == VAL_FLT && e->key.as.f == key.as.f) return (int)i;
        }
    }
    return -1;
}

void value_map_set(Value* mapval, Value key, Value val) {
    if (!mapval || mapval->type != VAL_MAP) return;
    Map* m = mapval->as.map;
    int idx = map_find_index(m, key);
    if (idx >= 0) {
        // replace
        value_free(m->items[idx].value);
        m->items[idx].value = value_copy(val);
        return;
    }
    if (m->count + 1 > m->capacity) {
        size_t newc = m->capacity == 0 ? 8 : m->capacity * 2;
        m->items = realloc(m->items, sizeof(MapEntry) * newc);
        if (!m->items) { fprintf(stderr, "Out of memory\n"); exit(1); }
        m->capacity = newc;
    }
    m->items[m->count].key = value_copy(key);
    m->items[m->count].value = value_copy(val);
    m->count++;
}

Value value_map_get(Value mapval, Value key, int* found) {
    Value out = value_null();
    if (!mapval.as.map) { if (found) *found = 0; return out; }
    Map* m = mapval.as.map;
    int idx = map_find_index(m, key);
    if (idx < 0) { if (found) *found = 0; return out; }
    if (found) *found = 1;
    return value_copy(m->items[idx].value);
}

void value_map_delete(Value* mapval, Value key) {
    if (!mapval || mapval->type != VAL_MAP) return;
    Map* m = mapval->as.map;
    int idx = map_find_index(m, key);
    if (idx < 0) return;
    value_free(m->items[idx].key);
    value_free(m->items[idx].value);
    // compact
    for (size_t i = (size_t)idx; i + 1 < m->count; i++) m->items[i] = m->items[i+1];
    m->count--;
}

// Shallow copy semantics: increment refcount for MAP/TNS and return aliasing Value.
Value value_copy(Value v) {
    Value out = v;
    if (v.type == VAL_STR && v.as.s) {
        // Duplicate strings to preserve independent ownership semantics for STR
        out.as.s = strdup(v.as.s);
    } else if (v.type == VAL_TNS && v.as.tns) {
        Tensor* t = v.as.tns;
        t->refcount++;
        out.as.tns = t;
    } else if (v.type == VAL_MAP && v.as.map) {
        Map* m = v.as.map;
        m->refcount++;
        out.as.map = m;
    }
    return out;
}

// Deep-copy helper: recursively duplicate container contents.
Value value_deep_copy(Value v) {
    Value out = v;
    if (v.type == VAL_STR && v.as.s) {
        out.as.s = strdup(v.as.s);
    } else if (v.type == VAL_TNS && v.as.tns) {
        Tensor* t = v.as.tns;
        Tensor* t2 = malloc(sizeof(Tensor));
        if (!t2) { fprintf(stderr, "Out of memory\n"); exit(1); }
        t2->elem_type = t->elem_type;
        t2->ndim = t->ndim;
        t2->shape = malloc(sizeof(size_t) * t2->ndim);
        t2->strides = malloc(sizeof(size_t) * t2->ndim);
        for (size_t i = 0; i < t2->ndim; i++) { t2->shape[i] = t->shape[i]; t2->strides[i] = t->strides[i]; }
        t2->length = t->length;
        t2->data = malloc(sizeof(Value) * t2->length);
        for (size_t i = 0; i < t2->length; i++) t2->data[i] = value_deep_copy(t->data[i]);
        t2->refcount = 1;
        out.as.tns = t2;
    } else if (v.type == VAL_MAP && v.as.map) {
        Map* m = v.as.map;
        Map* m2 = malloc(sizeof(Map));
        if (!m2) { fprintf(stderr, "Out of memory\n"); exit(1); }
        m2->count = m->count;
        m2->capacity = m->count;
        m2->items = malloc(sizeof(MapEntry) * (m2->capacity ? m2->capacity : 1));
        for (size_t i = 0; i < m->count; i++) {
            m2->items[i].key = value_deep_copy(m->items[i].key);
            m2->items[i].value = value_deep_copy(m->items[i].value);
        }
        m2->refcount = 1;
        out.as.map = m2;
    }
    return out;
}

void value_free(Value v) {
    if (v.type == VAL_STR) {
        free(v.as.s);
    } else if (v.type == VAL_TNS && v.as.tns) {
        Tensor* t = v.as.tns;
        if (--t->refcount <= 0) {
            if (t->data) {
                for (size_t i = 0; i < t->length; i++) value_free(t->data[i]);
                free(t->data);
            }
            if (t->shape) free(t->shape);
            if (t->strides) free(t->strides);
            free(t);
        }
    } else if (v.type == VAL_MAP && v.as.map) {
        Map* m = v.as.map;
        if (--m->refcount <= 0) {
            if (m->items) {
                for (size_t i = 0; i < m->count; i++) {
                    value_free(m->items[i].key);
                    value_free(m->items[i].value);
                }
                free(m->items);
            }
            free(m);
        }
    }
}

const char* value_type_name(Value v) {
    switch (v.type) {
        case VAL_INT: return "INT";
        case VAL_FLT: return "FLT";
        case VAL_MAP: return "MAP";
        case VAL_TNS: return "TNS";
        case VAL_STR: return "STR";
        case VAL_FUNC: return "FUNC";
        default: return "NULL";
    }
}
