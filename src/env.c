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

/* Forward declarations for local helpers used before their definitions. */
static EnvEntry* env_find_local(Env* env, const char* name);

/* ================================================================== */
/*  Thread-local snapshots for env_get_entry                           */
/* ================================================================== */

// env_get_entry historically returned a raw pointer into Env storage.
// With the namespace buffer active, that pointer could be invalidated
// immediately after returning (e.g. via realloc/value_free on the
// prepare thread), causing use-after-free crashes.
//
// Fix: return a per-thread snapshot copy of the entry.
//
// NOTE: The returned pointer is valid until the calling thread makes
// enough subsequent env_get_entry calls to wrap this ring buffer.

#ifndef ENV_ENTRY_SNAPSHOT_RING
#define ENV_ENTRY_SNAPSHOT_RING 32
#endif

static _Thread_local EnvEntry g_entry_snaps[ENV_ENTRY_SNAPSHOT_RING];
static _Thread_local size_t g_entry_snap_idx = 0;

static void env_entry_snap_clear(EnvEntry* e) {
    if (!e) return;
    if (e->name) {
        free(e->name);
        e->name = NULL;
    }
    if (e->alias_target) {
        free(e->alias_target);
        e->alias_target = NULL;
    }
    e->alias_target_env = NULL;
    value_free(e->value);
    e->value = value_null();
    e->decl_type = TYPE_UNKNOWN;
    e->initialized = false;
    e->frozen = false;
    e->permafrozen = false;
}

static EnvEntry* env_entry_snap_alloc(void) {
    EnvEntry* slot = &g_entry_snaps[g_entry_snap_idx++ % ENV_ENTRY_SNAPSHOT_RING];
    env_entry_snap_clear(slot);
    return slot;
}

static void env_entry_snap_from_raw(EnvEntry* dst, const EnvEntry* src) {
    if (!dst) return;
    if (!src) {
        // leave dst cleared
        return;
    }

    dst->name = src->name ? strdup(src->name) : NULL;
    dst->decl_type = src->decl_type;
    dst->initialized = src->initialized;
    dst->frozen = src->frozen;
    dst->permafrozen = src->permafrozen;
    dst->alias_target = src->alias_target ? strdup(src->alias_target) : NULL;
    dst->alias_target_env = src->alias_target_env;
    dst->value = value_copy(src->value);
}

// Helper: like env_get_entry_raw but also returns the Env* where the
// entry was found via out_env (if non-NULL).
static EnvEntry* env_get_entry_raw_with_env(Env* env, const char* name, Env** out_env) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            if (out_env) *out_env = e;
            return entry;
        }
    }
    return NULL;
}

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
    env->refcount = 1;
    return env;
}

void env_retain(Env* env) {
    if (!env) return;
    env->refcount++;
}

void env_free(Env* env) {
    if (!env) return;
    if (--env->refcount > 0) return;
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
    return env_get_entry_raw_with_env(env, name, NULL);
}

