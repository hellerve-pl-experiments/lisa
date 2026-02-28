#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void parser_advance(lisa_parser *parser);
static lisa_ast *expression(lisa_parser *parser);

static void error_at(lisa_parser *parser, lisa_token *token, const char *message) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    parser->had_error = true;

    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
}

static void error(lisa_parser *parser, const char *message) {
    error_at(parser, &parser->previous, message);
}

static void error_at_current(lisa_parser *parser, const char *message) {
    error_at(parser, &parser->current, message);
}

void lisa_parser_init(lisa_parser *parser, const char *source) {
    lisa_lexer_init(&parser->lexer, source);
    parser->had_error = false;
    parser->panic_mode = false;
    parser_advance(parser);
}

static void parser_advance(lisa_parser *parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = lisa_lexer_next(&parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;
        error_at_current(parser, parser->current.start);
    }
}

static bool check(lisa_parser *parser, lisa_token_type type) {
    return parser->current.type == type;
}


static void consume(lisa_parser *parser, lisa_token_type type, const char *message) {
    if (parser->current.type == type) {
        parser_advance(parser);
        return;
    }
    error_at_current(parser, message);
}

static bool is_symbol_token(lisa_token_type type) {
    /* Symbols can also be things that look like keywords but are used as identifiers */
    return type == TOKEN_SYMBOL;
}

/* --- Expression parsing --- */

static lisa_ast *parse_number(lisa_parser *parser) {
    int line = parser->previous.line;
    char *buf = strndup(parser->previous.start, (size_t)parser->previous.length);
    errno = 0;
    int64_t val = strtoll(buf, NULL, 10);
    free(buf);
    if (errno == ERANGE) {
        error(parser, "Number literal out of range.");
        return lisa_ast_int(0, line);
    }
    return lisa_ast_int(val, line);
}

static lisa_ast *parse_double(lisa_parser *parser) {
    int line = parser->previous.line;
    char *buf = strndup(parser->previous.start, (size_t)parser->previous.length);
    double val = strtod(buf, NULL);
    free(buf);
    return lisa_ast_double(val, line);
}

static lisa_ast *parse_string(lisa_parser *parser) {
    /* Token includes quotes, skip them */
    return lisa_ast_string(
        parser->previous.start + 1,
        parser->previous.length - 2,
        parser->previous.line
    );
}

static lisa_ast *parse_symbol(lisa_parser *parser) {
    return lisa_ast_symbol(
        parser->previous.start,
        parser->previous.length,
        parser->previous.line
    );
}

static lisa_ast *parse_def(lisa_parser *parser, int line) {
    /* (def name value) — name is next token */
    if (!is_symbol_token(parser->current.type)) {
        error_at_current(parser, "Expected symbol after 'def'.");
        return NULL;
    }
    parser_advance(parser);
    lisa_ast *name = parse_symbol(parser);

    lisa_ast *value = expression(parser);
    if (value == NULL) { lisa_ast_free(name); return NULL; }

    consume(parser, TOKEN_RPAREN, "Expected ')' after def.");
    return lisa_ast_def(name, value, line);
}

static lisa_ast *parse_fn(lisa_parser *parser, int line) {
    /* (fn [params...] body...) */
    lisa_ast *node = lisa_ast_fn(line);

    consume(parser, TOKEN_LBRACKET, "Expected '[' for fn parameters.");

    while (!check(parser, TOKEN_RBRACKET) && !check(parser, TOKEN_EOF)) {
        if (!is_symbol_token(parser->current.type)) {
            error_at_current(parser, "Expected parameter name.");
            lisa_ast_free(node);
            return NULL;
        }
        parser_advance(parser);
        lisa_ast_list_push(&node->as.fn.params, parse_symbol(parser));
    }

    consume(parser, TOKEN_RBRACKET, "Expected ']' after fn parameters.");

    /* Body: one or more expressions */
    while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
        lisa_ast *expr = expression(parser);
        if (expr == NULL) { lisa_ast_free(node); return NULL; }
        lisa_ast_list_push(&node->as.fn.body, expr);
    }

    if (node->as.fn.body.count == 0) {
        error(parser, "fn body cannot be empty.");
        lisa_ast_free(node);
        return NULL;
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after fn body.");
    return node;
}

static lisa_ast *parse_let(lisa_parser *parser, int line) {
    /* (let [x 1 y 2] body...) */
    lisa_ast *node = lisa_ast_let(line);

    consume(parser, TOKEN_LBRACKET, "Expected '[' for let bindings.");

    while (!check(parser, TOKEN_RBRACKET) && !check(parser, TOKEN_EOF)) {
        if (!is_symbol_token(parser->current.type)) {
            error_at_current(parser, "Expected binding name.");
            lisa_ast_free(node);
            return NULL;
        }
        parser_advance(parser);
        lisa_ast_list_push(&node->as.let.bindings, parse_symbol(parser));

        lisa_ast *val = expression(parser);
        if (val == NULL) { lisa_ast_free(node); return NULL; }
        lisa_ast_list_push(&node->as.let.bindings, val);
    }

    consume(parser, TOKEN_RBRACKET, "Expected ']' after let bindings.");

    while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
        lisa_ast *expr = expression(parser);
        if (expr == NULL) { lisa_ast_free(node); return NULL; }
        lisa_ast_list_push(&node->as.let.body, expr);
    }

    if (node->as.let.body.count == 0) {
        error(parser, "let body cannot be empty.");
        lisa_ast_free(node);
        return NULL;
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after let body.");
    return node;
}

