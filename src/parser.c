#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void report_error(Parser* parser, const char* message) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    parser->had_error = true;
    fprintf(stderr, "ParseError at %d:%d: %s\n",
            parser->current_token.line, parser->current_token.column, message);
}

void parser_init(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->panic_mode = false;
    parser->had_error = false;
    parser->current_token = lexer_next_token(parser->lexer);
    parser->next_token = lexer_next_token(parser->lexer);
}

static void advance(Parser* parser) {
    parser->previous_token = parser->current_token;
    parser->current_token = parser->next_token;
    parser->next_token = lexer_next_token(parser->lexer);

    /* If the lexer produced an error token, report it and advance until
       we reach a non-error token. Use a loop instead of recursion so the
       parser doesn't overflow the stack or hang when many consecutive
       error tokens are emitted. */
    while (parser->current_token.type == TOKEN_ERROR) {
        report_error(parser, parser->current_token.literal);
        parser->panic_mode = false;

        parser->previous_token = parser->current_token;
        parser->current_token = parser->next_token;
        parser->next_token = lexer_next_token(parser->lexer);
    }
}


static bool match(Parser* parser, TokenType type) {
    if (parser->current_token.type != type) return false;
    advance(parser);
    return true;
}

static bool consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current_token.type == type) {
        advance(parser);
        return true;
    }
    report_error(parser, message);
    return false;
}

static void skip_newlines(Parser* parser) {
    while (parser->current_token.type == TOKEN_NEWLINE) {
        advance(parser);
    }
}

static DeclType parse_type_name(const char* name) {
    if (strcmp(name, "INT") == 0) return TYPE_INT;
    if (strcmp(name, "FLT") == 0) return TYPE_FLT;
    if (strcmp(name, "STR") == 0) return TYPE_STR;
    if (strcmp(name, "FUNC") == 0) return TYPE_FUNC;
    if (strcmp(name, "TNS") == 0) return TYPE_TNS;
    return TYPE_UNKNOWN;
}

static Expr* parse_expression(Parser* parser);
static Stmt* parse_statement(Parser* parser);

static Expr* parse_primary(Parser* parser) {
    Token token = parser->current_token;
    if (match(parser, TOKEN_NUMBER)) {
        return expr_int(strtoll(token.literal, NULL, 2), token.line, token.column);
    }
    if (match(parser, TOKEN_FLOAT)) {
        // binary fixed-point: a.b
        char* dot = strchr(token.literal, '.');
        if (!dot) {
            return expr_flt((double)strtoll(token.literal, NULL, 2), token.line, token.column);
        }
        bool neg = token.literal[0] == '-';
        const char* core = neg ? token.literal + 1 : token.literal;
        const char* dot_pos = strchr(core, '.');
        long long left = 0;
        long long right = 0;
        int frac_len = 0;
        if (dot_pos) {
            size_t left_len = (size_t)(dot_pos - core);
            char* left_str = (char*)malloc(left_len + 1);
            memcpy(left_str, core, left_len);
            left_str[left_len] = '\0';
            left = left_len ? strtoll(left_str, NULL, 2) : 0;
            free(left_str);
            const char* right_str = dot_pos + 1;
            frac_len = (int)strlen(right_str);
            right = frac_len ? strtoll(right_str, NULL, 2) : 0;
        }
        double value = (double)left + (frac_len > 0 ? (double)right / (double)(1LL << frac_len) : 0.0);
        if (neg) value = -value;
        return expr_flt(value, token.line, token.column);
    }
    if (match(parser, TOKEN_STRING)) {
        return expr_str(token.literal, token.line, token.column);
    }
    if (match(parser, TOKEN_IDENT)) {
        return expr_ident(token.literal, token.line, token.column);
    }
    if (match(parser, TOKEN_LPAREN)) {
        Expr* expr = parse_expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }
    if (match(parser, TOKEN_LBRACKET)) {
        Token lb = parser->previous_token; // the '[' token
        Expr* tns = expr_tns(lb.line, lb.column);
        if (parser->current_token.type == TOKEN_RBRACKET) {
            report_error(parser, "Empty tensor literal is not allowed");
            return NULL;
        }
        do {
            Expr* item = parse_expression(parser);
            if (!item) return NULL;
            expr_list_add(&tns->as.tns_items, item);
        } while (match(parser, TOKEN_COMMA));
        consume(parser, TOKEN_RBRACKET, "Expected ']' after tensor literal");
        return tns;
    }
    if (match(parser, TOKEN_LANGLE)) {
        Token lb = parser->previous_token; // the '<' token
        Expr* mp = expr_map(lb.line, lb.column);
        if (parser->current_token.type == TOKEN_RANGLE) {
            report_error(parser, "Empty map literal is not allowed");
            return NULL;
        }
        do {
            // parse key
            Expr* key = parse_expression(parser);
            if (!key) return NULL;
            if (!match(parser, TOKEN_EQUALS)) {
                report_error(parser, "Expected '=' in map literal");
                return NULL;
            }
            Expr* val = parse_expression(parser);
            if (!val) return NULL;
            expr_list_add(&mp->as.map_items.keys, key);
            expr_list_add(&mp->as.map_items.values, val);
        } while (match(parser, TOKEN_COMMA));
        consume(parser, TOKEN_RANGLE, "Expected '>' after map literal");
        return mp;
    }
    report_error(parser, "Expected expression");
    return NULL;
}

