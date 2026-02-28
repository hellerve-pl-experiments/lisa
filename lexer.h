#ifndef LISA_LEXER_H
#define LISA_LEXER_H

typedef enum {
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_NUMBER,
    TOKEN_DOUBLE,
    TOKEN_STRING,
    TOKEN_SYMBOL,

    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NIL,
    TOKEN_DEF,
    TOKEN_FN,
    TOKEN_LET,
    TOKEN_IF,
    TOKEN_DO,

    TOKEN_ERROR,
    TOKEN_EOF,
} lisa_token_type;

typedef struct {
    lisa_token_type type;
    const char *start;
    int length;
    int line;
} lisa_token;

typedef struct {
    const char *start;
    const char *current;
    int line;
} lisa_lexer;

void lisa_lexer_init(lisa_lexer *lexer, const char *source);
lisa_token lisa_lexer_next(lisa_lexer *lexer);

#endif
