#include "parser.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_digit_tables(void);

static void report_error(Parser *parser, const char *message) {
    if (parser->panic_mode) {
        return;
    }
    parser->panic_mode = true;
    parser->had_error = true;
    /* Record last error for possible conversion into a runtime throw
       so try/catch can handle parse-time errors that occur inside
       parsed blocks. Also emit the usual diagnostic to stderr. */
    if (parser->error_msg) {
        free(parser->error_msg);
        parser->error_msg = NULL;
    }
    parser->error_msg = strdup(message);
    parser->error_line = parser->current_token.line;
    parser->error_col = parser->current_token.column;
    fprintf(stderr, "ParseError at %d:%d: %s\n", parser->current_token.line, parser->current_token.column, message);
}

void parser_init(Parser *parser, Lexer *lexer) {
    init_digit_tables();
    parser->lexer = lexer;
    parser->panic_mode = false;
    parser->had_error = false;
    parser->error_msg = NULL;
    parser->error_line = 0;
    parser->error_col = 0;
    parser->current_token = lexer_next_token(parser->lexer);
    parser->next_token = lexer_next_token(parser->lexer);
    parser->lookahead2_token = lexer_next_token(parser->lexer);
}

static void shift_tokens(Parser *parser) {
    parser->previous_token = parser->current_token;
    parser->current_token = parser->next_token;
    parser->next_token = parser->lookahead2_token;
    parser->lookahead2_token = lexer_next_token(parser->lexer);
}

static void advance(Parser *parser) {
    shift_tokens(parser);
    while (parser->current_token.type == TOKEN_ERROR) {
        report_error(parser, parser->current_token.literal);
        parser->panic_mode = false;
        shift_tokens(parser);
    }
}

static bool match(Parser *parser, PTokenType type) {
    if (parser->current_token.type != type) {
        return false;
    }
    advance(parser);
    return true;
}

static bool consume(Parser *parser, PTokenType type, const char *message) {
    if (parser->current_token.type == type) {
        advance(parser);
        return true;
    }
    report_error(parser, message);
    return false;
}

static void skip_newlines(Parser *parser) {
    while (parser->current_token.type == TOKEN_NEWLINE) {
        advance(parser);
    }
}

static DeclType parse_type_name(const char *name) {
    static const struct {
        const char *name;
        DeclType type;
    } k_types[] = {
        {"bool", TYPE_BOOL}, {"int", TYPE_INT},   {"float", TYPE_FLOAT},   {"str", TYPE_STR},
        {"map", TYPE_MAP},   {"func", TYPE_FUNC}, {"thread", TYPE_THREAD}, {"tensor", TYPE_TENSOR},
    };
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        if (strcmp(name, k_types[i].name) == 0) {
            return k_types[i].type;
        }
    }
    return TYPE_UNKNOWN;
}

// Forward declarations for type annotation parsing
static bool is_type_token(PTokenType type);
static int parse_prefixed_int_literal(const char *text, int64_t *out_value, int *out_base);
static Expr *parse_expression(Parser *parser);

typedef struct {
    DeclType type;
    int base;          // 0 = parent, 2..64 = named int/float base
    Expr *schema_expr; // NEW: map schema expression, NULL for non-map or bare map
    Token end_tok;     // last token of the annotation (type name, closing '}', or closing brace)
} TypeAnnotation;

static Expr *parse_index_suffix(Parser *parser, Expr *base);
static Stmt *parse_simple_typed_declaration(Parser *parser, Token type_tok, TypeAnnotation ta);

static TypeAnnotation parse_type_annotation(Parser *parser) {
    TypeAnnotation ta;
    ta.type = TYPE_UNKNOWN;
    ta.base = 0;
    ta.schema_expr = NULL;
    memset(&ta.end_tok, 0, sizeof(ta.end_tok));
    if (!is_type_token(parser->current_token.type)) {
        return ta;
    }
    ta.type = parse_type_name(parser->current_token.literal);
    if (ta.type == TYPE_UNKNOWN) {
        report_error(parser, "Unknown type name");
        return ta;
    }
    ta.end_tok = parser->current_token;
    advance(parser); // consume type name

    if (ta.type == TYPE_INT || ta.type == TYPE_FLOAT) {
        if (match(parser, TOKEN_LBRACE)) {
            // Parse base as an int literal
            Token lit = parser->current_token;
            int64_t base_val = 0;
            int lit_base = 2;
            if (lit.type != TOKEN_NUMBER || !parse_prefixed_int_literal(lit.literal, &base_val, &lit_base)) {
                if (lit.type == TOKEN_RBRACE) {
                    report_error(parser, "int{} requires a base number inside braces");
                } else {
                    report_error(parser, "Expected int literal for base");
                }
                ta.type = TYPE_UNKNOWN;
                return ta;
            }
            if (base_val < 2 || base_val > 64) {
                report_error(parser, "Base must be between 2 and 64");
                ta.type = TYPE_UNKNOWN;
                return ta;
            }
            ta.base = (int)base_val;
            advance(parser); // consume number
            if (parser->current_token.type != TOKEN_RBRACE) {
                report_error(parser, "Expected '}' after base");
                ta.type = TYPE_UNKNOWN;
                return ta;
            }
            ta.end_tok = parser->current_token;
            advance(parser); // consume '}'
        }
    } else if (ta.type == TYPE_MAP && match(parser, TOKEN_LBRACE)) {
        // Parse map schema expression
        if (parser->current_token.type == TOKEN_RBRACE) {
            report_error(parser, "map{} requires a schema expression; use map without braces for bare map");
            ta.type = TYPE_UNKNOWN;
            return ta;
        }
        ta.schema_expr = parse_expression(parser);
        if (!ta.schema_expr) {
            ta.type = TYPE_UNKNOWN;
            return ta;
        }
        if (!consume(parser, TOKEN_RBRACE, "Expected '}' after map schema")) {
            ta.type = TYPE_UNKNOWN;
            return ta;
        }
        ta.end_tok = parser->previous_token; // the '}'
    } else if (match(parser, TOKEN_LBRACE)) {
        report_error(parser, "Base annotation is only valid for int and float");
        ta.type = TYPE_UNKNOWN;
    }
    return ta;
}

static const char *k_type_name_gap_error = "Type annotations require one or more spaces between type and name";

static size_t token_source_width(const Token *token) {
    if (!token) {
        return 0;
    }
    if (token->literal) {
        return strlen(token->literal);
    }

    switch (token->type) {
    case TOKEN_FUNC:
        return 4;
    case TOKEN_THREAD:
        return 3;
    default:
        return 0;
    }
}

