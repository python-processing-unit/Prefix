#ifndef COMMON_H
#define COMMON_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define prefix_stricmp _stricmp
#else
#define prefix_stricmp strcasecmp
#endif

static inline char *prefix_fullpath_dup(const char *path) {
    if (!path || path[0] == '\0') {
        return NULL;
    }

#ifdef _WIN32
    char full[_MAX_PATH];
    if (_fullpath(full, path, _MAX_PATH)) {
        return strdup(full);
    }
#else
    char full[PATH_MAX];
    if (realpath(path, full)) {
        return strdup(full);
    }
#endif

    return strdup(path);
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
