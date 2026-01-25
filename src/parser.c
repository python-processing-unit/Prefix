#include "parser.h"
#include <stdio.h>

void parser_init(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->panic_mode = false;
    parser->had_error = false;
}

static void advance(Parser* parser) {
    parser->previous_token = parser->current_token;
    
    for (;;) {
        parser->current_token = lexer_next_token(parser->lexer);
        if (parser->current_token.type != TOKEN_ERROR) break;
        
        // Report error
        fprintf(stderr, "Error at line %d col %d: %s\n", 
                parser->current_token.line, parser->current_token.column, 
                parser->current_token.literal);
        parser->had_error = true;
    }
}

// Simple test driver for the parser/lexer interaction
void parser_run(Parser* parser) {
    advance(parser);
    while (parser->current_token.type != TOKEN_EOF) {
        printf("Token: %s", token_type_to_string(parser->current_token.type));
        if (parser->current_token.literal) {
            printf(" ('%s')", parser->current_token.literal);
        }
        printf(" at %d:%d\n", parser->current_token.line, parser->current_token.column);
        
        advance(parser);
    }
}
