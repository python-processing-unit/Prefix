#ifndef ENV_H
#define ENV_H

#include "value.h"

typedef struct {
    char* name;
    DeclType decl_type;
    Value value;
    bool initialized;
    bool frozen;
    bool permafrozen;
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

#endif // ENV_H