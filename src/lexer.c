#include "lexer.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

// Memory hardening helpers
static void* safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Fatal error: Out of memory.\n");
        exit(1);
    }
    return ptr;
}

static void* safe_realloc(void* ptr, size_t size) {
    void* new_ptr = realloc(ptr, size);
    if (new_ptr == NULL) {
        fprintf(stderr, "Fatal error: Out of memory.\n");
        exit(1);
    }
    return new_ptr;
}

static char* safe_strdup(const char* str) {
    if (str == NULL) return NULL;
    char* copy = strdup(str);
    if (copy == NULL) {
        fprintf(stderr, "Fatal error: Out of memory.\n");
        exit(1);
    }
    return copy;
}

// Helper functions declarations
static bool is_at_end(Lexer* lexer);
static char advance(Lexer* lexer);
static char peek(Lexer* lexer);
static char peek_next(Lexer* lexer);
static Token make_token(Lexer* lexer, PTokenType type, const char* start, size_t length);
static Token error_token(Lexer* lexer, const char* message);
static Token string_token(Lexer* lexer, char quote_char);
static Token number_token(Lexer* lexer, bool is_negative_start);
static Token identifier_token(Lexer* lexer);
static void consume_line_continuation(Lexer* lexer);
static int hex_digit(char c);

static PTokenType check_keyword(const char* text, size_t length) {
#define KEYWORD(str, type) \
    if (length == strlen(str) && memcmp(text, str, length) == 0) return type;

    KEYWORD("TRY", TOKEN_TRY);
    KEYWORD("CATCH", TOKEN_CATCH);
    KEYWORD("IF", TOKEN_IF);
    KEYWORD("ELSEIF", TOKEN_ELSEIF);
    KEYWORD("ELSE", TOKEN_ELSE);
    KEYWORD("WHILE", TOKEN_WHILE);
    KEYWORD("FOR", TOKEN_FOR);
    KEYWORD("PARFOR", TOKEN_PARFOR);
    KEYWORD("THR", TOKEN_THR);
    KEYWORD("FUNC", TOKEN_FUNC);
    KEYWORD("LAMBDA", TOKEN_LAMBDA);
    KEYWORD("ASYNC", TOKEN_ASYNC);
    KEYWORD("RETURN", TOKEN_RETURN);
    KEYWORD("POP", TOKEN_POP);
    KEYWORD("BREAK", TOKEN_BREAK);
    KEYWORD("CONTINUE", TOKEN_CONTINUE);
    KEYWORD("GOTO", TOKEN_GOTO);
    KEYWORD("GOTOPOINT", TOKEN_GOTOPOINT);

#undef KEYWORD
    return TOKEN_IDENT;
}

void lexer_init(Lexer* lexer, const char* source, const char* filename) {
    lexer->source = source;
    lexer->filename = filename;
    lexer->source_len = strlen(source);
    lexer->current = 0;
    lexer->line = 1;
    lexer->column = 1;
}

char* lexer_get_line(Lexer* lexer, int line_num) {
    if (!lexer || !lexer->source || line_num <= 0) return strdup("");
    size_t idx = 0;
    int cur_line = 1;
    size_t start = 0;
    // Advance to the start of the requested line
    while (idx < lexer->source_len && cur_line < line_num) {
        if (lexer->source[idx] == '\n') cur_line++;
        idx++;
    }
    if (cur_line != line_num || idx >= lexer->source_len) {
        // Requested line is past end; return empty string
        return strdup("");
    }
    start = idx;
    size_t end = start;
    while (end < lexer->source_len && lexer->source[end] != '\n' && lexer->source[end] != '\r') end++;
    size_t len = end - start;
    char* out = (char*)malloc(len + 1);
    if (!out) return strdup("");
    memcpy(out, lexer->source + start, len);
    out[len] = '\0';
    // Trim trailing whitespace
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t' || out[len-1] == '\r' || out[len-1] == '\n')) {
        out[--len] = '\0';
    }
    return out;
}

static bool is_at_end(Lexer* lexer) {
    return lexer->current >= lexer->source_len;
}