static void advance_past_line_terminator(const char *src, size_t src_len, size_t *pos) {
    if (*pos < src_len && src[*pos] == '\r') {
        (*pos)++;
        if (*pos < src_len && src[*pos] == '\n') {
            (*pos)++;
        }
    } else if (*pos < src_len && src[*pos] == '\n') {
        (*pos)++;
    }
}

static bool scan_gap_characters(Parser *parser, const Lexer *lexer, size_t left_end_offset, size_t right_start_offset,
                                const char *message) {
    size_t pos = left_end_offset;
    while (pos < right_start_offset) {
        char ch = lexer->source[pos];
        if (ch == ' ') {
            pos++;
            continue;
        }
        if (ch == '^') {
            if (pos + 1 >= lexer->source_len) {
                report_error(parser, message);
                return false;
            }
            char next = lexer->source[pos + 1];
            if (next == '\n') {
                pos += 2;
                continue;
            }
            if (next == '\r') {
                pos += 2;
                if (pos < lexer->source_len && lexer->source[pos] == '\n') {
                    pos++;
                }
                continue;
            }
            if (next == '!') {
                pos += 2;
                while (pos < lexer->source_len && lexer->source[pos] != '\n' && lexer->source[pos] != '\r') {
                    pos++;
                }
                advance_past_line_terminator(lexer->source, lexer->source_len, &pos);
                continue;
            }
            report_error(parser, message);
            return false;
        }
        report_error(parser, message);
        return false;
    }
    return true;
}

static bool require_space_only_gap(Parser *parser, const Token *left, const Token *right, const char *message) {
    char *line_text;
    size_t line_len;
    size_t left_width;
    int gap_start_col;
    int gap_end_col;

    /* Allow the right token to be an identifier or the '(' that begins a
       parameter list. This lets constructs like `LAMBDA int (int x)` use
       the same space-only gap rules as typed function declarations. */
    if (!left || !right || (right->type != TOKEN_IDENT && right->type != TOKEN_LPAREN)) {
        report_error(parser, message);
        return false;
    }
    /* If tokens are on different physical lines, allow that only when the
       characters between them in the raw source consist solely of spaces and
       valid line-continuation sequences (caret followed by a newline or a
       caret immediately before a comment). This implements the language's
       caret continuation semantics so declarations split across physical
       lines can still be treated as a single logical line. */
    if (left->line != right->line) {
        Lexer *lexer = parser->lexer;
        if (!lexer || !lexer->source) {
            report_error(parser, message);
            return false;
        }

        left_width = token_source_width(left);
        if (left_width == 0) {
            report_error(parser, message);
            return false;
        }

        /* Compute absolute offsets for the end of the left token and the
           start of the right token by scanning to each line start. */
        size_t idx = 0;
        int cur_line = 1;
        while (idx < lexer->source_len && cur_line < left->line) {
            if (lexer->source[idx] == '\n') {
                cur_line++;
            }
            idx++;
        }
        if (cur_line != left->line) {
            report_error(parser, message);
            return false;
        }
        size_t left_line_start = idx;
        size_t left_start_offset = left_line_start + (size_t)((left->column > 0) ? (left->column - 1) : 0);
        size_t left_end_offset = left_start_offset + left_width;

        idx = 0;
        cur_line = 1;
        while (idx < lexer->source_len && cur_line < right->line) {
            if (lexer->source[idx] == '\n') {
                cur_line++;
            }
            idx++;
        }
        if (cur_line != right->line) {
            report_error(parser, message);
            return false;
        }
        size_t right_line_start = idx;
        size_t right_start_offset = right_line_start + (size_t)((right->column > 0) ? (right->column - 1) : 0);

        if (right_start_offset <= left_end_offset) {
            report_error(parser, message);
            return false;
        }

        if (!scan_gap_characters(parser, lexer, left_end_offset, right_start_offset, message)) {
            return false;
        }
        return true;
    }

    left_width = token_source_width(left);
    if (left_width == 0) {
        report_error(parser, message);
        return false;
    }

    gap_start_col = left->column + (int)left_width;
    gap_end_col = right->column - 1;
    if (gap_end_col < gap_start_col) {
        report_error(parser, message);
        return false;
    }

    line_text = lexer_get_line(parser->lexer, left->line);
    if (!line_text) {
        report_error(parser, message);
        return false;
    }

    line_len = strlen(line_text);
    if ((size_t)gap_end_col > line_len) {
        free(line_text);
        report_error(parser, message);
        return false;
    }

    for (int col = gap_start_col; col <= gap_end_col; col++) {
        if (line_text[col - 1] != ' ') {
            free(line_text);
            report_error(parser, message);
            return false;
        }
    }

    free(line_text);
    return true;
}

static int base_from_literal_prefix(const char *s, size_t *prefix_len) {
    if (!s || s[0] != '0') {
        return -1;
    }
    static const struct {
        char prefix;
        int base;
        size_t len;
    } k_table[] = {
        {'b', 2, 2}, {'o', 8, 2}, {'d', 10, 2}, {'x', 16, 2}, {'t', 32, 2}, {'c', 58, 2}, {'s', 64, 2},
    };
    char p = s[1];
    for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++) {
        if (p == k_table[i].prefix) {
            if (prefix_len) {
                *prefix_len = k_table[i].len;
            }
            return k_table[i].base;
        }
    }
    if (p == 'r' && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3])) {
        int b = ((s[2] - '0') * 10) + (s[3] - '0');
        if (prefix_len) {
            *prefix_len = 4;
        }
        return b;
    }
    return -1;
}

static uint8_t k_digit64_tbl[256];
static uint8_t k_digit58_tbl[256];

