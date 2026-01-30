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

// Forward declaration for lookup helper used below
static EnvEntry* env_find_local(Env* env, const char* name);

bool env_set_alias(Env* env, const char* name, const char* target_name, DeclType type, bool declare_if_missing) {
    if (!env || !name || !target_name) return false;
    // Ensure the target exists
    EnvEntry* target = env_get_entry(env, target_name);
    if (!target) return false;

    // Resolve final target through alias chain and ensure no cycle to 'name'
    EnvEntry* cur = target;
    int depth = 0;
    while (cur && cur->alias_target) {
        if (depth++ > 256) return false;
        if (strcmp(cur->alias_target, name) == 0) return false; // would create cycle
        cur = env_get_entry(env, cur->alias_target);
    }
    if (!cur) return false;

    // Disallow aliasing to frozen/permafrozen target
    if (cur->frozen || cur->permafrozen) return false;

    // Find or create local entry
    EnvEntry* entry = env_find_local(env, name);
    if (!entry) {
        if (!declare_if_missing) return false;
        if (!env_define(env, name, type)) return false;
        entry = env_find_local(env, name);
    }

    // Respect frozen state on the entry itself
    if (entry->frozen || entry->permafrozen) return false;

    // Type compatibility
    if (type != TYPE_UNKNOWN && type != cur->decl_type) return false;
    entry->decl_type = cur->decl_type;

    // Clear any stored value and set alias
    if (entry->initialized) {
        value_free(entry->value);
        entry->initialized = false;
        entry->value = value_null();
    }
    if (entry->alias_target) { free(entry->alias_target); entry->alias_target = NULL; }
    entry->alias_target = strdup(cur->name);
    entry->initialized = true; // alias is considered an initialized binding
    return true;
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
    entry->frozen = false;
    entry->permafrozen = false;
    entry->alias_target = NULL;
    entry->value = value_null();
    return true;
}

bool env_assign(Env* env, const char* name, Value value, DeclType type, bool declare_if_missing) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            // If this binding is an alias, route assignment to the alias target
            if (entry->alias_target) {
                const char* target_name = entry->alias_target;
                if (!target_name) return false;
                EnvEntry* target = env_get_entry(env, target_name);
                if (!target) return false;
                // Respect freezing on the target
                if (target->frozen || target->permafrozen) return false;
                if (target->initialized) value_free(target->value);
                target->value = value_copy(value);
                target->initialized = true;
                return true;
            }

            // Respect frozen/permanent-frozen bindings for normal assignments
            if (entry->frozen || entry->permafrozen) {
                return false;
            }

            // Normal assignment: respect frozen/permafrozen bindings
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
            // If this entry is an alias, follow chain to final target
            EnvEntry* cur = entry;
            int depth = 0;
            while (cur && cur->alias_target) {
                if (depth++ > 256) return false; // cycle or too deep
                cur = env_get_entry(env, cur->alias_target);
            }
            if (!cur) return false;
            if (out_value) *out_value = value_copy(cur->value);
            if (out_type) *out_type = cur->decl_type;
            if (out_initialized) *out_initialized = cur->initialized;
            return true;
        }
    }
    return false;
}

bool env_delete(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            // Respect frozen/permanent-frozen bindings
            if (entry->frozen || entry->permafrozen) {
                return false;
            }
            if (entry->initialized) {
                value_free(entry->value);
            }
            if (entry->alias_target) { free(entry->alias_target); entry->alias_target = NULL; }
            entry->initialized = false;
            entry->value = value_null();
            return true;
        }
    }
    return false;
}

int env_freeze(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry) return -1;
    entry->frozen = true;
    return 0;
}

int env_thaw(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry) return -1;
    if (entry->permafrozen) return -2;
    entry->frozen = false;
    return 0;
}

int env_permafreeze(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry) return -1;
    entry->permafrozen = true;
    entry->frozen = true;
    return 0;
}

int env_frozen_state(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry) return 0;
    if (entry->permafrozen) return -1;
    if (entry->frozen) return 1;
    return 0;
}

int env_permafrozen(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry(env, name);
    if (!entry) return 0;
    return entry->permafrozen ? 1 : 0;
}

// EnvEntry accessors
bool env_entry_initialized(EnvEntry* entry) {
    if (!entry) return false;
    return entry->initialized;
}

Value env_entry_value_copy(EnvEntry* entry) {
    if (!entry) return value_null();
    return value_copy(entry->value);
}

int env_entry_frozen_state_local(EnvEntry* entry) {
    if (!entry) return 0;
    if (entry->permafrozen) return -1;
    if (entry->frozen) return 1;
    return 0;
}

bool env_exists(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry && entry->initialized) return true;
    }
    return false;
}

// Forward declaration for local lookup helper (defined later)
static EnvEntry* env_find_local(Env* env, const char* name);