static bool env_get_raw(Env* env, const char* name,
                        Value* out_value, DeclType* out_type,
                        bool* out_initialized) {
    for (Env* e = env; e != NULL; e = e->parent) {
        EnvEntry* entry = env_find_local(e, name);
        if (entry) {
            /* Follow alias chain to the final target */
            EnvEntry* cur = entry;
            Env* cur_env = e;
            int depth = 0;
            while (cur && cur->alias_target) {
                if (depth++ > 256) return false; /* cycle or too deep */
                Env* lookup_env = cur->alias_target_env ? cur->alias_target_env : cur_env;
                cur = env_get_entry_raw_with_env(lookup_env, cur->alias_target, &cur_env);
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

static DeclType env_decl_type_from_value(Value value) {
    switch (value.type) {
        case VAL_BOOL: return TYPE_BOOL;
        case VAL_INT: return TYPE_INT;
        case VAL_FLT: return TYPE_FLT;
        case VAL_STR: return TYPE_STR;
        case VAL_TNS: return TYPE_TNS;
        case VAL_MAP: return TYPE_MAP;
        case VAL_FUNC: return TYPE_FUNC;
        case VAL_THR: return TYPE_THR;
        default:      return TYPE_UNKNOWN;
    }
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
    entry->alias_target_env = NULL;
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
                Env* lookup_env = entry->alias_target_env ? entry->alias_target_env : e;
                Env* target_env_found = NULL;
                EnvEntry* target = env_get_entry_raw_with_env(lookup_env, target_name, &target_env_found);
                if (!target) return false;
                if (type != TYPE_UNKNOWN && target->decl_type != type) return false;
                DeclType actual_type = env_decl_type_from_value(value);
                if (target->decl_type != TYPE_UNKNOWN && actual_type != TYPE_UNKNOWN &&
                    target->decl_type != actual_type) {
                    return false;
                }
                if (target->frozen || target->permafrozen) return false;
                if (target->initialized) value_free(target->value);
                target->value = value_copy(value);
                target->initialized = true;
                return true;
            }

            /* Respect frozen / permafrozen bindings */
            if (entry->frozen || entry->permafrozen) return false;

            if (type != TYPE_UNKNOWN && entry->decl_type != type) return false;

            DeclType actual_type = env_decl_type_from_value(value);
            if (entry->decl_type != TYPE_UNKNOWN && actual_type != TYPE_UNKNOWN &&
                entry->decl_type != actual_type) {
                return false;
            }

            if (entry->initialized) value_free(entry->value);
            entry->value = value_copy(value);
            entry->initialized = true;
            return true;
        }
    }
    if (!declare_if_missing) return false;
    if (type == TYPE_UNKNOWN) return false;
    DeclType actual_type = env_decl_type_from_value(value);
    if (actual_type != TYPE_UNKNOWN && actual_type != type) {
        return false;
    }
    if (!env_define_direct(env, name, type)) return false;
    EnvEntry* entry = env_find_local(env, name);
    if (!entry) return false;
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
                entry->alias_target_env = NULL;
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

    /* Ensure the target exists; capture the Env where it was found */
    Env* target_env_found = NULL;
    EnvEntry* target = env_get_entry_raw_with_env(env, target_name, &target_env_found);
    if (!target) return false;

    /* Resolve final target through alias chain; detect cycles */
    EnvEntry* cur = target;
    Env* cur_env = target_env_found;
    int depth = 0;
    while (cur && cur->alias_target) {
        if (depth++ > 256) return false;
        if (strcmp(cur->alias_target, name) == 0) return false; /* cycle */
        Env* lookup_env = cur->alias_target_env ? cur->alias_target_env : cur_env;
        cur = env_get_entry_raw_with_env(lookup_env, cur->alias_target, &cur_env);
    }
    if (!cur) return false;

    /* Disallow aliasing to frozen / permafrozen target */
    if (cur->frozen || cur->permafrozen) return false;

    /* Type compatibility */
    if (type != TYPE_UNKNOWN && type != cur->decl_type) return false;

    /* Find existing local entry (but don't create it yet). Only create
       the local entry after all validation succeeds to avoid leaving a
       declared-but-uninitialized binding when alias setup fails. */
    EnvEntry* entry = env_find_local(env, name);
    if (!entry) {
        if (!declare_if_missing) return false;
        /* create now */
        if (!env_define_direct(env, name, type)) return false;
        entry = env_find_local(env, name);
        if (!entry) return false;
    }

    /* Respect frozen state on the entry itself */
    if (entry->frozen || entry->permafrozen) return false;

    /* Overwrite declared type with target's type */
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
    /* Record the Env where the final target was located so alias
       resolution works even when target lives in a different Env tree. */
    entry->alias_target_env = cur_env;
    entry->initialized = true; /* alias is considered initialised */
    return true;
}

bool env_set_alias_cross(Env* env, const char* name, Env* target_env, const char* target_name, DeclType type, bool declare_if_missing) {
    if (!env || !name || !target_env || !target_name) return false;

    /* Ensure the target exists; capture the Env where it was found */
    Env* target_env_found = NULL;
    EnvEntry* target = env_get_entry_raw_with_env(target_env, target_name, &target_env_found);
    if (!target) return false;

    /* Resolve final target through alias chain; detect cycles */
    EnvEntry* cur = target;
    Env* cur_env = target_env_found;
    int depth = 0;
    while (cur && cur->alias_target) {
        if (depth++ > 256) return false;
        if (strcmp(cur->alias_target, name) == 0) return false; /* cycle */
        Env* lookup_env = cur->alias_target_env ? cur->alias_target_env : cur_env;
        cur = env_get_entry_raw_with_env(lookup_env, cur->alias_target, &cur_env);
    }
    if (!cur) return false;

    /* Disallow aliasing to frozen / permafrozen target */
    if (cur->frozen || cur->permafrozen) return false;

    /* Type compatibility */
    if (type != TYPE_UNKNOWN && type != cur->decl_type) return false;

    /* Find existing local entry (but don't create it yet). Only create
       the local entry after all validation succeeds to avoid leaving a
       declared-but-uninitialized binding when alias setup fails. */
    EnvEntry* entry = env_find_local(env, name);
    if (!entry) {
        if (!declare_if_missing) return false;
        /* create now */
        if (!env_define_direct(env, name, type)) return false;
        entry = env_find_local(env, name);
        if (!entry) return false;
    }

    /* Respect frozen state on the entry itself */
    if (entry->frozen || entry->permafrozen) return false;

    /* Overwrite declared type with target's type */
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
    entry->alias_target_env = cur_env;
    entry->initialized = true; /* alias is considered initialised */
    return true;
}

bool env_restore_local_direct(Env* env, const char* name, Value value, DeclType type, bool initialized) {
    if (!env || !name) return false;

    EnvEntry* entry = env_find_local(env, name);
    if (!entry) {
        if (!env_define_direct(env, name, type)) return false;
        entry = env_find_local(env, name);
        if (!entry) return false;
    }

    /* Respect frozen/permafrozen state */
    if (entry->frozen || entry->permafrozen) return false;

    /* Overwrite declared type */
    entry->decl_type = type;

    /* Remove any aliasing */
    if (entry->alias_target) {
        free(entry->alias_target);
        entry->alias_target = NULL;
        entry->alias_target_env = NULL;
    }

    /* Replace stored value if present */
    if (entry->initialized) {
        value_free(entry->value);
        entry->initialized = false;
        entry->value = value_null();
    }

    if (initialized) {
        entry->value = value_copy(value);
        entry->initialized = true;
    } else {
        entry->initialized = false;
        entry->value = value_null();
    }

    return true;
}

bool env_restore_local(Env* env, const char* name, Value value, DeclType type, bool initialized) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_restore_local(env, name, value, type, initialized);
    return env_restore_local_direct(env, name, value, type, initialized);
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
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_define(env, name, type);
    return env_define_direct(env, name, type);
}

