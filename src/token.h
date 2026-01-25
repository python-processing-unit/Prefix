#ifndef TOKEN_H
#define TOKEN_H

#include "common.h"

typedef enum {
    // Special
    TOKEN_EOF,
    TOKEN_ERROR,
    TOKEN_NEWLINE,

    // Literals
    TOKEN_IDENT,
    TOKEN_NUMBER,  // Binary integer (signed or unsigned)
    TOKEN_FLOAT,   // Binary float
    TOKEN_STRING,

    // Symbols
    TOKEN_LPAREN,   // (
    TOKEN_RPAREN,   // )
    TOKEN_LBRACE,   // {
    TOKEN_RBRACE,   // }
    TOKEN_LBRACKET, // [
    TOKEN_RBRACKET, // ]
    TOKEN_LANGLE,   // <
    TOKEN_RANGLE,   // >
    TOKEN_COMMA,    // ,
    TOKEN_EQUALS,   // =
    TOKEN_COLON,    // :
    TOKEN_AT,       // @
    TOKEN_STAR,     // *
    TOKEN_DASH,     // - (when used as slice range separator)

    // Keywords
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_IF,
    TOKEN_ELSEIF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_PARFOR,
    TOKEN_THR,
    TOKEN_FUNC,
    TOKEN_LAMBDA,
    TOKEN_ASYNC,
    TOKEN_RETURN,
    TOKEN_POP,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_GOTO,
    TOKEN_GOTOPOINT

} TokenType;

typedef struct {
    TokenType type;
    char* literal; // For IDENT, NUMBER, FLOAT, STRING, etc.
    int line;
    int column;
} Token;

const char* token_type_to_string(TokenType type);
void free_token(Token* token);

#endif // TOKEN_H
