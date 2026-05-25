#include "parser.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report_error(Parser *parser, const char *message) {
    if (parser->panic_mode) {
        return;
    }
    parser->panic_mode = true;
    parser->had_error = true;
    /* Record last error for possible conversion into a runtime THROW
       so TRY/CATCH can handle parse-time errors that occur inside
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

static void advance(Parser *parser) {
    parser->previous_token = parser->current_token;
    parser->current_token = parser->next_token;
    parser->next_token = parser->lookahead2_token;
    parser->lookahead2_token = lexer_next_token(parser->lexer);

    /* If the lexer produced an error token, report it and advance until
       we reach a non-error token. Use a loop instead of recursion so the
       parser doesn't overflow the stack or hang when many consecutive
       error tokens are emitted. */
    while (parser->current_token.type == TOKEN_ERROR) {
        report_error(parser, parser->current_token.literal);
        parser->panic_mode = false;

        parser->previous_token = parser->current_token;
        parser->current_token = parser->next_token;
        parser->next_token = parser->lookahead2_token;
        parser->lookahead2_token = lexer_next_token(parser->lexer);
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
    if (strcmp(name, "BOOL") == 0) {
        return TYPE_BOOL;
    }
    if (strcmp(name, "INT") == 0) {
        return TYPE_INT;
    }
    if (strcmp(name, "FLT") == 0) {
        return TYPE_FLT;
    }
    if (strcmp(name, "STR") == 0) {
        return TYPE_STR;
    }
    if (strcmp(name, "MAP") == 0) {
        return TYPE_MAP;
    }
    if (strcmp(name, "FUNC") == 0) {
        return TYPE_FUNC;
    }
    if (strcmp(name, "THR") == 0) {
        return TYPE_THR;
    }
    if (strcmp(name, "TNS") == 0) {
        return TYPE_TNS;
    }
    return TYPE_UNKNOWN;
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
    case TOKEN_THR:
        return 3;
    default:
        return 0;
    }
}