static void init_digit_tables(void) {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        k_digit64_tbl[i] = 0xFF;
        k_digit58_tbl[i] = 0xFF;
    }
    k_digit64_tbl['0'] = 0;
    k_digit64_tbl['1'] = 1;
    k_digit64_tbl['2'] = 2;
    k_digit64_tbl['3'] = 3;
    k_digit64_tbl['4'] = 4;
    k_digit64_tbl['5'] = 5;
    k_digit64_tbl['6'] = 6;
    k_digit64_tbl['7'] = 7;
    k_digit64_tbl['8'] = 8;
    k_digit64_tbl['9'] = 9;
    k_digit64_tbl['A'] = 10;
    k_digit64_tbl['B'] = 11;
    k_digit64_tbl['C'] = 12;
    k_digit64_tbl['D'] = 13;
    k_digit64_tbl['E'] = 14;
    k_digit64_tbl['F'] = 15;
    k_digit64_tbl['G'] = 16;
    k_digit64_tbl['H'] = 17;
    k_digit64_tbl['I'] = 18;
    k_digit64_tbl['J'] = 19;
    k_digit64_tbl['K'] = 20;
    k_digit64_tbl['L'] = 21;
    k_digit64_tbl['M'] = 22;
    k_digit64_tbl['N'] = 23;
    k_digit64_tbl['O'] = 24;
    k_digit64_tbl['P'] = 25;
    k_digit64_tbl['Q'] = 26;
    k_digit64_tbl['R'] = 27;
    k_digit64_tbl['S'] = 28;
    k_digit64_tbl['T'] = 29;
    k_digit64_tbl['U'] = 30;
    k_digit64_tbl['V'] = 31;
    k_digit64_tbl['W'] = 32;
    k_digit64_tbl['X'] = 33;
    k_digit64_tbl['Y'] = 34;
    k_digit64_tbl['Z'] = 35;
    k_digit64_tbl['a'] = 36;
    k_digit64_tbl['b'] = 37;
    k_digit64_tbl['c'] = 38;
    k_digit64_tbl['d'] = 39;
    k_digit64_tbl['e'] = 40;
    k_digit64_tbl['f'] = 41;
    k_digit64_tbl['g'] = 42;
    k_digit64_tbl['h'] = 43;
    k_digit64_tbl['i'] = 44;
    k_digit64_tbl['j'] = 45;
    k_digit64_tbl['k'] = 46;
    k_digit64_tbl['l'] = 47;
    k_digit64_tbl['m'] = 48;
    k_digit64_tbl['n'] = 49;
    k_digit64_tbl['o'] = 50;
    k_digit64_tbl['p'] = 51;
    k_digit64_tbl['q'] = 52;
    k_digit64_tbl['r'] = 53;
    k_digit64_tbl['s'] = 54;
    k_digit64_tbl['t'] = 55;
    k_digit64_tbl['u'] = 56;
    k_digit64_tbl['v'] = 57;
    k_digit64_tbl['w'] = 58;
    k_digit64_tbl['x'] = 59;
    k_digit64_tbl['y'] = 60;
    k_digit64_tbl['z'] = 61;
    k_digit64_tbl['+'] = 62;
    k_digit64_tbl['_'] = 63;

    k_digit58_tbl['1'] = 0;
    k_digit58_tbl['2'] = 1;
    k_digit58_tbl['3'] = 2;
    k_digit58_tbl['4'] = 3;
    k_digit58_tbl['5'] = 4;
    k_digit58_tbl['6'] = 5;
    k_digit58_tbl['7'] = 6;
    k_digit58_tbl['8'] = 7;
    k_digit58_tbl['9'] = 8;
    k_digit58_tbl['A'] = 9;
    k_digit58_tbl['B'] = 10;
    k_digit58_tbl['C'] = 11;
    k_digit58_tbl['D'] = 12;
    k_digit58_tbl['E'] = 13;
    k_digit58_tbl['F'] = 14;
    k_digit58_tbl['G'] = 15;
    k_digit58_tbl['H'] = 16;
    k_digit58_tbl['J'] = 17;
    k_digit58_tbl['K'] = 18;
    k_digit58_tbl['L'] = 19;
    k_digit58_tbl['M'] = 20;
    k_digit58_tbl['N'] = 21;
    k_digit58_tbl['P'] = 22;
    k_digit58_tbl['Q'] = 23;
    k_digit58_tbl['R'] = 24;
    k_digit58_tbl['S'] = 25;
    k_digit58_tbl['T'] = 26;
    k_digit58_tbl['U'] = 27;
    k_digit58_tbl['V'] = 28;
    k_digit58_tbl['W'] = 29;
    k_digit58_tbl['X'] = 30;
    k_digit58_tbl['Y'] = 31;
    k_digit58_tbl['Z'] = 32;
    k_digit58_tbl['a'] = 33;
    k_digit58_tbl['b'] = 34;
    k_digit58_tbl['c'] = 35;
    k_digit58_tbl['d'] = 36;
    k_digit58_tbl['e'] = 37;
    k_digit58_tbl['f'] = 38;
    k_digit58_tbl['g'] = 39;
    k_digit58_tbl['h'] = 40;
    k_digit58_tbl['i'] = 41;
    k_digit58_tbl['j'] = 42;
    k_digit58_tbl['k'] = 43;
    k_digit58_tbl['m'] = 44;
    k_digit58_tbl['n'] = 45;
    k_digit58_tbl['o'] = 46;
    k_digit58_tbl['p'] = 47;
    k_digit58_tbl['q'] = 48;
    k_digit58_tbl['r'] = 49;
    k_digit58_tbl['s'] = 50;
    k_digit58_tbl['t'] = 51;
    k_digit58_tbl['u'] = 52;
    k_digit58_tbl['v'] = 53;
    k_digit58_tbl['w'] = 54;
    k_digit58_tbl['x'] = 55;
    k_digit58_tbl['y'] = 56;
    k_digit58_tbl['z'] = 57;
    initialized = true;
}

int digit_value_for_base(int base, char c) {
    if (base == 58) {
        uint8_t v = k_digit58_tbl[(unsigned char)c];
        if (v == 0xFF) {
            return -1;
        }
        return v;
    }
    uint8_t v = k_digit64_tbl[(unsigned char)c];
    if (v == 0xFF) {
        return -1;
    }
    return v;
}

typedef struct {
    int base;
    const char *digits;
    int neg;
} LiteralPrefix;

static LiteralPrefix parse_literal_prefix(const char *lit) {
    LiteralPrefix lp = {0};
    const char *s = lit;
    if (*s == '-') {
        lp.neg = 1;
        s++;
    }
    size_t prefix_len = 0;
    int base = base_from_literal_prefix(s, &prefix_len);
    if (base < 0 && isdigit((unsigned char)s[0])) {
        base = 10;
        prefix_len = 0;
    }
    if (base >= 2 && base <= 64) {
        lp.base = base;
        lp.digits = s + prefix_len;
    }
    return lp;
}

static int parse_prefixed_int_literal(const char *lit, int64_t *out_value, int *out_base) {
    if (!lit || !out_value || !out_base) {
        return 0;
    }
    LiteralPrefix lp = parse_literal_prefix(lit);
    if (lp.base == 0) {
        return 0;
    }
    const char *digits = lp.digits;
    if (*digits == '\0') {
        return 0;
    }
    if (strchr(digits, '.') != NULL) {
        return 0;
    }

    int64_t acc = 0;
    for (const char *p = digits; *p; p++) {
        int dv = digit_value_for_base(lp.base, *p);
        if (dv < 0 || dv >= lp.base) {
            return 0;
        }
        if (acc > (INT64_MAX - dv) / lp.base) {
            return 0;
        }
        acc = (acc * lp.base) + dv;
    }

    *out_value = lp.neg ? -acc : acc;
    *out_base = lp.base;
    return 1;
}

