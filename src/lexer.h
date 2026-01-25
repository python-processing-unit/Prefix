#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char* source;
    const char* filename;
    size_t source_len;
    size_t current;
    int line;
    int column;
} Lexer;

void lexer_init(Lexer* lexer, const char* source, const char* filename);
Token lexer_next_token(Lexer* lexer);

#endif // LEXER_H
