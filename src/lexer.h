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
// Return a newly-allocated string containing the requested 1-based line from the
// lexer's source (without trailing newline). Caller must free the returned string.
char* lexer_get_line(Lexer* lexer, int line);

#endif // LEXER_H
