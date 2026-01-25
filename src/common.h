#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Common definitions for Prefix-C

typedef enum {
    PREFIX_SUCCESS = 0,
    PREFIX_ERROR_MEMORY = 1,
    PREFIX_ERROR_IO = 2,
    PREFIX_ERROR_SYNTAX = 3,
    PREFIX_ERROR_RUNTIME = 4
} PrefixResult;

#endif // COMMON_H
