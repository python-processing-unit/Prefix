#include "token.h"

const char* token_type_to_string(PTokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_IDENT: return "IDENT";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_STRING: return "STRING";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_LANGLE: return "LANGLE";
        case TOKEN_RANGLE: return "RANGLE";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_EQUALS: return "EQUALS";
        case TOKEN_COLON: return "COLON";
        case TOKEN_AT: return "AT";
        case TOKEN_STAR: return "STAR";
        case TOKEN_DASH: return "DASH";
        case TOKEN_TRY: return "TRY";
        case TOKEN_CATCH: return "CATCH";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSEIF: return "ELSEIF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_PARFOR: return "PARFOR";
        case TOKEN_THR: return "THR";
        case TOKEN_FUNC: return "FUNC";
        case TOKEN_LAMBDA: return "LAMBDA";
        case TOKEN_ASYNC: return "ASYNC";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_POP: return "POP";
        case TOKEN_BREAK: return "BREAK";
        case TOKEN_CONTINUE: return "CONTINUE";
        case TOKEN_GOTO: return "GOTO";
        case TOKEN_GOTOPOINT: return "GOTOPOINT";
        default: return "UNKNOWN";
    }
}

void free_token(Token* token) {
    if (token->literal != NULL) {
        free(token->literal);
        token->literal = NULL;
    }
}
