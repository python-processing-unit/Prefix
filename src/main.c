#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "builtins.h"

int main(int argc, char** argv) {
    const char* path = NULL;
    if (argc >= 2) path = argv[1];
    else path = "../test.pre";

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open '%s'\n", path);
        return PREFIX_ERROR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fprintf(stderr, "Failed to read '%s'\n", path); return PREFIX_ERROR_IO; }
    long sz = ftell(f); if (sz < 0) sz = 0; rewind(f);

    char* src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); fprintf(stderr, "Out of memory\n"); return PREFIX_ERROR_MEMORY; }
    size_t r = fread(src, 1, (size_t)sz, f);
    src[r] = '\0';
    fclose(f);

    Lexer lex;
    lexer_init(&lex, src, path);

    Parser parser;
    parser_init(&parser, &lex);

    Stmt* program = parser_parse(&parser);
    if (parser.had_error) {
        free(src);
        return PREFIX_ERROR_SYNTAX;
    }

    // Provide process argv to builtins (ARGV)
    builtins_set_argv(argc, argv);

    ExecResult res = exec_program(program);
    if (res.status == EXEC_ERROR) {
        fprintf(stderr, "Runtime error: %s at %d:%d\n", res.error ? res.error : "error", res.error_line, res.error_column);
        free(src);
        return PREFIX_ERROR_RUNTIME;
    }

    free(src);
    return PREFIX_SUCCESS;
}