static lisa_ast *parse_if(lisa_parser *parser, int line) {
    /* (if cond then else?) */
    lisa_ast *cond = expression(parser);
    if (cond == NULL) return NULL;

    lisa_ast *then_b = expression(parser);
    if (then_b == NULL) { lisa_ast_free(cond); return NULL; }

    lisa_ast *else_b = NULL;
    if (!check(parser, TOKEN_RPAREN)) {
        else_b = expression(parser);
        if (else_b == NULL) {
            lisa_ast_free(cond);
            lisa_ast_free(then_b);
            return NULL;
        }
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after if.");
    return lisa_ast_if(cond, then_b, else_b, line);
}

static lisa_ast *parse_do(lisa_parser *parser, int line) {
    /* (do exprs...) */
    lisa_ast *node = lisa_ast_do(line);

    while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
        lisa_ast *expr = expression(parser);
        if (expr == NULL) { lisa_ast_free(node); return NULL; }
        lisa_ast_list_push(&node->as.do_block.exprs, expr);
    }

    if (node->as.do_block.exprs.count == 0) {
        error(parser, "do block cannot be empty.");
        lisa_ast_free(node);
        return NULL;
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after do.");
    return node;
}

static lisa_ast *parse_list_form(lisa_parser *parser) {
    /* We've consumed '(' already */
    int line = parser->previous.line;

    /* Empty list: () */
    if (check(parser, TOKEN_RPAREN)) {
        parser_advance(parser);
        return lisa_ast_nil(line);
    }

    /* Check for special forms */
    if (check(parser, TOKEN_DEF)) {
        parser_advance(parser);
        return parse_def(parser, line);
    }
    if (check(parser, TOKEN_FN)) {
        parser_advance(parser);
        return parse_fn(parser, line);
    }
    if (check(parser, TOKEN_LET)) {
        parser_advance(parser);
        return parse_let(parser, line);
    }
    if (check(parser, TOKEN_IF)) {
        parser_advance(parser);
        return parse_if(parser, line);
    }
    if (check(parser, TOKEN_DO)) {
        parser_advance(parser);
        return parse_do(parser, line);
    }

    /* Function call: (callee args...) */
    lisa_ast *callee = expression(parser);
    if (callee == NULL) return NULL;

    lisa_ast *call = lisa_ast_call(callee, line);
    while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
        lisa_ast *arg = expression(parser);
        if (arg == NULL) { lisa_ast_free(call); return NULL; }
        lisa_ast_list_push(&call->as.call.args, arg);
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after arguments.");
    return call;
}

static lisa_ast *expression(lisa_parser *parser) {
    parser_advance(parser);

    switch (parser->previous.type) {
    case TOKEN_NUMBER:   return parse_number(parser);
    case TOKEN_DOUBLE:   return parse_double(parser);
    case TOKEN_STRING:   return parse_string(parser);
    case TOKEN_TRUE:     return lisa_ast_bool(1, parser->previous.line);
    case TOKEN_FALSE:    return lisa_ast_bool(0, parser->previous.line);
    case TOKEN_NIL:      return lisa_ast_nil(parser->previous.line);
    case TOKEN_SYMBOL:   return parse_symbol(parser);
    case TOKEN_LPAREN:   return parse_list_form(parser);
    case TOKEN_EOF:
        error(parser, "Unexpected end of input.");
        return NULL;
    default:
        error(parser, "Unexpected token.");
        return NULL;
    }
}

lisa_ast *lisa_parse_expr(lisa_parser *parser) {
    return expression(parser);
}

lisa_ast **lisa_parse(lisa_parser *parser, int *count) {
    int cap = 8;
    lisa_ast **exprs = malloc(sizeof(lisa_ast*) * (size_t)cap);
    *count = 0;

    while (!check(parser, TOKEN_EOF)) {
        lisa_ast *expr = expression(parser);
        if (expr == NULL) {
            /* Error recovery: skip to next top-level form */
            parser->panic_mode = false;
            while (!check(parser, TOKEN_EOF) && !check(parser, TOKEN_LPAREN)) {
                parser_advance(parser);
            }
            continue;
        }
        if (*count >= cap) {
            cap *= 2;
            exprs = realloc(exprs, sizeof(lisa_ast*) * (size_t)cap);
        }
        exprs[(*count)++] = expr;
    }

    return exprs;
}

void lisa_parse_free(lisa_ast **exprs, int count) {
    for (int i = 0; i < count; i++) {
        lisa_ast_free(exprs[i]);
    }
    free(exprs);
}