static int parse_prefixed_float_literal(const char *lit, double *out_value, int *out_base) {
    if (!lit || !out_value || !out_base) {
        return 0;
    }
    LiteralPrefix lp = parse_literal_prefix(lit);
    if (lp.base == 0) {
        return 0;
    }
    const char *digits = lp.digits;
    const char *dot = strchr(digits, '.');
    if (!dot) {
        return 0;
    }
    if (dot == digits || *(dot + 1) == '\0') {
        return 0;
    }

    double int_part = 0.0;
    for (const char *p = digits; p < dot; p++) {
        int dv = digit_value_for_base(lp.base, *p);
        if (dv < 0 || dv >= lp.base) {
            return 0;
        }
        int_part = (int_part * (double)lp.base) + (double)dv;
    }

    double frac_part = 0.0;
    double weight = 1.0 / (double)lp.base;
    for (const char *p = dot + 1; *p; p++) {
        int dv = digit_value_for_base(lp.base, *p);
        if (dv < 0 || dv >= lp.base) {
            return 0;
        }
        frac_part += (double)dv * weight;
        weight /= (double)lp.base;
    }

    double v = int_part + frac_part;
    *out_value = lp.neg ? -v : v;
    *out_base = lp.base;
    return 1;
}

static Expr *parse_expression(Parser *parser);
static Stmt *parse_statement(Parser *parser);
static Stmt *parse_block(Parser *parser);

static void append_parse_error_throw_stmt(Parser *parser, Stmt *block) {
    if (!parser || !block || !parser->error_msg) {
        return;
    }

    char *msg_dup = strdup(parser->error_msg);
    Expr *callee = expr_ident(strdup("throw"), parser->error_line, parser->error_col);
    Expr *call = expr_call(callee, parser->error_line, parser->error_col);
    Expr *arg = expr_str(msg_dup, parser->error_line, parser->error_col);
    expr_list_add(&call->as.call.args, arg);
    Stmt *err_stmt = stmt_expr(call, parser->error_line, parser->error_col);
    stmt_list_add(&block->as.block, err_stmt);
    free(parser->error_msg);
    parser->error_msg = NULL;
    parser->had_error = false;
    parser->panic_mode = false;
}

static bool is_type_token(PTokenType type) { return type == TOKEN_IDENT || type == TOKEN_FUNC || type == TOKEN_THREAD; }

static bool starts_named_type_annotation(Parser *parser) {
    if (!is_type_token(parser->current_token.type)) {
        return false;
    }
    if (parser->next_token.type == TOKEN_IDENT || parser->next_token.type == TOKEN_COLON) {
        return true;
    }
    // Named number types: int{base} or float{base} followed by a name
    // map schema: map{expr} followed by a name
    if (parser->next_token.type == TOKEN_LBRACE) {
        DeclType t = parse_type_name(parser->current_token.literal);
        if (t == TYPE_INT || t == TYPE_FLOAT || t == TYPE_MAP) {
            return true;
        }
    }
    return false;
}

static bool looks_like_func_definition(Parser *parser) {
    if (parser->current_token.type != TOKEN_FUNC || !is_type_token(parser->next_token.type)) {
        return false;
    }
    if (parser->lookahead2_token.type == TOKEN_IDENT || parser->lookahead2_token.type == TOKEN_COLON) {
        return true;
    }
    // Named number return type: func int{base} name(...)
    // map schema return type: func map{expr} name(...)
    if (parser->lookahead2_token.type == TOKEN_LBRACE) {
        DeclType t = parse_type_name(parser->next_token.literal);
        if (t == TYPE_INT || t == TYPE_FLOAT || t == TYPE_MAP) {
            return true;
        }
    }
    return false;
}

static bool parse_one_param(Parser *parser, Param *out_param) {
    bool coerced = false;
    if (match(parser, TOKEN_TILDE)) {
        coerced = true;
    }
    if (!is_type_token(parser->current_token.type)) {
        if (coerced) {
            report_error(parser, "Expected parameter type after '~'");
        } else {
            report_error(parser, "Expected parameter type");
        }
        return false;
    }
    TypeAnnotation ptype = parse_type_annotation(parser);
    if (ptype.type == TYPE_UNKNOWN) {
        return false;
    }
    if (!require_space_only_gap(parser, &ptype.end_tok, &parser->current_token, k_type_name_gap_error)) {
        return false;
    }
    out_param->type = ptype.type;
    out_param->num_base = ptype.base;
    out_param->name = parser->current_token.literal;
    out_param->coerced = coerced;
    out_param->default_value = NULL;
    out_param->schema_expr = ptype.schema_expr;
    advance(parser);
    if (match(parser, TOKEN_EQUALS)) {
        out_param->default_value = parse_expression(parser);
        if (!out_param->default_value) {
            return false;
        }
    }
    return true;
}

static bool parse_param_list(Parser *parser, ParamList *params) {
    if (parser->current_token.type == TOKEN_RPAREN) {
        return true;
    }
    do {
        Param param;
        if (!parse_one_param(parser, &param)) {
            return false;
        }
        param_list_add(params, param);
    } while (match(parser, TOKEN_COMMA));
    return true;
}

static Expr *parse_typed_ident_expr(Parser *parser) {
    Token type_tok = parser->current_token;
    TypeAnnotation ta = parse_type_annotation(parser);
    if (ta.type == TYPE_UNKNOWN) {
        return NULL;
    }
    if (!require_space_only_gap(parser, &ta.end_tok, &parser->current_token, k_type_name_gap_error)) {
        return NULL;
    }
    char *name = parser->current_token.literal;
    advance(parser);
    return expr_typed_ident(ta.type, ta.base, name, ta.schema_expr, type_tok.line, type_tok.column);
}

