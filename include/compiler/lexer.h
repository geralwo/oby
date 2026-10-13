#ifndef OBY_LEXER_H
#define OBY_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
    TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_COLON_COLON,
    TOKEN_COMMA, TOKEN_DOT, TOKEN_DOT_DOT, TOKEN_DOLLAR, TOKEN_ARROW,
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL, TOKEN_BANG_EQUAL,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL,
    TOKEN_PERCENT_EQUAL,
    TOKEN_LT, TOKEN_GT, TOKEN_LESS_EQUAL, TOKEN_GREATER_EQUAL,
    TOKEN_AMP, TOKEN_PIPE, TOKEN_CARET, TOKEN_TILDE,
    TOKEN_SHIFT_LEFT, TOKEN_SHIFT_RIGHT,
    TOKEN_AMP_EQUAL, TOKEN_PIPE_EQUAL, TOKEN_CARET_EQUAL,
    TOKEN_SHIFT_LEFT_EQUAL, TOKEN_SHIFT_RIGHT_EQUAL,
    TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_FLOAT, TOKEN_STRING, TOKEN_CHAR,
    TOKEN_NEWLINE,
    TOKEN_ERROR, TOKEN_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    size_t length;
    int line;
    const char *message;  /* human-readable text for TOKEN_ERROR, else NULL */
} Token;

typedef struct {
    const char *source;
    const char *start;
    const char *current;
    const char *end;   /* optional end bound; NULL = NUL-terminated */
    int line;
    char errbuf[64];   /* storage for formatted error messages */
} Lexer;

void lexer_init(Lexer *l, const char *source);
Token scan_token(Lexer *l);

/* Human-readable name for a token kind, e.g. "'('", "identifier", "end of file". */
const char *token_kind_name(TokenKind kind);

#endif // OBY_LEXER_H