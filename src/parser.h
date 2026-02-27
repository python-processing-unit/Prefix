#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "common.h"
#include "ast.h"

typedef struct {
    Lexer* lexer;
    Token current_token;
    Token previous_token;
    Token next_token;
    bool panic_mode;
    bool had_error;
    char* error_msg;
    int error_line;
    int error_col;
} Parser;

void parser_init(Parser* parser, Lexer* lexer);
Stmt* parser_parse(Parser* parser);

#endif // PARSER_H