static char *build_dotted_ident(Parser *parser, Token first) {
    char *name = NULL;
    size_t len0 = first.literal ? strlen(first.literal) : 0;
    name = malloc(len0 + 1);
    if (!name) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    if (len0) {
        memcpy(name, first.literal, len0 + 1);
    } else {
        name[0] = '\0';
    }
    while (parser->current_token.type == TOKEN_DOT && parser->next_token.type == TOKEN_IDENT) {
        advance(parser); // consume DOT
        const char *part = parser->current_token.literal ? parser->current_token.literal : "";
        size_t part_len = strlen(part);
        size_t cur_len = strlen(name);
        size_t newlen = cur_len + 1 + part_len + 1;
        char *tmp = realloc(name, newlen);
        if (!tmp) {
            free(name);
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        name = tmp;
        name[cur_len] = '.';
        if (part_len) {
            memcpy(name + cur_len + 1, part, part_len);
        }
        name[cur_len + 1 + part_len] = '\0';
        advance(parser); // consume IDENT
    }
    return name;
}

static Expr *parse_primary(Parser *parser) {
    Token token = parser->current_token;
    // Recognize float literal names `INF` and `NaN` as primary expressions
    if (parser->current_token.type == TOKEN_IDENT) {
        if (strcmp(parser->current_token.literal, "true") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_bool(true, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "false") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_bool(false, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "INF") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_float(INFINITY, 0, 1, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "NaN") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_float(NAN, 0, 1, t.line, t.column);
        }
    }
    // Support negative INF written as `-INF` (but disallow `-NaN`)
    if (parser->current_token.type == TOKEN_DASH && parser->next_token.type == TOKEN_IDENT) {
        if (strcmp(parser->next_token.literal, "INF") == 0) {
            Token dash = parser->current_token;
            advance(parser); // consume '-'
            advance(parser); // consume 'INF'
            return expr_float(-INFINITY, 0, 1, dash.line, dash.column);
        }
        if (strcmp(parser->next_token.literal, "NaN") == 0) {
            report_error(parser, "NaN must not be negative");
            return NULL;
        }
    }
    if (parser->current_token.type == TOKEN_ASYNC) {
        Token kw = parser->current_token;
        advance(parser);
        Stmt *block = parse_block(parser);
        return expr_async(block, kw.line, kw.column);
    }
    if (match(parser, TOKEN_NUMBER)) {
        int64_t iv = 0;
        int base = 2;
        if (!parse_prefixed_int_literal(token.literal, &iv, &base)) {
            report_error(parser, "Invalid int literal");
            return NULL;
        }
        return expr_int(iv, base, token.line, token.column);
    }
    if (match(parser, TOKEN_FLOAT)) {
        double fv = 0.0;
        int base = 2;
        if (!parse_prefixed_float_literal(token.literal, &fv, &base)) {
            report_error(parser, "Invalid float literal");
            return NULL;
        }
        return expr_float(fv, base, 0, token.line, token.column);
    }
    if (match(parser, TOKEN_STRING)) {
        return expr_str(token.literal, token.line, token.column);
    }
    if (match(parser, TOKEN_FSTRING)) {
        ExprList parts;
        parts.items = NULL;
        parts.count = 0;
        parts.capacity = 0;
        expr_list_add(&parts, expr_str(token.literal, token.line, token.column));

        while (match(parser, TOKEN_FMT_OPEN)) {
            Expr *expr = parse_expression(parser);
            if (!expr) {
                return NULL;
            }
            if (!consume(parser, TOKEN_FMT_CLOSE, "Expected '}' in formatted string")) {
                return NULL;
            }
            expr_list_add(&parts, expr);

            if (match(parser, TOKEN_FSTRING)) {
                expr_list_add(&parts, expr_str(parser->previous_token.literal, parser->previous_token.line,
                                               parser->previous_token.column));
            } else {
                expr_list_add(&parts, expr_str("", parser->current_token.line, parser->current_token.column));
            }
        }
        return expr_fmt_str(parts, token.line, token.column);
    }
    if (match(parser, TOKEN_AT)) {
        if (parser->current_token.type != TOKEN_IDENT) {
            report_error(parser, "Expected identifier after '@'");
            return NULL;
        }
        Token id = parser->current_token;
        advance(parser);
        return expr_ptr(id.literal, id.line, id.column);
    }
    if (match(parser, TOKEN_LAMBDA)) {
        Token lambda_tok = token;
        /* LAMBDA R ( params ) { body }  -- also accept legacy 'R: (' */
        if (!is_type_token(parser->current_token.type)) {
            report_error(parser, "Expected return type after LAMBDA");
            return NULL;
        }
        TypeAnnotation ret = parse_type_annotation(parser);
        if (ret.type == TYPE_UNKNOWN) {
            return NULL;
        }

        /* Enforce the spec-compliant space-separated form `int (` and
           explicitly reject the legacy colon form `int: (`. Use the same
           space-only gap rules as for named function declarations so
           that line-continuations and comments are handled consistently. */
        if (!require_space_only_gap(parser, &ret.end_tok, &parser->current_token, k_type_name_gap_error)) {
            return NULL;
        }

        consume(parser, TOKEN_LPAREN, "Expected '(' after LAMBDA parameter list");

        ParamList params = {0};
        if (!parse_param_list(parser, &params)) {
            return NULL;
        }

        consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");
        Stmt *body = parse_block(parser);
        return expr_lambda(params, ret.type, ret.base, ret.schema_expr, body, lambda_tok.line, lambda_tok.column);
    }
    if (parser->current_token.type == TOKEN_IDENT) {
        Token idtok = parser->current_token;
        advance(parser); // consume first IDENT
        char *name = build_dotted_ident(parser, idtok);
        return expr_ident(name, idtok.line, idtok.column);
    }
    if (match(parser, TOKEN_LPAREN)) {
        Expr *expr = parse_expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }
    if (match(parser, TOKEN_LBRACKET)) {
        Token lb = parser->previous_token; // the '[' token
        Expr *tns = expr_tensor(lb.line, lb.column);
        if (parser->current_token.type == TOKEN_RBRACKET) {
            report_error(parser, "Empty tensor literal is not allowed");
            return NULL;
        }
        do {
            Expr *item = parse_expression(parser);
            if (!item) {
                return NULL;
            }
            expr_list_add(&tns->as.tns_items, item);
        } while (match(parser, TOKEN_COMMA));
        consume(parser, TOKEN_RBRACKET, "Expected ']' after tensor literal");
        return tns;
    }
    if (match(parser, TOKEN_LANGLE)) {
        Token lb = parser->previous_token; // the '<' token
        Expr *mp = expr_map(lb.line, lb.column);
        if (parser->current_token.type == TOKEN_RANGLE) {
            // Allow empty map literal: consume '>' and return empty map
            advance(parser);
            return mp;
        }
        do {
            // parse key
            Expr *key = parse_expression(parser);
            if (!key) {
                return NULL;
            }
            if (!match(parser, TOKEN_EQUALS)) {
                report_error(parser, "Expected '=' in map literal");
                return NULL;
            }
            Expr *val = parse_expression(parser);
            if (!val) {
                return NULL;
            }
            expr_list_add(&mp->as.map_items.keys, key);
            expr_list_add(&mp->as.map_items.values, val);
        } while (match(parser, TOKEN_COMMA));
        consume(parser, TOKEN_RANGLE, "Expected '>' after map literal");
        return mp;
    }
    report_error(parser, "Expected expression");
    return NULL;
}

static Expr *parse_call(Parser *parser) {
    Expr *expr = parse_primary(parser);
    if (!expr) {
        return NULL;
    }
    while (parser->current_token.type == TOKEN_LPAREN || parser->current_token.type == TOKEN_LBRACKET ||
           parser->current_token.type == TOKEN_LANGLE) {
        if (parser->current_token.type == TOKEN_LPAREN) {
            int line = parser->current_token.line;
            int column = parser->current_token.column;
            advance(parser); // consume '('
            Expr *call = expr_call(expr, line, column);
            if (parser->current_token.type != TOKEN_RPAREN) {
                bool seen_kw = false;
                do {
                    bool is_typed_assign_target =
                        (call->as.call.callee->type == EXPR_IDENT &&
                         strcmp(call->as.call.callee->as.ident, "assign") == 0 && call->as.call.args.count == 0 &&
                         call->as.call.kw_count == 0 && is_type_token(parser->current_token.type) &&
                         (parser->next_token.type == TOKEN_IDENT || parser->next_token.type == TOKEN_COLON ||
                          parser->next_token.type == TOKEN_LBRACE)) != 0;

                    if (is_typed_assign_target) {
                        Expr *arg = parse_typed_ident_expr(parser);
                        if (!arg) {
                            return NULL;
                        }
                        expr_list_add(&call->as.call.args, arg);
                    } else if (parser->current_token.type == TOKEN_IDENT && parser->next_token.type == TOKEN_EQUALS) {
                        // Keyword arg form: IDENT '=' expr
                        seen_kw = true;
                        char *name = parser->current_token.literal;
                        advance(parser); // consume IDENT
                        consume(parser, TOKEN_EQUALS, "Expected '=' after keyword name");
                        Expr *val = parse_expression(parser);
                        if (!val) {
                            return NULL;
                        }
                        call_kw_add(call, name, val);
                    } else {
                        if (seen_kw) {
                            report_error(parser, "Positional arguments cannot follow keyword arguments");
                            return NULL;
                        }
                        Expr *arg = parse_expression(parser);
                        if (!arg) {
                            return NULL;
                        }
                        expr_list_add(&call->as.call.args, arg);
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            expr = call;
            continue;
        }

        expr = parse_index_suffix(parser, expr);
        if (!expr) {
            return NULL;
        }
    }
    return expr;
}

static Expr *parse_expression(Parser *parser) { return parse_call(parser); }

static Expr *parse_index_suffix(Parser *parser, Expr *base) {
    while (parser->current_token.type == TOKEN_LBRACKET || parser->current_token.type == TOKEN_LANGLE) {
        if (parser->current_token.type == TOKEN_LBRACKET) {
            int line = parser->current_token.line;
            int column = parser->current_token.column;
            advance(parser); // consume '['
            Expr *idx = expr_index(base, line, column, false);
            if (parser->current_token.type == TOKEN_RBRACKET) {
                report_error(parser, "Empty index list");
                return NULL;
            }
            while (parser->current_token.type != TOKEN_RBRACKET && parser->current_token.type != TOKEN_EOF) {
                if (match(parser, TOKEN_HASH)) {
                    Expr *wc = expr_wildcard(parser->previous_token.line, parser->previous_token.column);
                    expr_list_add(&idx->as.index.indices, wc);
                } else {
                    Expr *start = parse_expression(parser);
                    if (!start) {
                        return NULL;
                    }
                    if (parser->current_token.type == TOKEN_COLON) {
                        advance(parser); // consume ':'
                        Expr *end = parse_expression(parser);
                        if (!end) {
                            return NULL;
                        }
                        Expr *range = expr_range(start, end, start->line, start->column);
                        expr_list_add(&idx->as.index.indices, range);
                    } else {
                        expr_list_add(&idx->as.index.indices, start);
                    }
                }
                if (parser->current_token.type == TOKEN_COMMA) {
                    advance(parser);
                    continue;
                }
                break;
            }
            consume(parser, TOKEN_RBRACKET, "Expected ']' after index list");
            base = idx;
            continue;
        }

        int line = parser->current_token.line;
        int column = parser->current_token.column;
        advance(parser); // consume '<'
        Expr *idx = expr_index(base, line, column, true);
        if (parser->current_token.type == TOKEN_RANGLE) {
            report_error(parser, "Empty index list");
            return NULL;
        }
        while (parser->current_token.type != TOKEN_RANGLE && parser->current_token.type != TOKEN_EOF) {
            Expr *key = parse_expression(parser);
            if (!key) {
                return NULL;
            }
            expr_list_add(&idx->as.index.indices, key);
            if (parser->current_token.type == TOKEN_COMMA) {
                advance(parser);
                continue;
            }
            break;
        }
        consume(parser, TOKEN_RANGLE, "Expected '>' after index list");
        base = idx;
    }
    return base;
}

static Stmt *parse_block(Parser *parser) {
    Token brace = parser->current_token;
    consume(parser, TOKEN_LBRACE, "Expected '{'");
    Stmt *block = stmt_block(brace.line, brace.column);
    skip_newlines(parser);
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        Stmt *stmt = parse_statement(parser);
        if (stmt) {
            char *line_text = lexer_get_line(parser->lexer, stmt->line);
            stmt_set_src(stmt, line_text);
            free(line_text);
            stmt_list_add(&block->as.block, stmt);
            skip_newlines(parser);
            continue;
        }

        if (!parser->error_msg) {
            break;
        }

        append_parse_error_throw_stmt(parser, block);

        while (parser->current_token.type != TOKEN_EOF && parser->current_token.type != TOKEN_NEWLINE &&
               parser->current_token.type != TOKEN_RBRACE) {
            advance(parser);
        }
        skip_newlines(parser);
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after block");
    return block;
}

static Stmt *parse_if(Parser *parser) {
    Token if_tok = parser->current_token;
    consume(parser, TOKEN_IF, "Expected 'if'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after if");
    Expr *cond = parse_expression(parser);
    if (!cond) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    Stmt *then_block = parse_block(parser);
    Stmt *stmt = stmt_if(cond, then_block, if_tok.line, if_tok.column);

    while (parser->current_token.type == TOKEN_ELSEIF) {
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after elseif");
        Expr *elif_cond = parse_expression(parser);
        if (!elif_cond) {
            return NULL;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
        Stmt *elif_block = parse_block(parser);
        expr_list_add(&stmt->as.if_stmt.elif_conditions, elif_cond);
        stmt_list_add(&stmt->as.if_stmt.elif_blocks, elif_block);
    }

    if (parser->current_token.type == TOKEN_ELSE) {
        advance(parser);
        Stmt *else_block = parse_block(parser);
        stmt->as.if_stmt.else_branch = else_block;
    }
    return stmt;
}

static Stmt *parse_while(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_WHILE, "Expected 'while'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after while");
    Expr *cond = parse_expression(parser);
    if (!cond) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    Stmt *body = parse_block(parser);
    return stmt_while(cond, body, tok.line, tok.column);
}

static Stmt *parse_for(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_FOR, "Expected 'for'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after for");
    if (parser->current_token.type != TOKEN_IDENT) {
        report_error(parser, "Expected counter identifier");
        return NULL;
    }
    char *counter = parser->current_token.literal;
    advance(parser);
    consume(parser, TOKEN_COMMA, "Expected ',' after counter");
    Expr *target = parse_expression(parser);
    if (!target) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after for");
    Stmt *body = parse_block(parser);
    return stmt_for(counter, target, body, tok.line, tok.column);
}

static Stmt *parse_parfor(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_PARFOR, "Expected 'parfor'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after parfor");
    if (parser->current_token.type != TOKEN_IDENT) {
        report_error(parser, "Expected counter identifier");
        return NULL;
    }
    char *counter = parser->current_token.literal;
    advance(parser);
    consume(parser, TOKEN_COMMA, "Expected ',' after counter");
    Expr *target = parse_expression(parser);
    if (!target) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after parfor");
    Stmt *body = parse_block(parser);
    return stmt_parfor(counter, target, body, tok.line, tok.column);
}

static Stmt *parse_try(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_TRY, "Expected 'try'");
    Stmt *try_block = parse_block(parser);
    if (parser->current_token.type != TOKEN_CATCH) {
        report_error(parser, "Expected 'catch' after try");
        while (parser->current_token.type != TOKEN_EOF && parser->current_token.type != TOKEN_RBRACE) {
            advance(parser);
        }
        return NULL;
    }
    advance(parser);
    char *catch_name = NULL;
    if (match(parser, TOKEN_LPAREN)) {
        if (parser->current_token.type == TOKEN_IDENT) {
            catch_name = parser->current_token.literal;
            advance(parser);
        } else {
            report_error(parser, "Expected identifier in catch");
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after catch");
    }
    Stmt *catch_block = parse_block(parser);
    return stmt_try(try_block, catch_name, catch_block, tok.line, tok.column);
}

static Stmt *parse_func(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_FUNC, "Expected 'func'");
    /* func R name( params ) { body } */
    if (!is_type_token(parser->current_token.type)) {
        report_error(parser, "Expected return type after func");
        return NULL;
    }
    TypeAnnotation ret = parse_type_annotation(parser);
    if (ret.type == TYPE_UNKNOWN) {
        return NULL;
    }
    if (!require_space_only_gap(parser, &ret.end_tok, &parser->current_token, k_type_name_gap_error)) {
        return NULL;
    }
    char *name = parser->current_token.literal;
    advance(parser);
    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    ParamList params = {0};
    if (!parse_param_list(parser, &params)) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");
    Stmt *body = parse_block(parser);
    Stmt *stmt = stmt_func(name, ret.type, ret.base, ret.schema_expr, body, tok.line, tok.column);
    stmt->as.func_stmt.params = params;
    return stmt;
}

static Stmt *parse_simple_typed_declaration(Parser *parser, Token type_tok, TypeAnnotation ta) {
    if (!require_space_only_gap(parser, &ta.end_tok, &parser->current_token, k_type_name_gap_error)) {
        return NULL;
    }
    char *name = parser->current_token.literal;
    advance(parser);
    if (match(parser, TOKEN_EQUALS)) {
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        return stmt_assign(true, ta.type, ta.base, name, NULL, expr, ta.schema_expr, type_tok.line, type_tok.column);
    }
    return stmt_decl(ta.type, ta.base, name, ta.schema_expr, type_tok.line, type_tok.column);
}

static Stmt *parse_paren_expr_stmt(Parser *parser, Stmt *(*builder)(Expr *, int, int), const char *open_msg,
                                   const char *close_msg) {
    Token tok = parser->current_token;
    advance(parser);
    consume(parser, TOKEN_LPAREN, open_msg);
    Expr *expr = parse_expression(parser);
    if (!expr) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, close_msg);
    return builder(expr, tok.line, tok.column);
}

