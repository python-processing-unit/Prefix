#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifndef PREFIX_HAVE_PORTABLE_STRDUP
/* Portable strdup fallback implemented as a static inline so all
 * translation units include a safe implementation without relying on
 * platform-specific libc extensions. We then map `strdup` to this
 * implementation when no macro is present. */
static inline char* prefix_portable_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* r = (char*)malloc(n);
    if (!r) return NULL;
    memcpy(r, s, n);
    return r;
}
#ifndef strdup
#define strdup prefix_portable_strdup
#endif
#define PREFIX_HAVE_PORTABLE_STRDUP 1
#endif

static inline int prefix_stricmp(const char* lhs, const char* rhs) {
    unsigned char left;
    unsigned char right;

    if (!lhs) lhs = "";
    if (!rhs) rhs = "";

    for (;;) {
        left = (unsigned char)tolower((unsigned char)*lhs++);
        right = (unsigned char)tolower((unsigned char)*rhs++);
        if (left != right || left == '\0') {
            return (int)left - (int)right;
        }
    }
}

static inline char* prefix_getcwd(char* buffer, size_t size) {
#ifdef _WIN32
    return _getcwd(buffer, (int)size);
#else
    return getcwd(buffer, size);
#endif
}

static inline int prefix_chdir(const char* path) {
#ifdef _WIN32
    return _chdir(path);
#else
    return chdir(path);
#endif
}

static inline char* prefix_fullpath_dup(const char* path) {
    if (!path || path[0] == '\0') return NULL;

#ifdef _WIN32
    char full[_MAX_PATH];
    if (_fullpath(full, path, _MAX_PATH)) {
        return prefix_portable_strdup(full);
    }
#else
    char full[PATH_MAX];
    if (realpath(path, full)) {
        return prefix_portable_strdup(full);
    }
#endif

    return prefix_portable_strdup(path);
}

// Common definitions for Prefix-C

typedef enum {
    PREFIX_SUCCESS = 0,
    PREFIX_ERROR_MEMORY = 1,
    PREFIX_ERROR_IO = 2,
    PREFIX_ERROR_SYNTAX = 3,
    PREFIX_ERROR_RUNTIME = 4
} PrefixResult;

#endif // COMMON_H
