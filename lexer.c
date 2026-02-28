#include "lexer.h"
#include <string.h>
#include <stdbool.h>

void lisa_lexer_init(lisa_lexer *lexer, const char *source) {
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
}

static bool is_at_end(lisa_lexer *lexer) {
    return *lexer->current == '\0';
}

static char advance(lisa_lexer *lexer) {
    return *lexer->current++;
}

static char peek(lisa_lexer *lexer) {
    return *lexer->current;
}

static lisa_token make_token(lisa_lexer *lexer, lisa_token_type type) {
    lisa_token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    return token;
}

static lisa_token error_token(lisa_lexer *lexer, const char *message) {
    lisa_token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    return token;
}

static void skip_whitespace(lisa_lexer *lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case ',': /* commas are whitespace in Clojure */
            advance(lexer);
            break;
        case '\n':
            lexer->line++;
            advance(lexer);
            break;
        case ';': /* line comment */
            while (!is_at_end(lexer) && peek(lexer) != '\n') {
                advance(lexer);
            }
            break;
        default:
            return;
        }
    }
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_symbol_char(char c) {
    if (c == '\0') return false;
    if (c <= ' ') return false;
    switch (c) {
    case '(': case ')': case '[': case ']':
    case '"': case ';': case ',':
        return false;
    default:
        return true;
    }
}

static lisa_token_type check_keyword(lisa_lexer *lexer, int start, int rest_len,
                                     const char *rest, lisa_token_type type) {
    int token_len = (int)(lexer->current - lexer->start);
    if (token_len == start + rest_len &&
        memcmp(lexer->start + start, rest, (size_t)rest_len) == 0) {
        return type;
    }
    return TOKEN_SYMBOL;
}

static lisa_token_type identifier_type(lisa_lexer *lexer) {
    int len = (int)(lexer->current - lexer->start);
    switch (lexer->start[0]) {
    case 'd':
        if (len > 1) {
            switch (lexer->start[1]) {
            case 'e': return check_keyword(lexer, 2, 1, "f", TOKEN_DEF);
            case 'o': if (len == 2) return TOKEN_DO; break;
            }
        }
        break;
    case 'f':
        if (len > 1) {
            switch (lexer->start[1]) {
            case 'n': if (len == 2) return TOKEN_FN; break;
            case 'a': return check_keyword(lexer, 2, 3, "lse", TOKEN_FALSE);
            }
        }
        break;
    case 'i': return check_keyword(lexer, 1, 1, "f", TOKEN_IF);
    case 'l': return check_keyword(lexer, 1, 2, "et", TOKEN_LET);
    case 'n': return check_keyword(lexer, 1, 2, "il", TOKEN_NIL);
    case 't': return check_keyword(lexer, 1, 3, "rue", TOKEN_TRUE);
    }
    return TOKEN_SYMBOL;
}

static lisa_token number(lisa_lexer *lexer) {
    bool has_dot = false;
    while (!is_at_end(lexer) && (is_digit(peek(lexer)) || peek(lexer) == '.')) {
        if (peek(lexer) == '.') {
            if (has_dot) break;
            has_dot = true;
        }
        advance(lexer);
    }
    return make_token(lexer, has_dot ? TOKEN_DOUBLE : TOKEN_NUMBER);
}

static lisa_token string(lisa_lexer *lexer) {
    while (!is_at_end(lexer) && peek(lexer) != '"') {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\' && *(lexer->current + 1) != '\0') {
            advance(lexer); /* skip backslash */
        }
        advance(lexer);
    }
    if (is_at_end(lexer)) return error_token(lexer, "Unterminated string.");
    advance(lexer); /* closing quote */
    return make_token(lexer, TOKEN_STRING);
}

static lisa_token symbol(lisa_lexer *lexer) {
    while (!is_at_end(lexer) && is_symbol_char(peek(lexer))) {
        advance(lexer);
    }
    return make_token(lexer, identifier_type(lexer));
}

lisa_token lisa_lexer_next(lisa_lexer *lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer)) return make_token(lexer, TOKEN_EOF);

    char c = advance(lexer);

    switch (c) {
    case '(': return make_token(lexer, TOKEN_LPAREN);
    case ')': return make_token(lexer, TOKEN_RPAREN);
    case '[': return make_token(lexer, TOKEN_LBRACKET);
    case ']': return make_token(lexer, TOKEN_RBRACKET);
    case '"': return string(lexer);
    }

    if (is_digit(c)) return number(lexer);

    /* Negative number: '-' followed by digit */
    if (c == '-' && !is_at_end(lexer) && is_digit(peek(lexer))) {
        return number(lexer);
    }

    if (is_symbol_char(c)) return symbol(lexer);

    return error_token(lexer, "Unexpected character.");
}