static Stmt *parse_statement(Parser *parser) {
    skip_newlines(parser);
    if (looks_like_func_definition(parser)) {
        return parse_func(parser);
    }

    // Handle typed declarations where the type token may be a keyword like thread.
    if (starts_named_type_annotation(parser)) {
        Token type_tok = parser->current_token;
        TypeAnnotation ta = parse_type_annotation(parser);
        if (ta.type == TYPE_UNKNOWN) {
            return NULL;
        }
        if (!require_space_only_gap(parser, &ta.end_tok, &parser->current_token, k_type_name_gap_error)) {
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        // Support typed declaration with indexed-assignment target, e.g. `tensor: t[1-10] = ...`
        if (parser->current_token.type == TOKEN_LBRACKET || parser->current_token.type == TOKEN_LANGLE) {
            Expr *base = expr_ident(name, type_tok.line, type_tok.column);
            base = parse_index_suffix(parser, base);
            if (!base) {
                return NULL;
            }
            if (match(parser, TOKEN_EQUALS)) {
                Expr *expr = parse_expression(parser);
                if (!expr) {
                    return NULL;
                }
                return stmt_assign(true, ta.type, ta.base, NULL, base, expr, ta.schema_expr, type_tok.line,
                                   type_tok.column);
            }
            report_error(parser, "Expected '=' after typed indexed target");
            return NULL;
        }

        if (match(parser, TOKEN_EQUALS)) {
            Expr *expr = parse_expression(parser);
            if (!expr) {
                return NULL;
            }
            return stmt_assign(true, ta.type, ta.base, name, NULL, expr, ta.schema_expr, type_tok.line,
                               type_tok.column);
        }
        return stmt_decl(ta.type, ta.base, name, ta.schema_expr, type_tok.line, type_tok.column);
    }
    switch (parser->current_token.type) {
    case TOKEN_IF:
        return parse_if(parser);
    case TOKEN_WHILE:
        return parse_while(parser);
    case TOKEN_FOR:
        return parse_for(parser);
    case TOKEN_PARFOR:
        return parse_parfor(parser);
    case TOKEN_TRY:
        return parse_try(parser);
    case TOKEN_FUNC:
        return parse_func(parser);
    case TOKEN_RETURN:
        return parse_paren_expr_stmt(parser, stmt_return, "Expected '(' after return",
                                     "Expected ')' after return value");
    case TOKEN_POP:
        return parse_paren_expr_stmt(parser, stmt_pop, "Expected '(' after pop", "Expected ')' after pop expression");
    case TOKEN_BREAK:
        return parse_paren_expr_stmt(parser, stmt_break, "Expected '(' after break", "Expected ')' after break value");
    case TOKEN_THREAD: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after thread");
        if (parser->current_token.type != TOKEN_IDENT) {
            report_error(parser, "Expected identifier after thread(");
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after thread identifier");
        Stmt *body = parse_block(parser);
        return stmt_thread(name, body, tok.line, tok.column);
    }
    case TOKEN_ASYNC: {
        Token tok = parser->current_token;
        advance(parser);
        Stmt *body = parse_block(parser);
        return stmt_async(body, tok.line, tok.column);
    }
    case TOKEN_CONTINUE: {
        Token tok = parser->current_token;
        advance(parser);
        if (!consume(parser, TOKEN_LPAREN, "Expected '(' after continue")) {
            return stmt_continue(tok.line, tok.column);
        }

        /* If the next token is a real expression-start token, report
         * that continue does not accept arguments and skip until the
         * closing ')'. If the token is something like '}' or EOF,
         * prefer the missing-')' message so the user sees the
         * syntactically relevant error.
         */
        PTokenType t = parser->current_token.type;
        bool looks_like_expr = (t == TOKEN_NUMBER || t == TOKEN_FLOAT || t == TOKEN_STRING || t == TOKEN_IDENT ||
                                t == TOKEN_AT || t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LANGLE ||
                                t == TOKEN_ASYNC || t == TOKEN_LAMBDA || t == TOKEN_DASH);

        if (looks_like_expr) {
            report_error(parser, "continue does not accept arguments");
            while (parser->current_token.type != TOKEN_RPAREN && parser->current_token.type != TOKEN_EOF) {
                advance(parser);
            }
        }

        /* Require the closing paren; this will produce the standard
         * "Expected ')' after continue" message when appropriate.
         */
        consume(parser, TOKEN_RPAREN, "Expected ')' after continue");
        return stmt_continue(tok.line, tok.column);
    }
    case TOKEN_GOTO:
        return parse_paren_expr_stmt(parser, stmt_goto, "Expected '(' after goto", "Expected ')' after goto");
    case TOKEN_GOTOPOINT:
        return parse_paren_expr_stmt(parser, stmt_gotopoint, "Expected '(' after gotopoint",
                                     "Expected ')' after gotopoint target");
    default:
        break;
    }

    if (starts_named_type_annotation(parser)) {
        Token type_tok = parser->current_token;
        TypeAnnotation ta = parse_type_annotation(parser);
        if (ta.type == TYPE_UNKNOWN) {
            return NULL;
        }
        return parse_simple_typed_declaration(parser, type_tok, ta);
    }

    if (parser->current_token.type == TOKEN_IDENT && parser->next_token.type == TOKEN_EQUALS) {
        Token name_tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_EQUALS, "Expected '=' after identifier");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        return stmt_assign(false, TYPE_UNKNOWN, 0, name_tok.literal, NULL, expr, NULL, name_tok.line, name_tok.column);
    }

    Expr *expr = parse_expression(parser);
    if (!expr) {
        return NULL;
    }

    // Support assignment to an expression LHS (e.g., indexed assignment): expr '=' rhs
    if (parser->current_token.type == TOKEN_EQUALS) {
        // consume '=' and parse RHS
        advance(parser);
        Expr *rhs = parse_expression(parser);
        if (!rhs) {
            return NULL;
        }
        // Create an assign stmt with the expression as target
        return stmt_assign(false, TYPE_UNKNOWN, 0, NULL, expr, rhs, NULL, expr->line, expr->column);
    }

    return stmt_expr(expr, expr->line, expr->column);
}

