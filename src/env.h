#ifndef ENV_H
#define ENV_H

#include "value.h"

typedef struct {
    char* name;
    DeclType decl_type;
    Value value;
    bool initialized;
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

#endif // ENV_H