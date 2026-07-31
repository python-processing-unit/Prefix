#ifndef ENV_H
#define ENV_H

#include "value.h"

/* Forward-declare Env so EnvEntry can hold an Env* pointer. */
typedef struct Env Env;

typedef struct EnvEntry {
    char *name;
    DeclType decl_type;
    int decl_base; // 0 = parent int/float, 2..64 = named base
    Value schema;  // NEW: map schema value, .type==VAL_NULL if none
    Value value;
    bool initialized;
    bool frozen;
    bool permafrozen;
    // If non-NULL, this entry is an alias to another binding name in the environment
    char *alias_target;
    // If non-NULL, indicates the Env where alias_target should be resolved.
    // This allows aliasing to bindings that live in a different Env tree
    // (for example, caller environments when binding pointer arguments).
    Env *alias_target_env;
} EnvEntry;

typedef struct Env {
    struct Env *parent;
    EnvEntry *entries;
    size_t count;
    size_t capacity;
    int refcount;
} Env;

Env *env_create(Env *parent);
// Kept to make extension development easier.
void env_retain(Env *env);
void env_free(Env *env);

bool env_define(Env *env, const char *name, DeclType type, int base, Value schema);
bool env_assign(Env *env, const char *name, Value value, DeclType type, int type_base, bool declare_if_missing);
bool env_get(Env *env, const char *name, Value *out_value, DeclType *out_type, int *out_base, bool *out_initialized);
bool env_delete(Env *env, const char *name);
// Kept to make extension development easier.
bool env_exists(Env *env, const char *name);
// Return a per-thread snapshot of the EnvEntry for the given name, searching parents.
// The snapshot is safe to read after the function returns even when the
// namespace buffer is active (it does not point into Env storage).
// Caller must NOT free the returned pointer.
// Returns NULL if not found.
EnvEntry *env_get_entry(Env *env, const char *name);

// Create or update an alias (pointer) binding: `name` will become an alias to `target_name`.
// If declare_if_missing is true, `name` will be defined if absent. Returns true on success.
bool env_set_alias(Env *env, const char *name, const char *target_name, DeclType type, int type_base,
                   bool declare_if_missing);
// Create or update an alias where the alias target should be resolved in a
// specific environment (target_env). This supports pointer args binding
// where the pointed-to symbol is visible in the caller's environment.
bool env_set_alias_cross(Env *env, const char *name, Env *target_env, const char *target_name, DeclType type,
                         int type_base, bool declare_if_missing);

// Symbol freezing API
// Returns 0 on success, -1 if the identifier was not found.
// Kept to make extension development easier.
int env_freeze(Env *env, const char *name);
// Returns 0 on success, -1 if not found, -2 if identifier is permanently frozen
// Kept to make extension development easier.
int env_thaw(Env *env, const char *name);
// Returns 0 on success, -1 if not found
// Kept to make extension development easier.
int env_permafreeze(Env *env, const char *name);
// Returns -1 if permanently frozen, 1 if frozen, 0 if not frozen or not found
// Kept to make extension development easier.
int env_frozen_state(Env *env, const char *name);
// Returns 1 if permanently frozen, 0 otherwise (or not found)
// Kept to make extension development easier.
int env_permafrozen(Env *env, const char *name);

// ---- Direct (unbuffered) entry points used by the namespace buffer ----
// These perform the actual work and must NOT be called from outside
// env.c / ns_buffer.c.  Public callers should use the non-_direct
// versions above, which route through the write buffer when active.

bool env_define_direct(Env *env, const char *name, DeclType type, int base, Value schema);
bool env_assign_direct(Env *env, const char *name, Value value, DeclType type, int type_base, bool declare_if_missing);
bool env_delete_direct(Env *env, const char *name);
bool env_set_alias_direct(Env *env, const char *name, const char *target_name, DeclType type, int type_base,
                          bool declare_if_missing);
int env_freeze_direct(Env *env, const char *name);
int env_thaw_direct(Env *env, const char *name);
int env_permafreeze_direct(Env *env, const char *name);

// Restore or overwrite a local entry's declared type and value. This will
// update the local entry (creating it if absent) to have `type` and, if
// `initialized` is true, set its value to a copy of `value` and mark it
// initialized; otherwise leave it uninitialized. Returns true on success.
bool env_restore_local(Env *env, const char *name, Value value, DeclType type, int type_base, bool initialized);
bool env_restore_local_direct(Env *env, const char *name, Value value, DeclType type, int type_base, bool initialized);

#endif // ENV_H