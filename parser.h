#ifndef LISA_PARSER_H
#define LISA_PARSER_H

#include "lexer.h"
#include "ast.h"
#include <stdbool.h>

typedef struct {
    lisa_lexer lexer;
    lisa_token current;
    lisa_token previous;
    bool had_error;
    bool panic_mode;
} lisa_parser;

void lisa_parser_init(lisa_parser *parser, const char *source);

/* Parse a single expression. Returns NULL on error. */
lisa_ast *lisa_parse_expr(lisa_parser *parser);

/* Parse all top-level expressions until EOF. Returns count, fills array. */
lisa_ast **lisa_parse(lisa_parser *parser, int *count);

/* Free an array returned by lisa_parse */
void lisa_parse_free(lisa_ast **exprs, int count);

#endif
