#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char *source;
    const char *filename;
    size_t source_len;
    size_t current;
    int line;
    int column;
    Token pending_token;
    bool has_pending;
    bool in_fmt_string;
    bool fmt_mode;
    int fmt_depth;
    char fmt_quote;
    char *fmt_buffer;
    size_t fmt_buffer_len;
    size_t fmt_buffer_cap;
} Lexer;

void lexer_init(Lexer *lexer, const char *source, const char *filename);
Token lexer_next_token(Lexer *lexer);
// Return a newly-allocated string containing the requested 1-based line from the
// lexer's source (without trailing newline). Caller must free the returned string.
char *lexer_get_line(Lexer *lexer, int line_num);

#endif // LEXER_H