Stmt *parser_parse(Parser *parser) {
    Stmt *program = stmt_block(parser->current_token.line, parser->current_token.column);
    skip_newlines(parser);
    while (parser->current_token.type != TOKEN_EOF) {
        Stmt *stmt = parse_statement(parser);
        if (stmt) {
            /* Enforce specification: top-level expression statements and
             * assignments must occupy their own logical line. If we parsed
             * such a statement but the next token is not a logical newline
             * (TOKEN_NEWLINE) or EOF, report a parse error and synthesize
             * a throw so runtime try/catch can observe it. Then
             * synchronize to the next logical newline. */
            if ((stmt->type == STMT_EXPR || stmt->type == STMT_ASSIGN) && parser->current_token.type != TOKEN_NEWLINE &&
                parser->current_token.type != TOKEN_EOF) {
                report_error(parser, "Expression statement or assignment must occupy its own logical line");
                append_parse_error_throw_stmt(parser, program);
                while (parser->current_token.type != TOKEN_EOF && parser->current_token.type != TOKEN_NEWLINE) {
                    advance(parser);
                }
                skip_newlines(parser);
                continue;
            }

            char *line_text = lexer_get_line(parser->lexer, stmt->line);
            stmt_set_src(stmt, line_text);
            free(line_text);
            stmt_list_add(&program->as.block, stmt);
            skip_newlines(parser);
            continue;
        }

        /* If a parse error was recorded, synthesize a runtime throw call
           statement so that runtime try/catch can observe the parse error
           as a catchable runtime exception. Then clear the parser error
           state so callers (e.g. run/import) don't treat it as a fatal
           top-level parse failure. */
        if (parser->error_msg) {
            append_parse_error_throw_stmt(parser, program);
        }

        /* Synchronize after an error: advance to next newline or EOF so the
           parser makes progress instead of repeatedly returning NULL and
           hanging. */
        while (parser->current_token.type != TOKEN_EOF && parser->current_token.type != TOKEN_NEWLINE) {
            advance(parser);
        }
        skip_newlines(parser);
    }
    return program;
}