static Expr* parse_call(Parser* parser) {
    Expr* expr = parse_primary(parser);
    if (!expr) return NULL;
    while (parser->current_token.type == TOKEN_LPAREN || parser->current_token.type == TOKEN_LBRACKET || parser->current_token.type == TOKEN_LANGLE) {
        if (parser->current_token.type == TOKEN_LPAREN) {
            int line = parser->current_token.line;
            int column = parser->current_token.column;
            advance(parser); // consume '('
            Expr* call = expr_call(expr, line, column);
            if (parser->current_token.type != TOKEN_RPAREN) {
                do {
                    Expr* arg = parse_expression(parser);
                    if (!arg) return NULL;
                    expr_list_add(&call->as.call.args, arg);
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
            Expr* idx = expr_index(expr, line, column);
            if (parser->current_token.type == TOKEN_RBRACKET) {
                report_error(parser, "Empty index list");
                return NULL;
            }
            while (parser->current_token.type != TOKEN_RBRACKET && parser->current_token.type != TOKEN_EOF) {
                // wildcard
                if (match(parser, TOKEN_STAR)) {
                    Expr* wc = expr_wildcard(parser->previous_token.line, parser->previous_token.column);
                    expr_list_add(&idx->as.index.indices, wc);
                } else {
                    // parse an expression for index or possibly a range
                    Expr* start = parse_expression(parser);
                    if (!start) return NULL;
                    // Accept either an explicit DASH token or a negative-number token as the range separator
                    bool is_range = false;
                    if (parser->current_token.type == TOKEN_DASH) {
                        is_range = true;
                    } else if (parser->current_token.type == TOKEN_NUMBER && parser->current_token.literal && parser->current_token.literal[0] == '-') {
                        is_range = true;
                    }
                    if (is_range) {
                        // If it's a DASH token, consume it; otherwise leave the negative-number token for parse_expression
                        if (parser->current_token.type == TOKEN_DASH) advance(parser);
                        Expr* end = parse_expression(parser);
                        if (!end) return NULL;
                        Expr* range = expr_range(start, end, start->line, start->column);
                        expr_list_add(&idx->as.index.indices, range);
                    } else {
                        expr_list_add(&idx->as.index.indices, start);
                    }
                }

                if (parser->current_token.type == TOKEN_COMMA) { advance(parser); continue; }
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
            Expr* idx = expr_index(expr, line, column);
            if (parser->current_token.type == TOKEN_RANGLE) {
                report_error(parser, "Empty index list");
                return NULL;
            }
            while (parser->current_token.type != TOKEN_RANGLE && parser->current_token.type != TOKEN_EOF) {
                Expr* start = parse_expression(parser);
                if (!start) return NULL;
                expr_list_add(&idx->as.index.indices, start);

                if (parser->current_token.type == TOKEN_COMMA) { advance(parser); continue; }
                break;
            }
            consume(parser, TOKEN_RANGLE, "Expected '>' after index list");
            expr = idx;
            continue;
        }
    }
    return expr;
}

static Expr* parse_expression(Parser* parser) {
    return parse_call(parser);
}

static Stmt* parse_block(Parser* parser) {
    Token brace = parser->current_token;
    consume(parser, TOKEN_LBRACE, "Expected '{'");
    Stmt* block = stmt_block(brace.line, brace.column);
    skip_newlines(parser);
    while (parser->current_token.type != TOKEN_RBRACE && parser->current_token.type != TOKEN_EOF) {
        Stmt* stmt = parse_statement(parser);
        if (stmt) {
            stmt_list_add(&block->as.block, stmt);
        }
        skip_newlines(parser);
    }
    consume(parser, TOKEN_RBRACE, "Expected '}' after block");
    return block;
}

static Stmt* parse_if(Parser* parser) {
    Token if_tok = parser->current_token;
    consume(parser, TOKEN_IF, "Expected 'IF'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after IF");
    Expr* cond = parse_expression(parser);
    if (!cond) return NULL;
    consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    Stmt* then_block = parse_block(parser);
    Stmt* stmt = stmt_if(cond, then_block, if_tok.line, if_tok.column);

    skip_newlines(parser);
    while (parser->current_token.type == TOKEN_ELSEIF) {
        advance(parser);
        consume(parser, TOKEN_LPAREN, "Expected '(' after ELSEIF");
        Expr* elif_cond = parse_expression(parser);
        if (!elif_cond) return NULL;
        consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
        Stmt* elif_block = parse_block(parser);
        expr_list_add(&stmt->as.if_stmt.elif_conditions, elif_cond);
        stmt_list_add(&stmt->as.if_stmt.elif_blocks, elif_block);
        skip_newlines(parser);
    }

    if (parser->current_token.type == TOKEN_ELSE) {
        advance(parser);
        Stmt* else_block = parse_block(parser);
        stmt->as.if_stmt.else_branch = else_block;
    }
    return stmt;
}

static Stmt* parse_while(Parser* parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_WHILE, "Expected 'WHILE'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after WHILE");
    Expr* cond = parse_expression(parser);
    if (!cond) return NULL;
    consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    Stmt* body = parse_block(parser);
    return stmt_while(cond, body, tok.line, tok.column);
}

static Stmt* parse_for(Parser* parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_FOR, "Expected 'FOR'");
    consume(parser, TOKEN_LPAREN, "Expected '(' after FOR");
    if (parser->current_token.type != TOKEN_IDENT) {
        report_error(parser, "Expected counter identifier");
        return NULL;
    }
    char* counter = parser->current_token.literal;
    advance(parser);
    consume(parser, TOKEN_COMMA, "Expected ',' after counter");
    Expr* target = parse_expression(parser);
    if (!target) return NULL;
    consume(parser, TOKEN_RPAREN, "Expected ')' after FOR");
    Stmt* body = parse_block(parser);
    return stmt_for(counter, target, body, tok.line, tok.column);
}

static Stmt* parse_try(Parser* parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_TRY, "Expected 'TRY'");
    Stmt* try_block = parse_block(parser);
    skip_newlines(parser);
    consume(parser, TOKEN_CATCH, "Expected 'CATCH' after TRY");
    char* catch_name = NULL;
    if (match(parser, TOKEN_LPAREN)) {
        if (parser->current_token.type == TOKEN_IDENT && strcmp(parser->current_token.literal, "SYMBOL") == 0) {
            advance(parser);
            consume(parser, TOKEN_COLON, "Expected ':' after SYMBOL");
        }
        if (parser->current_token.type == TOKEN_IDENT) {
            catch_name = parser->current_token.literal;
            advance(parser);
        } else {
            report_error(parser, "Expected identifier in CATCH" );
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after CATCH");
    }
    Stmt* catch_block = parse_block(parser);
    return stmt_try(try_block, catch_name, catch_block, tok.line, tok.column);
}

static Stmt* parse_func(Parser* parser) {
    Token tok = parser->current_token;
    consume(parser, TOKEN_FUNC, "Expected 'FUNC'");
    if (parser->current_token.type != TOKEN_IDENT) {
        report_error(parser, "Expected function name");
        return NULL;
    }
    char* name = parser->current_token.literal;
    advance(parser);
    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    ParamList params = {0};
    if (parser->current_token.type != TOKEN_RPAREN) {
        do {
            if (parser->current_token.type != TOKEN_IDENT) {
                report_error(parser, "Expected parameter type");
                break;
            }
            DeclType ptype = parse_type_name(parser->current_token.literal);
            advance(parser);
            consume(parser, TOKEN_COLON, "Expected ':' after parameter type");
            if (parser->current_token.type != TOKEN_IDENT) {
                report_error(parser, "Expected parameter name");
                break;
            }
            Param param;
            param.type = ptype;
            param.name = parser->current_token.literal;
            param.default_value = NULL;
            advance(parser);
            if (match(parser, TOKEN_EQUALS)) {
                param.default_value = parse_expression(parser);
                if (!param.default_value) return NULL;
            }
            param_list_add(&params, param);
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");
    consume(parser, TOKEN_COLON, "Expected ':' before return type");
    if (parser->current_token.type != TOKEN_IDENT) {
        report_error(parser, "Expected return type");
        return NULL;
    }
    DeclType ret = parse_type_name(parser->current_token.literal);
    advance(parser);
    Stmt* body = parse_block(parser);
    Stmt* stmt = stmt_func(name, ret, body, tok.line, tok.column);
    stmt->as.func_stmt.params = params;
    return stmt;
}

static Stmt* parse_statement(Parser* parser) {
    skip_newlines(parser);
    switch (parser->current_token.type) {
        case TOKEN_IF:
            return parse_if(parser);
        case TOKEN_WHILE:
            return parse_while(parser);
        case TOKEN_FOR:
            return parse_for(parser);
        case TOKEN_TRY:
            return parse_try(parser);
        case TOKEN_FUNC:
            return parse_func(parser);
        case TOKEN_RETURN: {
            Token tok = parser->current_token;
            advance(parser);
            consume(parser, TOKEN_LPAREN, "Expected '(' after RETURN");
            Expr* expr = parse_expression(parser);
            if (!expr) return NULL;
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
            char* name = parser->current_token.literal;
            advance(parser);
            consume(parser, TOKEN_RPAREN, "Expected ')' after POP identifier");
            return stmt_pop(name, tok.line, tok.column);
        }
        case TOKEN_BREAK: {
            Token tok = parser->current_token;
            advance(parser);
            consume(parser, TOKEN_LPAREN, "Expected '(' after BREAK");
            Expr* expr = parse_expression(parser);
            if (!expr) return NULL;
            consume(parser, TOKEN_RPAREN, "Expected ')' after BREAK value");
            return stmt_break(expr, tok.line, tok.column);
        }
        case TOKEN_CONTINUE: {
            Token tok = parser->current_token;
            advance(parser);
            if (match(parser, TOKEN_LPAREN)) {
                consume(parser, TOKEN_RPAREN, "Expected ')' after CONTINUE");
            }
            return stmt_continue(tok.line, tok.column);
        }
        case TOKEN_GOTO: {
            Token tok = parser->current_token;
            advance(parser);
            consume(parser, TOKEN_LPAREN, "Expected '(' after GOTO");
            Expr* expr = parse_expression(parser);
            if (!expr) return NULL;
            consume(parser, TOKEN_RPAREN, "Expected ')' after GOTO");
            return stmt_goto(expr, tok.line, tok.column);
        }
        case TOKEN_GOTOPOINT: {
            Token tok = parser->current_token;
            advance(parser);
            consume(parser, TOKEN_LPAREN, "Expected '(' after GOTOPOINT");
            Expr* expr = parse_expression(parser);
            if (!expr) return NULL;
            consume(parser, TOKEN_RPAREN, "Expected ')' after GOTOPOINT target");
            return stmt_gotopoint(expr, tok.line, tok.column);
        }
        default:
            break;
    }

    if (parser->current_token.type == TOKEN_IDENT && parser->next_token.type == TOKEN_COLON) {
        Token type_tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_COLON, "Expected ':' after type");
        if (parser->current_token.type != TOKEN_IDENT) {
            report_error(parser, "Expected identifier name");
            return NULL;
        }
        char* name = parser->current_token.literal;
        advance(parser);
        DeclType dtype = parse_type_name(type_tok.literal);
        if (match(parser, TOKEN_EQUALS)) {
            Expr* expr = parse_expression(parser);
            if (!expr) return NULL;
            return stmt_assign(true, dtype, name, NULL, expr, type_tok.line, type_tok.column);
        }
        return stmt_decl(dtype, name, type_tok.line, type_tok.column);
    }

    if (parser->current_token.type == TOKEN_IDENT && parser->next_token.type == TOKEN_EQUALS) {
        Token name_tok = parser->current_token;
        advance(parser);
        consume(parser, TOKEN_EQUALS, "Expected '=' after identifier");
        Expr* expr = parse_expression(parser);
        if (!expr) return NULL;
        return stmt_assign(false, TYPE_UNKNOWN, name_tok.literal, NULL, expr, name_tok.line, name_tok.column);
    }

    Expr* expr = parse_expression(parser);
    if (!expr) return NULL;

    // Support assignment to an expression LHS (e.g., indexed assignment): expr '=' rhs
    if (parser->current_token.type == TOKEN_EQUALS) {
        // consume '=' and parse RHS
        advance(parser);
        Expr* rhs = parse_expression(parser);
        if (!rhs) return NULL;
        // Create an assign stmt with the expression as target
        return stmt_assign(false, TYPE_UNKNOWN, NULL, expr, rhs, expr->line, expr->column);
    }

    return stmt_expr(expr, expr->line, expr->column);
}

Stmt* parser_parse(Parser* parser) {
    Stmt* program = stmt_block(parser->current_token.line, parser->current_token.column);
    skip_newlines(parser);
    while (parser->current_token.type != TOKEN_EOF) {
        Stmt* stmt = parse_statement(parser);
        if (stmt) {
            stmt_list_add(&program->as.block, stmt);
            skip_newlines(parser);
            continue;
        }

        /* Synchronize after an error: advance to next newline or EOF so the
           parser makes progress instead of repeatedly returning NULL and
           hanging. Clear panic mode to allow further error reports. */
        while (parser->current_token.type != TOKEN_EOF && parser->current_token.type != TOKEN_NEWLINE) {
            advance(parser);
        }
        skip_newlines(parser);
    }
    return program;
}
