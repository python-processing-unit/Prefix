#include "env.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

static void* env_alloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memset(ptr, 0, size);
    return ptr;
}

Env* env_create(Env* parent) {
    Env* env = env_alloc(sizeof(Env));
    env->parent = parent;
    return env;
}

void env_free(Env* env) {
    if (!env) return;
    for (size_t i = 0; i < env->count; i++) {
        free(env->entries[i].name);
        if (env->entries[i].initialized) {
            value_free(env->entries[i].value);
        }
    }
    free(env->entries);
    free(env);
}

static EnvEntry* env_find_local(Env* env, const char* name) {
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            return &env->entries[i];
        }
    }
    return NULL;
}

EnvEntry* env_get_entry(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) return entry;
    }
    return NULL;
}

bool env_define(Env* env, const char* name, DeclType type) {
    if (env_find_local(env, name) != NULL) return false;
    if (env->count + 1 > env->capacity) {
        size_t new_cap = env->capacity == 0 ? 8 : env->capacity * 2;
        env->entries = realloc(env->entries, new_cap * sizeof(EnvEntry));
        if (!env->entries) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        env->capacity = new_cap;
    }
    EnvEntry* entry = &env->entries[env->count++];
    entry->name = strdup(name);
    entry->decl_type = type;
    entry->initialized = false;
    entry->value = value_null();
    return true;
}

bool env_assign(Env* env, const char* name, Value value, DeclType type, bool declare_if_missing) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            if (entry->initialized) {
                value_free(entry->value);
            }
            entry->value = value_copy(value);
            entry->initialized = true;
            return true;
        }
    }
    if (!declare_if_missing) return false;
    env_define(env, name, type);
    EnvEntry* entry = env_find_local(env, name);
    entry->value = value_copy(value);
    entry->initialized = true;
    return true;
}

bool env_get(Env* env, const char* name, Value* out_value, DeclType* out_type, bool* out_initialized) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            if (out_value) *out_value = value_copy(entry->value);
            if (out_type) *out_type = entry->decl_type;
            if (out_initialized) *out_initialized = entry->initialized;
            return true;
        }
    }
    return false;
}

bool env_delete(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            if (entry->initialized) {
                value_free(entry->value);
            }
            entry->initialized = false;
            entry->value = value_null();
            return true;
        }
    }
    return false;
}

bool env_exists(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry && entry->initialized) return true;
    }
    return false;
}