static char advance(Lexer* lexer) {
    char c = lexer->source[lexer->current++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static char peek(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->source[lexer->current];
}

static char peek_next(Lexer* lexer) {
    if (lexer->current + 1 >= lexer->source_len) return '\0';
    return lexer->source[lexer->current + 1];
}

static Token make_token(Lexer* lexer, PTokenType type, const char* start, size_t length) {
    Token token;
    token.type = type;
    token.line = lexer->line;
    token.column = lexer->column - (int)length; // Approximate start column
    if (length == 0 && start != NULL) {
        token.literal = safe_strdup(start);
    } else if (start != NULL) {
        token.literal = (char*)safe_malloc(length + 1);
        memcpy(token.literal, start, length);
        token.literal[length] = '\0';
    } else {
        token.literal = NULL;
    }
    return token;
}

static Token error_token(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.line = lexer->line;
    token.column = lexer->column;
    token.literal = safe_strdup(message);
    return token;
}

static void consume_line_continuation(Lexer* lexer) {
    advance(lexer); // consume '^'
    char next = peek(lexer);
    if (next == '\n') {
        advance(lexer);
    } else if (next == '\r') {
        advance(lexer);
        if (peek(lexer) == '\n') advance(lexer);
    } else if (next == '!') {
        while (!is_at_end(lexer) && peek(lexer) != '\n') {
            advance(lexer);
        }
        if (!is_at_end(lexer)) advance(lexer); // consume newline
    }
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Token lexer_next_token(Lexer* lexer) {
    while (!is_at_end(lexer)) {
        char c = peek(lexer);

        if (c == ' ' || c == '\t') {
            advance(lexer);
            continue;
        }
        if (c == '\r') {
            advance(lexer);
            continue;
        }
        if (c == '^') {
            if (lexer->current + 1 < lexer->source_len) {
                char next = lexer->source[lexer->current + 1];
                if (next == '\n' || next == '\r' || next == '!') {
                     consume_line_continuation(lexer); 
                     continue;
                }
            }
            advance(lexer); 
            return error_token(lexer, "Invalid line continuation");
        }
        if (c == '\n') {
            advance(lexer);
            Token t = {TOKEN_NEWLINE, safe_strdup("\n"), lexer->line - 1, lexer->column}; 
            return t;
        }
        if (c == ';') {
            advance(lexer);
            Token t = {TOKEN_NEWLINE, safe_strdup("\n"), lexer->line, lexer->column};
            return t;
        }
        if (c == '!') {
            while (!is_at_end(lexer) && peek(lexer) != '\n') {
                advance(lexer);
            }
            continue;
        }

        if (c == '(') { advance(lexer); return make_token(lexer, TOKEN_LPAREN, "(", 1); }
        if (c == ')') { advance(lexer); return make_token(lexer, TOKEN_RPAREN, ")", 1); }
        if (c == '{') { advance(lexer); return make_token(lexer, TOKEN_LBRACE, "{", 1); }
        if (c == '}') { advance(lexer); return make_token(lexer, TOKEN_RBRACE, "}", 1); }
        if (c == '[') { advance(lexer); return make_token(lexer, TOKEN_LBRACKET, "[", 1); }
        if (c == ']') { advance(lexer); return make_token(lexer, TOKEN_RBRACKET, "]", 1); }
        if (c == '<') { advance(lexer); return make_token(lexer, TOKEN_LANGLE, "<", 1); }
        if (c == '>') { advance(lexer); return make_token(lexer, TOKEN_RANGLE, ">", 1); }
        if (c == ',') { advance(lexer); return make_token(lexer, TOKEN_COMMA, ",", 1); }
        if (c == '=') { advance(lexer); return make_token(lexer, TOKEN_EQUALS, "=", 1); }
        if (c == ':') { advance(lexer); return make_token(lexer, TOKEN_COLON, ":", 1); }
        if (c == '@') { advance(lexer); return make_token(lexer, TOKEN_AT, "@", 1); }
        if (c == '*') { advance(lexer); return make_token(lexer, TOKEN_STAR, "*", 1); }

        if (c == '"' || c == '\'') {
            return string_token(lexer, c);
        }

        if (c == '-') {
            int start_line = lexer->line;
            int start_col = lexer->column;
            advance(lexer);

            // Determine the previous non-whitespace character to avoid
            // interpreting a mid-token '-' (eg. `1-10`) as a negative number
            // start. If the previous significant character is a digit,
            // identifier character, or a closing bracket, treat '-' as a
            // plain DASH token.
            int prev_index = (int)lexer->current - 2; // position before the '-'
            while (prev_index >= 0) {
                char pc = lexer->source[prev_index];
                if (pc == ' ' || pc == '\t' || pc == '\r' || pc == '\n') {
                    prev_index--;
                    continue;
                }
                // found a non-whitespace previous char
                if (pc == '0' || pc == '1' || isalnum((unsigned char)pc) || pc == ']' || pc == ')' || pc == '}' ) {
                    Token t = {TOKEN_DASH, safe_strdup("-"), start_line, start_col};
                    return t;
                }
                break;
            }

            size_t lookahead = lexer->current;
            while (lookahead < lexer->source_len &&
                  (lexer->source[lookahead] == ' ' || lexer->source[lookahead] == '\t' || lexer->source[lookahead] == '\r')) {
                lookahead++;
            }
            if (lookahead < lexer->source_len &&
               (lexer->source[lookahead] == '0' || lexer->source[lookahead] == '1')) {
                   while(lexer->current < lookahead) advance(lexer);
                   return number_token(lexer, true);
            }

            Token t = {TOKEN_DASH, safe_strdup("-"), start_line, start_col};
            return t;
        }

        if (c == '0' || c == '1') {
            return number_token(lexer, false);
        }

        if (strchr("abcdefghijklmnopqrstuvwxyz23456789/ABCDEFGHIJKLMNOPQRSTUVWXYZ$%&~_+|?", c)) {
            return identifier_token(lexer);
        }

        char err_msg[50];
        snprintf(err_msg, 50, "Unexpected character: %c", c);
        advance(lexer);
        return error_token(lexer, err_msg);
    }

    Token t = {TOKEN_EOF, NULL, lexer->line, lexer->column};
    return t;
}

static Token string_token(Lexer* lexer, char quote_char) {
    int start_line = lexer->line;
    int start_col = lexer->column;
    advance(lexer); 
    
    size_t capacity = 64;
    char* value = safe_malloc(capacity);
    size_t len_val = 0;
    bool raw_mode = false;
    
    while (!is_at_end(lexer)) {
        char c = peek(lexer);
        
        if (c == quote_char) {
            advance(lexer);
            value[len_val] = '\0';
            Token t = {TOKEN_STRING, value, start_line, start_col};
            return t;
        }
        
        if (c == '\n' || c == '\r') {
            free(value);
            return error_token(lexer, "Unterminated string literal");
        }
        
        if (c == '\\') {
            advance(lexer);
            if (is_at_end(lexer)) {
                free(value);
                return error_token(lexer, "Unterminated string literal");
            }
            char next = peek(lexer);
            
            if (raw_mode) {
                if (next == 'R') {
                    advance(lexer);
                    raw_mode = false;
                    continue;
                }
                if (len_val + 2 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
                value[len_val++] = '\\';
                advance(lexer); 
                value[len_val++] = next;
                continue;
            }
            
            if (next == 'R') {
                advance(lexer);
                raw_mode = true;
                continue;
            }
            
            int codepoint = -1;
            char esc_char = 0;
            bool handled = true;
            
            switch(next) {
                case '\\': esc_char = '\\'; break;
                case '\'': esc_char = '\''; break;
                case '"':  esc_char = '"'; break;
                case 'a':  esc_char = '\a'; break;
                case 'b':  esc_char = '\b'; break;
                case 'f':  esc_char = '\f'; break;
                case 'n':  esc_char = '\n'; break;
                case 'r':  esc_char = '\r'; break;
                case 't':  esc_char = '\t'; break;
                case 'v':  esc_char = '\v'; break;
                case 'e':  esc_char = '\x1b'; break;
                case 'x': handled = false; break; // specific handling
                default: handled = false; break;
            }
            
            if (handled) {
                advance(lexer);
                if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
                value[len_val++] = esc_char;
                continue;
            }
            
            // Hex/Unicode
            if (next == 'x') {
                 advance(lexer); 
                 int d1 = hex_digit(peek(lexer)); advance(lexer);
                 int d2 = hex_digit(peek(lexer)); advance(lexer);
                 if (d1 < 0 || d2 < 0) { free(value); return error_token(lexer, "Invalid \\x escape"); }
                 codepoint = (d1 << 4) | d2;
            } else if (next == 'u') {
                 advance(lexer);
                 int acc = 0;
                 for(int i=0; i<4; i++) {
                     int d = hex_digit(peek(lexer)); advance(lexer);
                     if (d < 0) { free(value); return error_token(lexer, "Invalid \\u escape"); }
                     acc = (acc << 4) | d;
                 }
                 codepoint = acc;
            } else if (next == 'U') {
                 advance(lexer);
                 int acc = 0;
                 for(int i=0; i<8; i++) {
                     int d = hex_digit(peek(lexer)); advance(lexer);
                     if (d < 0) { free(value); return error_token(lexer, "Invalid \\U escape"); }
                     acc = (acc << 4) | d;
                 }
                 codepoint = acc;
            } else {
                 free(value);
                 return error_token(lexer, "Unknown escape sequence");
            }
            
            if (codepoint != -1) {
                if (codepoint <= 0x7F) {
                    if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
                    value[len_val++] = (char)codepoint;
                } else {
                    if (len_val + 4 >= capacity) { capacity += 10; value = safe_realloc(value, capacity); }
                    if(codepoint <= 0x7FF) {
                        value[len_val++] = 0xC0 | ((codepoint >> 6) & 0x1F);
                        value[len_val++] = 0x80 | (codepoint & 0x3F);
                    } else if(codepoint <= 0xFFFF) {
                        value[len_val++] = 0xE0 | ((codepoint >> 12) & 0x0F);
                        value[len_val++] = 0x80 | ((codepoint >> 6) & 0x3F);
                        value[len_val++] = 0x80 | (codepoint & 0x3F);
                    } else {
                        value[len_val++] = 0xF0 | ((codepoint >> 18) & 0x07);
                        value[len_val++] = 0x80 | ((codepoint >> 12) & 0x3F);
                        value[len_val++] = 0x80 | ((codepoint >> 6) & 0x3F);
                        value[len_val++] = 0x80 | (codepoint & 0x3F);
                    }
                }
                continue;
            }
        }
        
        advance(lexer);
        if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
        value[len_val++] = c;
    }
    
    free(value);
    return error_token(lexer, "Unterminated string literal");
}

static Token identifier_token(Lexer* lexer) {
    int start_line = lexer->line;
    int start_col = lexer->column;
    
    size_t capacity = 32;
    char* value = safe_malloc(capacity);
    size_t len_val = 0;
    
    while (!is_at_end(lexer)) {
        char c = peek(lexer);
        if (strchr("abcdefghijklmnopqrstuvwxyz1234567890./ABCDEFGHIJKLMNOPQRSTUVWXYZ$%&~_+|?", c)) {
            advance(lexer);
            if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
            value[len_val++] = c;
        } else if (c == '^') {
            consume_line_continuation(lexer);
        } else {
            break;
        }
    }
    value[len_val] = '\0';
    PTokenType type = check_keyword(value, len_val);
    
    Token t;
    t.type = type;
    t.literal = value;
    t.line = start_line;
    t.column = start_col;
    return t;
}

static Token number_token(Lexer* lexer, bool is_negative_start) {
    int start_line = lexer->line;
    int start_col = lexer->column;
    
    size_t capacity = 32;
    char* value = safe_malloc(capacity);
    size_t len_val = 0;
    
    if (is_negative_start) {
        value[len_val++] = '-';
    }
    
    while (!is_at_end(lexer)) {
        char c = peek(lexer);
        if (c == '0' || c == '1') {
            advance(lexer);
            if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
            value[len_val++] = c;
        } else if (c == '^') {
            consume_line_continuation(lexer);
        } else {
            break;
        }
    }
    
    if (peek(lexer) == '.') {
        size_t saved_current = lexer->current;
        int saved_line = lexer->line;
        int saved_col = lexer->column;
        
        advance(lexer); 
        
        bool has_frac = false;
        size_t frac_start_len = len_val;
        if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
        value[len_val++] = '.';
        
        while (!is_at_end(lexer)) {
            char c = peek(lexer);
            if (c == '0' || c == '1') {
                advance(lexer);
                has_frac = true;
                if (len_val + 1 >= capacity) { capacity *= 2; value = safe_realloc(value, capacity); }
                value[len_val++] = c;
            } else if (c == '^') {
                consume_line_continuation(lexer);
            } else {
                break;
            }
        }
        
            if (!has_frac) {
            lexer->current = saved_current;
            lexer->line = saved_line;
            lexer->column = saved_col;
            len_val = frac_start_len;
            value[len_val] = '\0';
            Token t = {TOKEN_NUMBER, value, start_line, start_col};
            return t;
        }
        
        value[len_val] = '\0';
        Token t = {TOKEN_FLOAT, value, start_line, start_col};
        return t;
    }
    
    value[len_val] = '\0';
    Token t = {TOKEN_NUMBER, value, start_line, start_col};
    return t;
}
