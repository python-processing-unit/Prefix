#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "token.h"

int main(int argc, char** argv) {
    const char* path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        // default to repository test file
        path = "../test.pre";
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open '%s'\n", path);
        return 2;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "Failed to read '%s'\n", path);
        return 2;
    }
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    rewind(f);

    char* src = malloc((size_t)sz + 1);
    if (!src) {
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return 2;
    }
    size_t r = fread(src, 1, (size_t)sz, f);
    src[r] = '\0';
    fclose(f);

    Lexer lex;
    lexer_init(&lex, src, path);

    Parser parser;
    parser_init(&parser, &lex);

    parser_run(&parser);

    free(src);
    return 0;
}
