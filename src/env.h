#ifndef ENV_H
#define ENV_H

#include "value.h"

typedef struct EnvEntry {
    char* name;
    DeclType decl_type;
    Value value;
    bool initialized;
    bool frozen;
    bool permafrozen;
    // If non-NULL, this entry is an alias to another binding name in the environment
    char* alias_target;
} EnvEntry;

typedef struct Env {
    struct Env* parent;
    EnvEntry* entries;
    size_t count;
    size_t capacity;
} Env;

Env* env_create(Env* parent);
void env_free(Env* env);

bool env_define(Env* env, const char* name, DeclType type);
bool env_assign(Env* env, const char* name, Value value, DeclType type, bool declare_if_missing);
bool env_get(Env* env, const char* name, Value* out_value, DeclType* out_type, bool* out_initialized);
bool env_delete(Env* env, const char* name);
bool env_exists(Env* env, const char* name);
// Return pointer to the EnvEntry for the given name, searching parents.
// Caller must NOT free the returned pointer. Returns NULL if not found.
EnvEntry* env_get_entry(Env* env, const char* name);

// Create or update an alias (pointer) binding: `name` will become an alias to `target_name`.
// If declare_if_missing is true, `name` will be defined if absent. Returns true on success.
bool env_set_alias(Env* env, const char* name, const char* target_name, DeclType type, bool declare_if_missing);

// Accessors for EnvEntry opaque use from other translation units
// Returns true if the entry is initialized
bool env_entry_initialized(EnvEntry* entry);
// Returns a copy of the entry's value (caller owns the returned Value)
Value env_entry_value_copy(EnvEntry* entry);
// Returns frozen state: -1 permafrozen, 1 frozen, 0 not frozen or not found
int env_entry_frozen_state_local(EnvEntry* entry);

// Symbol freezing API
// Returns 0 on success, -1 if the identifier was not found.
int env_freeze(Env* env, const char* name);
// Returns 0 on success, -1 if not found, -2 if identifier is permanently frozen
int env_thaw(Env* env, const char* name);
// Returns 0 on success, -1 if not found
int env_permafreeze(Env* env, const char* name);
// Returns -1 if permanently frozen, 1 if frozen, 0 if not frozen or not found
int env_frozen_state(Env* env, const char* name);
// Returns 1 if permanently frozen, 0 otherwise (or not found)
int env_permafrozen(Env* env, const char* name);

// ---- Direct (unbuffered) entry points used by the namespace buffer ----
// These perform the actual work and must NOT be called from outside
// env.c / ns_buffer.c.  Public callers should use the non-_direct
// versions above, which route through the write buffer when active.

bool env_define_direct(Env* env, const char* name, DeclType type);
bool env_assign_direct(Env* env, const char* name, Value value, DeclType type, bool declare_if_missing);
bool env_delete_direct(Env* env, const char* name);
bool env_set_alias_direct(Env* env, const char* name, const char* target_name, DeclType type, bool declare_if_missing);
int  env_freeze_direct(Env* env, const char* name);
int  env_thaw_direct(Env* env, const char* name);
int  env_permafreeze_direct(Env* env, const char* name);

#endif // ENV_H