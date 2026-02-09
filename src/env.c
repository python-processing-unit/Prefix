/*
 * env.c – Variable environment with namespace write-buffer integration.
 *
 * The "direct" (_direct) functions perform the actual work and are called
 * either by the prepare thread (via ns_buffer) or when the buffer is
 * inactive.  The public env_* functions route through the buffer when it
 * is active, falling back to the _direct path otherwise.
 *
 * Internal read helpers (_raw) never touch the buffer and are safe to
 * call from within _direct functions (which execute on the prepare
 * thread while it already holds the env-access lock).
 */

#include "env.h"
#include "ns_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static void* env_alloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    memset(ptr, 0, size);
    return ptr;
}

/* ================================================================== */
/*  Lifecycle (not buffered)                                           */
/* ================================================================== */

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
        if (env->entries[i].alias_target) {
            free(env->entries[i].alias_target);
        }
    }
    free(env->entries);
    free(env);
}

/* ================================================================== */
/*  Raw internal lookup helpers (no buffer interaction)                */
/* ================================================================== */

static EnvEntry* env_find_local(Env* env, const char* name) {
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->entries[i].name, name) == 0) {
            return &env->entries[i];
        }
    }
    return NULL;
}

static EnvEntry* env_get_entry_raw(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) return entry;
    }
    return NULL;
}

static bool env_get_raw(Env* env, const char* name,
                        Value* out_value, DeclType* out_type,
                        bool* out_initialized) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            /* Follow alias chain to the final target */
            EnvEntry* cur = entry;
            int depth = 0;
            while (cur && cur->alias_target) {
                if (depth++ > 256) return false; /* cycle or too deep */
                cur = env_get_entry_raw(env, cur->alias_target);
            }
            if (!cur) return false;
            if (out_value)       *out_value = value_copy(cur->value);
            if (out_type)        *out_type  = cur->decl_type;
            if (out_initialized) *out_initialized = cur->initialized;
            return true;
        }
    }
    return false;
}

static bool env_exists_raw(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry && entry->initialized) return true;
    }
    return false;
}

static int env_frozen_state_raw(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry_raw(env, name);
    if (!entry) return 0;
    if (entry->permafrozen) return -1;
    if (entry->frozen) return 1;
    return 0;
}

static int env_permafrozen_raw(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry_raw(env, name);
    if (!entry) return 0;
    return entry->permafrozen ? 1 : 0;
}

/* ================================================================== */
/*  Direct (unbuffered) write implementations                          */
/*  Called by the prepare thread or when the buffer is inactive.       */
/* ================================================================== */

bool env_define_direct(Env* env, const char* name, DeclType type) {
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

bool env_assign_direct(Env* env, const char* name, Value value,
                       DeclType type, bool declare_if_missing) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            /* Route through alias chain */
            if (entry->alias_target) {
                const char* target_name = entry->alias_target;
                EnvEntry* target = env_get_entry_raw(env, target_name);
                if (!target) return false;
                if (target->frozen || target->permafrozen) return false;
                if (target->initialized) value_free(target->value);
                target->value = value_copy(value);
                target->initialized = true;
                return true;
            }

            /* Respect frozen / permafrozen bindings */
            if (entry->frozen || entry->permafrozen) return false;

            if (entry->initialized) value_free(entry->value);
            entry->value = value_copy(value);
            entry->initialized = true;
            return true;
        }
    }
    if (!declare_if_missing) return false;
    env_define_direct(env, name, type);
    EnvEntry* entry = env_find_local(env, name);
    entry->value = value_copy(value);
    entry->initialized = true;
    return true;
}

bool env_delete_direct(Env* env, const char* name) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            if (entry->frozen || entry->permafrozen) return false;
            if (entry->initialized) value_free(entry->value);
            if (entry->alias_target) {
                free(entry->alias_target);
                entry->alias_target = NULL;
            }
            entry->initialized = false;
            entry->value = value_null();
            return true;
        }
    }
    return false;
}

bool env_set_alias_direct(Env* env, const char* name,
                          const char* target_name, DeclType type,
                          bool declare_if_missing) {
    if (!env || !name || !target_name) return false;

    /* Ensure the target exists */
    EnvEntry* target = env_get_entry_raw(env, target_name);
    if (!target) return false;

    /* Resolve final target through alias chain; detect cycles */
    EnvEntry* cur = target;
    int depth = 0;
    while (cur && cur->alias_target) {
        if (depth++ > 256) return false;
        if (strcmp(cur->alias_target, name) == 0) return false; /* cycle */
        cur = env_get_entry_raw(env, cur->alias_target);
    }
    if (!cur) return false;

    /* Disallow aliasing to frozen / permafrozen target */
    if (cur->frozen || cur->permafrozen) return false;

    /* Find or create local entry */
    EnvEntry* entry = env_find_local(env, name);
    if (!entry) {
        if (!declare_if_missing) return false;
        if (!env_define_direct(env, name, type)) return false;
        entry = env_find_local(env, name);
    }

    /* Respect frozen state on the entry itself */
    if (entry->frozen || entry->permafrozen) return false;

    /* Type compatibility */
    if (type != TYPE_UNKNOWN && type != cur->decl_type) return false;
    entry->decl_type = cur->decl_type;

    /* Clear any stored value and set alias */
    if (entry->initialized) {
        value_free(entry->value);
        entry->initialized = false;
        entry->value = value_null();
    }
    if (entry->alias_target) {
        free(entry->alias_target);
        entry->alias_target = NULL;
    }
    entry->alias_target = strdup(cur->name);
    entry->initialized = true; /* alias is considered initialised */
    return true;
}