bool env_assign(Env* env, const char* name, Value value,
                DeclType type, bool declare_if_missing) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_assign(env, name, value, type, declare_if_missing);
    return env_assign_direct(env, name, value, type, declare_if_missing);
}

bool env_delete(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_delete(env, name);
    return env_delete_direct(env, name);
}

bool env_set_alias(Env* env, const char* name, const char* target_name,
                   DeclType type, bool declare_if_missing) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_set_alias(env, name, target_name, type,
                                   declare_if_missing);
    return env_set_alias_direct(env, name, target_name, type,
                                declare_if_missing);
}

int env_freeze(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_freeze(env, name);
    return env_freeze_direct(env, name);
}

int env_thaw(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_thaw(env, name);
    return env_thaw_direct(env, name);
}

int env_permafreeze(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread())
        return ns_buffer_permafreeze(env, name);
    return env_permafreeze_direct(env, name);
}

/* ================================================================== */
/*  Public API – read operations                                       */
/*  Block until the queried symbol's pending writes are drained, then  */
/*  acquire the env-access lock for a safe read.                       */
/* ================================================================== */

EnvEntry* env_get_entry(Env* env, const char* name) {
    EnvEntry* snap = env_entry_snap_alloc();
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
        ns_buffer_read_lock(name);
        EnvEntry* entry = env_get_entry_raw(env, name);
        env_entry_snap_from_raw(snap, entry);
        ns_buffer_read_unlock();
        if (!entry) {
            env_entry_snap_clear(snap);
            return NULL;
        }
        return snap;
    }

    EnvEntry* entry = env_get_entry_raw(env, name);
    env_entry_snap_from_raw(snap, entry);
    if (!entry) {
        // Clear to a canonical empty state and return NULL for not-found.
        env_entry_snap_clear(snap);
        return NULL;
    }
    return snap;
}

bool env_get(Env* env, const char* name, Value* out_value,
             DeclType* out_type, bool* out_initialized) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
        ns_buffer_read_lock(name);
        bool r = env_get_raw(env, name, out_value, out_type, out_initialized);
        ns_buffer_read_unlock();
        return r;
    }
    return env_get_raw(env, name, out_value, out_type, out_initialized);
}

bool env_exists(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
        ns_buffer_read_lock(name);
        bool r = env_exists_raw(env, name);
        ns_buffer_read_unlock();
        return r;
    }
    return env_exists_raw(env, name);
}

int env_frozen_state(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
        ns_buffer_read_lock(name);
        int r = env_frozen_state_raw(env, name);
        ns_buffer_read_unlock();
        return r;
    }
    return env_frozen_state_raw(env, name);
}

int env_permafrozen(Env* env, const char* name) {
    if (ns_buffer_active() && !ns_buffer_is_prepare_thread()) {
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