static bool require_space_only_gap(Parser *parser, const Token *left, const Token *right, const char *message) {
    char *line_text;
    size_t line_len;
    size_t left_width;
    int gap_start_col;
    int gap_end_col;

    if (!left || !right || right->type != TOKEN_IDENT) {
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

        /* Walk the raw source between the two token offsets and accept only
           spaces and valid caret-continuation sequences. */
        size_t pos = left_end_offset;
        while (pos < right_start_offset) {
            char ch = lexer->source[pos];
            if (ch == ' ') {
                pos++;
                continue;
            }

            if (ch == '^') {
                /* Must match the same rules as the lexer: '^' followed by LF,
                   CR (optionally CRLF), or '!' (a comment) is a valid
                   continuation. Anything else is invalid here. */
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
                    /* Skip '!' and the comment text until the line terminator. */
                    pos += 2;
                    while (pos < lexer->source_len && lexer->source[pos] != '\n' && lexer->source[pos] != '\r') {
                        pos++;
                    }
                    if (pos < lexer->source_len) {
                        if (lexer->source[pos] == '\r') {
                            pos++;
                            if (pos < lexer->source_len && lexer->source[pos] == '\n') {
                                pos++;
                            }
                        } else if (lexer->source[pos] == '\n') {
                            {
                                pos++;
                            }
                        }
                    }
                    continue;
                }

                report_error(parser, message);
                return false;
            }

            /* Any other character (including tabs or plain newlines) is invalid
               as a gap between type and name. */
            report_error(parser, message);
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

static bool advance_to_annotated_name(Parser *parser, const char *message) {
    if (!require_space_only_gap(parser, &parser->current_token, &parser->next_token, message)) {
        return false;
    }

    advance(parser);
    return true;
}

static int base_from_literal_prefix(const char *s, size_t *prefix_len) {
    if (!s || s[0] != '0') {
        return -1;
    }
    char p = s[1];
    switch (p) {
    case 'b':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 2;
    case 'o':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 8;
    case 'd':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 10;
    case 'x':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 16;
    case 't':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 32;
    case 'c':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 58;
    case 's':
        if (prefix_len) {
            *prefix_len = 2;
        }
        return 64;
    case 'r': {
        if (!isdigit((unsigned char)s[2]) || !isdigit((unsigned char)s[3])) {
            return -1;
        }
        int b = ((s[2] - '0') * 10) + (s[3] - '0');
        if (prefix_len) {
            *prefix_len = 4;
        }
        return b;
    }
    default:
        return -1;
    }
}

static int digit_value_for_base(int base, char c) {
    const char *digits64 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+_";
    const char *digits58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    const char *alphabet = (base == 58) ? digits58 : digits64;
    int limit = (base == 58) ? 58 : base;
    if (limit < 0) {
        return -1;
    }
    for (int i = 0; i < limit; i++) {
        if (alphabet[i] == c) {
            return i;
        }
    }
    return -1;
}

static int parse_prefixed_int_literal(const char *lit, int64_t *out_value, int *out_base) {
    if (!lit || !out_value || !out_base) {
        return 0;
    }
    int neg = 0;
    const char *s = lit;
    if (*s == '-') {
        neg = 1;
        s++;
    }

    size_t prefix_len = 0;
    int base = base_from_literal_prefix(s, &prefix_len);
    if (base < 2 || base > 64) {
        return 0;
    }
    const char *digits = s + prefix_len;
    if (*digits == '\0') {
        return 0;
    }
    if (strchr(digits, '.') != NULL) {
        return 0;
    }

    int64_t acc = 0;
    for (const char *p = digits; *p; p++) {
        int dv = digit_value_for_base(base, *p);
        if (dv < 0 || dv >= base) {
            return 0;
        }
        if (acc > (INT64_MAX - dv) / base) {
            return 0;
        }
        acc = (acc * base) + dv;
    }

    *out_value = neg ? -acc : acc;
    *out_base = base;
    return 1;
}

static int parse_prefixed_float_literal(const char *lit, double *out_value, int *out_base) {
    if (!lit || !out_value || !out_base) {
        return 0;
    }
    int neg = 0;
    const char *s = lit;
    if (*s == '-') {
        neg = 1;
        s++;
    }

    size_t prefix_len = 0;
    int base = base_from_literal_prefix(s, &prefix_len);
    if (base < 2 || base > 64) {
        return 0;
    }
    const char *digits = s + prefix_len;
    const char *dot = strchr(digits, '.');
    if (!dot) {
        return 0;
    }
    if (dot == digits || *(dot + 1) == '\0') {
        return 0;
    }

    double int_part = 0.0;
    for (const char *p = digits; p < dot; p++) {
        int dv = digit_value_for_base(base, *p);
        if (dv < 0 || dv >= base) {
            return 0;
        }
        int_part = (int_part * (double)base) + (double)dv;
    }

    double frac_part = 0.0;
    double weight = 1.0 / (double)base;
    for (const char *p = dot + 1; *p; p++) {
        int dv = digit_value_for_base(base, *p);
        if (dv < 0 || dv >= base) {
            return 0;
        }
        frac_part += (double)dv * weight;
        weight /= (double)base;
    }

    double v = int_part + frac_part;
    *out_value = neg ? -v : v;
    *out_base = base;
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
    Expr *callee = expr_ident(strdup("THROW"), parser->error_line, parser->error_col);
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

static bool is_type_token(PTokenType type) {
    return (type == TOKEN_IDENT || type == TOKEN_FUNC || type == TOKEN_THR) != 0;
}

static bool starts_named_type_annotation(Parser *parser) {
    return (is_type_token(parser->current_token.type) &&
            (parser->next_token.type == TOKEN_IDENT || parser->next_token.type == TOKEN_COLON)) != 0;
}

static bool looks_like_func_definition(Parser *parser) {
    return (parser->current_token.type == TOKEN_FUNC && is_type_token(parser->next_token.type) &&
            (parser->lookahead2_token.type == TOKEN_IDENT || parser->lookahead2_token.type == TOKEN_COLON)) != 0;
}

static bool parse_param_list(Parser *parser, ParamList *params) {
    if (parser->current_token.type == TOKEN_RPAREN) {
        return true;
    }
    do {
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
        DeclType ptype = parse_type_name(parser->current_token.literal);
        if (!advance_to_annotated_name(parser, k_type_name_gap_error)) {
            return false;
        }
        Param param;
        param.type = ptype;
        param.name = parser->current_token.literal;
        param.coerced = coerced;
        param.default_value = NULL;
        advance(parser);
        if (match(parser, TOKEN_EQUALS)) {
            param.default_value = parse_expression(parser);
            if (!param.default_value) {
                return false;
            }
        }
        param_list_add(params, param);
    } while (match(parser, TOKEN_COMMA));
    return true;
}

static Expr *parse_typed_ident_expr(Parser *parser) {
    Token type_tok = parser->current_token;
    DeclType dtype = parse_type_name(type_tok.literal);
    if (!advance_to_annotated_name(parser, k_type_name_gap_error)) {
        return NULL;
    }
    char *name = parser->current_token.literal;
    advance(parser);
    return expr_typed_ident(dtype, name, type_tok.line, type_tok.column);
}

static Expr *parse_primary(Parser *parser) {
    Token token = parser->current_token;
    // Recognize FLT literal names `INF` and `NaN` as primary expressions
    if (parser->current_token.type == TOKEN_IDENT) {
        if (strcmp(parser->current_token.literal, "TRUE") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_bool(true, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "FALSE") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_bool(false, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "INF") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_flt(INFINITY, 0, 1, t.line, t.column);
        }
        if (strcmp(parser->current_token.literal, "NaN") == 0) {
            Token t = parser->current_token;
            advance(parser);
            return expr_flt(NAN, 0, 1, t.line, t.column);
        }
    }
    // Support negative INF written as `-INF` (but disallow `-NaN`)
    if (parser->current_token.type == TOKEN_DASH && parser->next_token.type == TOKEN_IDENT) {
        if (strcmp(parser->next_token.literal, "INF") == 0) {
            Token dash = parser->current_token;
            advance(parser); // consume '-'
            advance(parser); // consume 'INF'
            return expr_flt(-INFINITY, 0, 1, dash.line, dash.column);
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
            report_error(parser, "Invalid INT literal");
            return NULL;
        }
        return expr_int(iv, base, token.line, token.column);
    }
    if (match(parser, TOKEN_FLOAT)) {
        double fv = 0.0;
        int base = 2;
        if (!parse_prefixed_float_literal(token.literal, &fv, &base)) {
            report_error(parser, "Invalid FLT literal");
            return NULL;
        }
        return expr_flt(fv, base, 0, token.line, token.column);
    }
    if (match(parser, TOKEN_STRING)) {
        return expr_str(token.literal, token.line, token.column);
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
        /* LAMBDA R: ( params ) { body } */
        if (!is_type_token(parser->current_token.type)) {
            report_error(parser, "Expected return type after LAMBDA");
            return NULL;
        }
        DeclType ret = parse_type_name(parser->current_token.literal);
        advance(parser);
        consume(parser, TOKEN_COLON, "Expected ':' after return type");
        consume(parser, TOKEN_LPAREN, "Expected '(' after LAMBDA parameter list");

        ParamList params = {0};
        if (!parse_param_list(parser, &params)) {
            return NULL;
        }

        consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");
        Stmt *body = parse_block(parser);
        return expr_lambda(params, ret, body, lambda_tok.line, lambda_tok.column);
    }
    if (parser->current_token.type == TOKEN_IDENT) {
        Token idtok = parser->current_token;
        // Build possibly-dotted identifier by concatenating IDENT (DOT IDENT)*
        size_t len0 = idtok.literal ? strlen(idtok.literal) : 0;
        char *name = malloc(len0 + 1);
        if (!name) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        if (len0) {
            memcpy(name, idtok.literal, len0 + 1);
        } else {
            name[0] = '\0';
        }
        advance(parser); // consume first IDENT
        while (parser->current_token.type == TOKEN_DOT && parser->next_token.type == TOKEN_IDENT) {
            advance(parser); // consume DOT
            // append '.' + next ident
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
        return expr_ident(name, idtok.line, idtok.column);
    }
    if (match(parser, TOKEN_LPAREN)) {
        Expr *expr = parse_expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }
    if (match(parser, TOKEN_LBRACKET)) {
        Token lb = parser->previous_token; // the '[' token
        Expr *tns = expr_tns(lb.line, lb.column);
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
                         strcmp(call->as.call.callee->as.ident, "ASSIGN") == 0 && call->as.call.args.count == 0 &&
                         call->as.call.kw_count == 0 && is_type_token(parser->current_token.type) &&
                         (parser->next_token.type == TOKEN_IDENT || parser->next_token.type == TOKEN_COLON)) != 0;

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

        // Handle indexing: '['
        if (parser->current_token.type == TOKEN_LBRACKET) {
            int line = parser->current_token.line;
            int column = parser->current_token.column;
            advance(parser); // consume '['
            Expr *idx = expr_index(expr, line, column, false);
            if (parser->current_token.type == TOKEN_RBRACKET) {
                report_error(parser, "Empty index list");
                return NULL;
            }
            while (parser->current_token.type != TOKEN_RBRACKET && parser->current_token.type != TOKEN_EOF) {
                // wildcard
                if (match(parser, TOKEN_HASH)) {
                    Expr *wc = expr_wildcard(parser->previous_token.line, parser->previous_token.column);
                    expr_list_add(&idx->as.index.indices, wc);
                } else {
                    // parse an expression for index or possibly a range
                    Expr *start = parse_expression(parser);
                    if (!start) {
                        return NULL;
                    }
                    // Range separator is ':' inside index expressions
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
            expr = idx;
            continue;
        }

        // Handle angle-bracket indexing for maps: '<' ... '>'
        if (parser->current_token.type == TOKEN_LANGLE) {
            int line = parser->current_token.line;
            int column = parser->current_token.column;
            advance(parser); // consume '<'
            Expr *idx = expr_index(expr, line, column, true);
            if (parser->current_token.type == TOKEN_RANGLE) {
                report_error(parser, "Empty index list");
                return NULL;
            }
            while (parser->current_token.type != TOKEN_RANGLE && parser->current_token.type != TOKEN_EOF) {
                Expr *start = parse_expression(parser);
                if (!start) {
                    return NULL;
                }
                expr_list_add(&idx->as.index.indices, start);

                if (parser->current_token.type == TOKEN_COMMA) {
                    advance(parser);
                    continue;
                }
                break;
            }
            consume(parser, TOKEN_RANGLE, "Expected '>' after index list");
            expr = idx;
            continue;
        }
    }
    return expr;
}

static Expr *parse_expression(Parser *parser) { return parse_call(parser); }

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
    consume(parser, TOKEN_IF, "Expected 'IF'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after IF");
    Expr *cond = parse_expression(parser);
    if (!cond) {
        return NULL;
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    Stmt *then_block = parse_block(parser);
    Stmt *stmt = stmt_if(cond, then_block, if_tok.line, if_tok.column);

    while (parser->current_token.type == TOKEN_ELSEIF) {
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after ELSEIF");
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
    consume(parser, TOKEN_WHILE, "Expected 'WHILE'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after WHILE");
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
    consume(parser, TOKEN_FOR, "Expected 'FOR'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after FOR");
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
    consume(parser, TOKEN_RPAREN, "Expected ')' after FOR");
    Stmt *body = parse_block(parser);
    return stmt_for(counter, target, body, tok.line, tok.column);
}

static Stmt *parse_parfor(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_PARFOR, "Expected 'PARFOR'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after PARFOR");
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
    consume(parser, TOKEN_RPAREN, "Expected ')' after PARFOR");
    Stmt *body = parse_block(parser);
    return stmt_parfor(counter, target, body, tok.line, tok.column);
}

static Stmt *parse_try(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_TRY, "Expected 'TRY'");
    Stmt *try_block = parse_block(parser);
    if (parser->current_token.type != TOKEN_CATCH) {
        report_error(parser, "Expected 'CATCH' after TRY");
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
            report_error(parser, "Expected identifier in CATCH");
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after CATCH");
    }
    Stmt *catch_block = parse_block(parser);
    return stmt_try(try_block, catch_name, catch_block, tok.line, tok.column);
}

static Stmt *parse_func(Parser *parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_FUNC, "Expected 'FUNC'");
    /* FUNC R name( params ) { body } */
    if (!is_type_token(parser->current_token.type)) {
        report_error(parser, "Expected return type after FUNC");
        return NULL;
    }
    DeclType ret = parse_type_name(parser->current_token.literal);
    if (!advance_to_annotated_name(parser, k_type_name_gap_error)) {
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
    Stmt *stmt = stmt_func(name, ret, body, tok.line, tok.column);
    stmt->as.func_stmt.params = params;
    return stmt;
}

static Stmt *parse_statement(Parser *parser) {
    skip_newlines(parser);
    if (looks_like_func_definition(parser)) {
        return parse_func(parser);
    }

    // Handle typed declarations where the type token may be a keyword like THR.
    if (starts_named_type_annotation(parser)) {
        Token type_tok = parser->current_token;
        DeclType dtype = parse_type_name(type_tok.literal);
        if (!advance_to_annotated_name(parser, k_type_name_gap_error)) {
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        // Support typed declaration with indexed-assignment target, e.g. `TNS: t[1-10] = ...`
        if (parser->current_token.type == TOKEN_LBRACKET || parser->current_token.type == TOKEN_LANGLE) {
            // construct base identifier expr and parse trailing indexers
            Expr *base = expr_ident(name, type_tok.line, type_tok.column);
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

                // angle-bracket indexing for maps
                if (parser->current_token.type == TOKEN_LANGLE) {
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
                    continue;
                }
            }

            if (match(parser, TOKEN_EQUALS)) {
                Expr *expr = parse_expression(parser);
                if (!expr) {
                    return NULL;
                }
                return stmt_assign(true, dtype, NULL, base, expr, type_tok.line, type_tok.column);
            }
            report_error(parser, "Expected '=' after typed indexed target");
            return NULL;
        }

        if (match(parser, TOKEN_EQUALS)) {
            Expr *expr = parse_expression(parser);
            if (!expr) {
                return NULL;
            }
            return stmt_assign(true, dtype, name, NULL, expr, type_tok.line, type_tok.column);
        }
        return stmt_decl(dtype, name, type_tok.line, type_tok.column);
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
    case TOKEN_RETURN: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after RETURN");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after RETURN value");
        return stmt_return(expr, tok.line, tok.column);
    }
    case TOKEN_POP: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after POP");
        if (parser->current_token.type != TOKEN_IDENT) {
            report_error(parser, "POP expects an identifier");
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after POP identifier");
        return stmt_pop(name, tok.line, tok.column);
    }
    case TOKEN_BREAK: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after BREAK");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after BREAK value");
        return stmt_break(expr, tok.line, tok.column);
    }
    case TOKEN_THR: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after THR");
        if (parser->current_token.type != TOKEN_IDENT) {
            report_error(parser, "Expected identifier after THR(");
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after THR identifier");
        Stmt *body = parse_block(parser);
        return stmt_thr(name, body, tok.line, tok.column);
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
        if (!consume(parser, TOKEN_LPAREN, "Expected '(' after CONTINUE")) {
            return stmt_continue(tok.line, tok.column);
        }

        /* If the next token is a real expression-start token, report
         * that CONTINUE does not accept arguments and skip until the
         * closing ')'. If the token is something like '}' or EOF,
         * prefer the missing-')' message so the user sees the
         * syntactically relevant error.
         */
        PTokenType t = parser->current_token.type;
        bool looks_like_expr = (t == TOKEN_NUMBER || t == TOKEN_FLOAT || t == TOKEN_STRING || t == TOKEN_IDENT ||
                                t == TOKEN_AT || t == TOKEN_LPAREN || t == TOKEN_LBRACKET || t == TOKEN_LANGLE ||
                                t == TOKEN_ASYNC || t == TOKEN_LAMBDA || t == TOKEN_DASH) != 0;

        if (looks_like_expr) {
            report_error(parser, "CONTINUE does not accept arguments");
            while (parser->current_token.type != TOKEN_RPAREN && parser->current_token.type != TOKEN_EOF) {
                advance(parser);
            }
        }

        /* Require the closing paren; this will produce the standard
         * "Expected ')' after CONTINUE" message when appropriate.
         */
        consume(parser, TOKEN_RPAREN, "Expected ')' after CONTINUE");
        return stmt_continue(tok.line, tok.column);
    }
    case TOKEN_GOTO: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after GOTO");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after GOTO");
        return stmt_goto(expr, tok.line, tok.column);
    }
    case TOKEN_GOTOPOINT: {
        Token tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after GOTOPOINT");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after GOTOPOINT target");
        return stmt_gotopoint(expr, tok.line, tok.column);
    }
    default:
        break;
    }

    if (starts_named_type_annotation(parser)) {
        Token type_tok = parser->current_token;
        DeclType dtype = parse_type_name(type_tok.literal);
        if (!advance_to_annotated_name(parser, k_type_name_gap_error)) {
            return NULL;
        }
        char *name = parser->current_token.literal;
        advance(parser);
        if (match(parser, TOKEN_EQUALS)) {
            Expr *expr = parse_expression(parser);
            if (!expr) {
                return NULL;
            }
            return stmt_assign(true, dtype, name, NULL, expr, type_tok.line, type_tok.column);
        }
        return stmt_decl(dtype, name, type_tok.line, type_tok.column);
    }

    if (parser->current_token.type == TOKEN_IDENT && parser->next_token.type == TOKEN_EQUALS) {
        Token name_tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_EQUALS, "Expected '=' after identifier");
        Expr *expr = parse_expression(parser);
        if (!expr) {
            return NULL;
        }
        return stmt_assign(false, TYPE_UNKNOWN, name_tok.literal, NULL, expr, name_tok.line, name_tok.column);
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
        return stmt_assign(false, TYPE_UNKNOWN, NULL, expr, rhs, expr->line, expr->column);
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
             * a THROW so runtime TRY/CATCH can observe it. Then
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

        /* If a parse error was recorded, synthesize a runtime THROW call
           statement so that runtime TRY/CATCH can observe the parse error
           as a catchable runtime exception. Then clear the parser error
           state so callers (e.g. RUN/IMPORT) don't treat it as a fatal
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
