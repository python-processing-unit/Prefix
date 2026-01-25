#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "common.h"

// Parser definition would go here.
// For Stage 2, we just need to ensure the structure exists.

typedef struct {
    Lexer* lexer;
    Token current_token;
    Token previous_token;
    bool panic_mode;
    bool had_error;
} Parser;

void parser_init(Parser* parser, Lexer* lexer);
void parser_run(Parser* parser);

#endif // PARSER_H