int env_freeze_direct(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry_raw(env, name);
    if (!entry) return -1;
    entry->frozen = true;
    return 0;
}

int env_thaw_direct(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry_raw(env, name);
    if (!entry) return -1;
    if (entry->permafrozen) return -2;
    entry->frozen = false;
    return 0;
}

int env_permafreeze_direct(Env* env, const char* name) {
    EnvEntry* entry = env_get_entry_raw(env, name);
    if (!entry) return -1;
    entry->permafrozen = true;
    entry->frozen = true;
    return 0;
}

/* ================================================================== */
/*  Public API – write operations                                      */
/*  Route through the namespace buffer when active; otherwise direct.  */
/* ================================================================== */

bool env_define(Env* env, const char* name, DeclType type) {
    if (ns_buffer_active())
        return ns_buffer_define(env, name, type);
    return env_define_direct(env, name, type);
}

bool env_assign(Env* env, const char* name, Value value,
                DeclType type, bool declare_if_missing) {
    if (ns_buffer_active())
        return ns_buffer_assign(env, name, value, type, declare_if_missing);
    return env_assign_direct(env, name, value, type, declare_if_missing);
}

bool env_delete(Env* env, const char* name) {
    if (ns_buffer_active())
        return ns_buffer_delete(env, name);
    return env_delete_direct(env, name);
}

bool env_set_alias(Env* env, const char* name, const char* target_name,
                   DeclType type, bool declare_if_missing) {
    if (ns_buffer_active())
        return ns_buffer_set_alias(env, name, target_name, type,
                                   declare_if_missing);
    return env_set_alias_direct(env, name, target_name, type,
                                declare_if_missing);
}

int env_freeze(Env* env, const char* name) {
    if (ns_buffer_active())
        return ns_buffer_freeze(env, name);
    return env_freeze_direct(env, name);
}

int env_thaw(Env* env, const char* name) {
    if (ns_buffer_active())
        return ns_buffer_thaw(env, name);
    return env_thaw_direct(env, name);
}

int env_permafreeze(Env* env, const char* name) {
    if (ns_buffer_active())
        return ns_buffer_permafreeze(env, name);
    return env_permafreeze_direct(env, name);
}

/* ================================================================== */
/*  Public API – read operations                                       */
/*  Block until the queried symbol's pending writes are drained, then  */
/*  acquire the env-access lock for a safe read.                       */
/* ================================================================== */

EnvEntry* env_get_entry(Env* env, const char* name) {
    if (ns_buffer_active()) {
        ns_buffer_read_lock(name);
        EnvEntry* entry = env_get_entry_raw(env, name);
        ns_buffer_read_unlock();
        return entry;
    }
    return env_get_entry_raw(env, name);
}

bool env_get(Env* env, const char* name, Value* out_value,
             DeclType* out_type, bool* out_initialized) {
    if (ns_buffer_active()) {
        ns_buffer_read_lock(name);
        bool r = env_get_raw(env, name, out_value, out_type, out_initialized);
        ns_buffer_read_unlock();
        return r;
    }
    return env_get_raw(env, name, out_value, out_type, out_initialized);
}

bool env_exists(Env* env, const char* name) {
    if (ns_buffer_active()) {
        ns_buffer_read_lock(name);
        bool r = env_exists_raw(env, name);
        ns_buffer_read_unlock();
        return r;
    }
    return env_exists_raw(env, name);
}

int env_frozen_state(Env* env, const char* name) {
    if (ns_buffer_active()) {
        ns_buffer_read_lock(name);
        int r = env_frozen_state_raw(env, name);
        ns_buffer_read_unlock();
        return r;
    }
    return env_frozen_state_raw(env, name);
}

int env_permafrozen(Env* env, const char* name) {
    if (ns_buffer_active()) {
        ns_buffer_read_lock(name);
        int r = env_permafrozen_raw(env, name);
        ns_buffer_read_unlock();
        return r;
    }
    return env_permafrozen_raw(env, name);
}

/* ================================================================== */
/*  EnvEntry accessors (operate on already-obtained pointers)          */
/* ================================================================== */

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