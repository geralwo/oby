#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "compiler/lexer.h"
#include "util.h"

static bool is_at_end(Lexer *l) {
    if (l->end) return l->current >= l->end;
    return *l->current == '\0';
}

static char advance(Lexer *l) {
    return *l->current++;
}

static char peek(Lexer *l) {
    return is_at_end(l) ? '\0' : *l->current;
}

static char peek_next(Lexer *l) {
    if (is_at_end(l)) return '\0';
    if (l->end && l->current + 1 >= l->end) return '\0';
    return l->current[1];
}

/* Look `offset` characters ahead, stopping at the end of the source. */
static char peek_at(Lexer *l, size_t offset) {
    for (size_t i = 0; i <= offset; i++) {
        if (l->end) {
            if (l->current + i >= l->end) return '\0';
        } else if (l->current[i] == '\0') {
            return '\0';
        }
    }
    return l->current[offset];
}

static bool match(Lexer *l, char expected) {
    if (is_at_end(l) || *l->current != expected) return false;
    l->current++;
    return true;
}

static Token make_token(Lexer *l, TokenKind kind) {
    Token token;
    token.kind = kind;
    token.start = l->start;
    token.length = (size_t)(l->current - l->start);
    token.line = l->line;
    token.message = NULL;
    return token;
}

static Token error_token(Lexer *l, const char *message) {
    Token token;
    token.kind = TOKEN_ERROR;
    /* Point at the offending source span so diagnostics can underline it. */
    token.start = l->start;
    token.length = (size_t)(l->current - l->start);
    token.line = l->line;
    token.message = message;
    return token;
}

/* Skip spaces, tabs, carriage returns and comments. Newlines are NOT skipped
   here: they are emitted as TOKEN_NEWLINE so the parser can use them as
   statement terminators. */
static void skip_whitespace(Lexer *l) {
    for (;;) {
        char c = peek(l);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(l);
                break;
            case '/':
                if (peek_next(l) == '/') {
                    while (!is_at_end(l) && peek(l) != '\n') advance(l);
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

#define BAD_SEPARATOR "a digit separator must sit between digits"

/* Scan a run of digits of `base`, taking `_` separators: one is allowed only
   when a digit of the same base follows one just scanned. Returns false when a
   `_` is out of place. */
static bool scan_digit_run(Lexer *l, int base) {
    for (;;) {
        if (digit_in_base(base, peek(l))) { advance(l); continue; }
        if (peek(l) == '_') {
            if (!digit_in_base(base, peek_next(l))) return false;
            advance(l);
            continue;
        }
        return true;
    }
}

/* number ::= digit ( '_'? digit )* ( '.' digit… )? ( [eE] [+-]? digit… )?
   An integer may also be written `0x` (hex) or `0b` (binary); there is no
   octal. A '.' only joins the number when a digit follows, so `1.method` still
   lexes as `1` `.` `method`. A `_` between two digits is a separator. */
static Token number(Lexer *l) {
    /* The first digit was consumed by the scanner, so the prefix is checked at
       the current position; `l->start[0]` is that first digit. */
    if (l->start[0] == '0' && (peek(l) == 'x' || peek(l) == 'X')) {
        advance(l); /* 'x' */
        if (!isxdigit(peek(l))) return error_token(l, "expected hex digits after '0x'");
        if (!scan_digit_run(l, 16)) return error_token(l, BAD_SEPARATOR);
        if (isalnum(peek(l)) || peek(l) == '_')
            return error_token(l, "invalid digit in a hex literal");
        return make_token(l, TOKEN_NUMBER);
    }
    if (l->start[0] == '0' && (peek(l) == 'b' || peek(l) == 'B')) {
        advance(l); /* 'b' */
        if (peek(l) != '0' && peek(l) != '1')
            return error_token(l, "expected binary digits after '0b'");
        if (!scan_digit_run(l, 2)) return error_token(l, BAD_SEPARATOR);
        if (isalnum(peek(l)) || peek(l) == '_')
            return error_token(l, "invalid digit in a binary literal");
        return make_token(l, TOKEN_NUMBER);
    }

    if (!scan_digit_run(l, 10)) return error_token(l, BAD_SEPARATOR);

    bool is_float = false;
    if (peek(l) == '.' && isdigit(peek_next(l))) {
        is_float = true;
        advance(l); /* '.' */
        if (!scan_digit_run(l, 10)) return error_token(l, BAD_SEPARATOR);
    }

    if (peek(l) == 'e' || peek(l) == 'E') {
        char after = peek_next(l);
        bool has_exp = isdigit(after) ||
                       ((after == '+' || after == '-') && isdigit(peek_at(l, 2)));
        if (has_exp) {
            is_float = true;
            advance(l); /* 'e' */
            if (peek(l) == '+' || peek(l) == '-') advance(l);
            if (!scan_digit_run(l, 10)) return error_token(l, BAD_SEPARATOR);
        }
    }

    return make_token(l, is_float ? TOKEN_FLOAT : TOKEN_NUMBER);
}

static Token identifier(Lexer *l) {
    // Identifiers must start with an alphabetical character
    if (isalpha(peek(l))) {
        advance(l);
        while (isalnum(peek(l)) || peek(l) == '_') advance(l);
        return make_token(l, TOKEN_IDENTIFIER);
    }
    return error_token(l, "identifier must start with a letter");
}

/* ------------------------------------------------------------------ */
/* Interpolation bodies                                                */
/* ------------------------------------------------------------------ */

/* True at a scan bound: an explicit `end` or the NUL of an unbounded source. */
static bool scan_end(const char *p, const char *end) {
    return end ? p >= end : *p == '\0';
}

static const char *body_end_from(const char *p, const char *end, const char **bad);

/* Skip a string literal inside a body (`p` is its opening quote). Escapes are
   the ordinary ones here: the body is code, so the enclosing literal's escapes
   do not apply to it. NULL when the input runs out. */
static const char *skip_body_string(const char *p, const char *end, const char **bad) {
    p++; /* the opening quote */
    while (!scan_end(p, end)) {
        if (*p == '\\') {
            p++;
            if (scan_end(p, end)) return NULL;
            p++;
            continue;
        }
        if (*p == '"') return p + 1;
        if (*p == '$' && !scan_end(p + 1, end) && p[1] == '{') {
            const char *close = body_end_from(p + 2, end, bad);
            if (!close) return NULL;
            p = close + 1;
            continue;
        }
        p++;
    }
    return NULL;
}

/* The body of a `${...}`: `p` is just after the `{`. Returns its matching `}`,
   or NULL when the input runs out first. */
static const char *body_end_from(const char *p, const char *end, const char **bad) {
    int depth = 1;
    while (!scan_end(p, end)) {
        char c = *p;
        if (c == '"') {
            const char *after = skip_body_string(p, end, bad);
            if (!after) return NULL;
            p = after;
            continue;
        }
        if (c == '\\') {
            /* At this level we are looking at code, where a backslash is never
               valid: say so rather than "the body never ended". */
            if (bad) *bad = p;
            return NULL;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) return p;
        }
        p++;
    }
    return NULL;
}

const char *interp_body_end(const char *text, const char *end, const char **bad) {
    if (bad) *bad = NULL;
    return body_end_from(text, end, bad);
}

/* The line of `at`, counted from `from`, which sits on `line`. */
static int line_at(const char *from, const char *at, int line) {
    for (; from < at; from++)
        if (*from == '\n') line++;
    return line;
}

static Token string(Lexer *l) {
    unsigned start_line = l->line;
    while (!is_at_end(l)) {
        char c = peek(l);
        if (c == '\\') {           /* keep `\"` from ending the string */
            advance(l);
            if (is_at_end(l)) break;
            advance(l);
            continue;
        }
        if (c == '"') break;
        if (c == '\n') l->line++;
        if (c == '$' && peek_next(l) == '{') {
            /* The body is skipped whole — it is code of its own, so its quotes
               and braces cannot end (or split) this literal. */
            const char *body = l->current + 2;
            const char *bad = NULL;
            const char *close = interp_body_end(body, l->end, &bad);
            if (!close) {
                if (bad) {
                    Token token = error_token(l, "unexpected character '\\'");
                    token.start = bad;
                    token.length = 1;
                    token.line = line_at(l->start, bad, (int)start_line);
                    return token;
                }
                l->line = start_line;   /* report where the literal began */
                return error_token(l, "unterminated '${' in string");
            }
            for (const char *p = body; p < close; p++)
                if (*p == '\n') l->line++;
            l->current = close + 1;     /* past the `}` */
            continue;
        }
        advance(l);
    }

    if (is_at_end(l)) {
        l->line = start_line;
        return error_token(l, "unterminated string");
    }

    advance(l); // The closing quote
    return make_token(l, TOKEN_STRING);
}

/* char ::= '\'' escape | any '\'' — the raw text; the parser decodes it, so it
   must not span a line (otherwise a missing quote would swallow the file). */
static Token char_literal(Lexer *l) {
    while (!is_at_end(l) && peek(l) != '\n') {
        char c = peek(l);
        if (c == '\\') {           /* keep `\''` from ending the literal */
            advance(l);
            if (is_at_end(l) || peek(l) == '\n') break;
            advance(l);
            continue;
        }
        if (c == '\'') break;
        advance(l);
    }

    if (is_at_end(l) || peek(l) != '\'')
        return error_token(l, "unterminated character literal");

    advance(l); // The closing quote
    return make_token(l, TOKEN_CHAR);
}

void lexer_init(Lexer *l, const char *source) {
    l->source = source;
    l->start = source;
    l->current = source;
    l->end = NULL;
    l->line = 1;
}

Token scan_token(Lexer *l) {
    skip_whitespace(l);
    l->start = l->current;

    if (is_at_end(l)) return make_token(l, TOKEN_EOF);

    char c = advance(l);
    if (c == '\n') {
        Token t = make_token(l, TOKEN_NEWLINE);
        l->line++;
        return t;
    }
    if (isalpha(c)) {
        // Backtrack to let identifier() handle the full identifier
        l->current--; 
        return identifier(l);
    }
    if (isdigit(c)) return number(l);
    if (c == '"') return string(l);
    if (c == '\'') return char_literal(l);

    switch (c) {
        case '(': return make_token(l, TOKEN_LEFT_PAREN);
        case ')': return make_token(l, TOKEN_RIGHT_PAREN);
        case '{': return make_token(l, TOKEN_LEFT_BRACE);
        case '}': return make_token(l, TOKEN_RIGHT_BRACE);
        case '[': return make_token(l, TOKEN_LEFT_BRACKET);
        case ']': return make_token(l, TOKEN_RIGHT_BRACKET);
        case ';': return make_token(l, TOKEN_SEMICOLON);
        case ',': return make_token(l, TOKEN_COMMA);
        case '.': return make_token(l, match(l, '.') ? TOKEN_DOT_DOT : TOKEN_DOT);
        case '$': return make_token(l, TOKEN_DOLLAR);
        case ':': return make_token(l, match(l, ':') ? TOKEN_COLON_COLON : TOKEN_COLON);
        case '-':
            if (match(l, '>')) return make_token(l, TOKEN_ARROW);
            return make_token(l, match(l, '=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
        case '=': return make_token(l, match(l, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '!':
            if (match(l, '=')) return make_token(l, TOKEN_BANG_EQUAL);
            snprintf(l->errbuf, sizeof(l->errbuf), "unexpected character '!' (use 'not')");
            return error_token(l, l->errbuf);
        case '+': return make_token(l, match(l, '=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
        case '*': return make_token(l, match(l, '=') ? TOKEN_STAR_EQUAL : TOKEN_STAR);
        case '/': return make_token(l, match(l, '=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
        case '%': return make_token(l, match(l, '=') ? TOKEN_PERCENT_EQUAL : TOKEN_PERCENT);
        case '<':
            if (match(l, '<'))
                return make_token(l, match(l, '=') ? TOKEN_SHIFT_LEFT_EQUAL : TOKEN_SHIFT_LEFT);
            return make_token(l, match(l, '=') ? TOKEN_LESS_EQUAL : TOKEN_LT);
        case '>':
            if (match(l, '>'))
                return make_token(l, match(l, '=') ? TOKEN_SHIFT_RIGHT_EQUAL : TOKEN_SHIFT_RIGHT);
            return make_token(l, match(l, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GT);
        case '&':
            if (match(l, '&')) {
                snprintf(l->errbuf, sizeof(l->errbuf), "unexpected '&&' (use 'and')");
                return error_token(l, l->errbuf);
            }
            return make_token(l, match(l, '=') ? TOKEN_AMP_EQUAL : TOKEN_AMP);
        case '|':
            if (match(l, '|')) {
                snprintf(l->errbuf, sizeof(l->errbuf), "unexpected '||' (use 'or')");
                return error_token(l, l->errbuf);
            }
            return make_token(l, match(l, '=') ? TOKEN_PIPE_EQUAL : TOKEN_PIPE);
        case '^': return make_token(l, match(l, '=') ? TOKEN_CARET_EQUAL : TOKEN_CARET);
        case '~': return make_token(l, TOKEN_TILDE);
    }

    snprintf(l->errbuf, sizeof(l->errbuf), "unexpected character '%c'", c);
    return error_token(l, l->errbuf);
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOKEN_LEFT_PAREN:   return "'('";
        case TOKEN_RIGHT_PAREN:  return "')'";
        case TOKEN_LEFT_BRACE:   return "'{'";
        case TOKEN_RIGHT_BRACE:  return "'}'";
        case TOKEN_LEFT_BRACKET: return "'['";
        case TOKEN_RIGHT_BRACKET:return "']'";
        case TOKEN_SEMICOLON:    return "';'";
        case TOKEN_COLON:        return "':'";
        case TOKEN_COLON_COLON:  return "'::'";
        case TOKEN_COMMA:        return "','";
        case TOKEN_DOT:          return "'.'";
        case TOKEN_DOT_DOT:      return "'..'";
        case TOKEN_DOLLAR:       return "'$'";
        case TOKEN_ARROW:        return "'->'";
        case TOKEN_EQUAL:        return "'='";
        case TOKEN_EQUAL_EQUAL:  return "'=='";
        case TOKEN_BANG_EQUAL:   return "'!='";
        case TOKEN_PLUS:         return "'+'";
        case TOKEN_MINUS:        return "'-'";
        case TOKEN_STAR:         return "'*'";
        case TOKEN_SLASH:        return "'/'";
        case TOKEN_PERCENT:      return "'%'";
        case TOKEN_PLUS_EQUAL:   return "'+='";
        case TOKEN_MINUS_EQUAL:  return "'-='";
        case TOKEN_STAR_EQUAL:   return "'*='";
        case TOKEN_SLASH_EQUAL:  return "'/='";
        case TOKEN_PERCENT_EQUAL:return "'%='";
        case TOKEN_LT:           return "'<'";
        case TOKEN_GT:           return "'>'";
        case TOKEN_LESS_EQUAL:   return "'<='";
        case TOKEN_GREATER_EQUAL:return "'>='";
        case TOKEN_AMP:          return "'&'";
        case TOKEN_PIPE:         return "'|'";
        case TOKEN_CARET:        return "'^'";
        case TOKEN_TILDE:        return "'~'";
        case TOKEN_SHIFT_LEFT:   return "'<<'";
        case TOKEN_SHIFT_RIGHT:  return "'>>'";
        case TOKEN_AMP_EQUAL:    return "'&='";
        case TOKEN_PIPE_EQUAL:   return "'|='";
        case TOKEN_CARET_EQUAL:  return "'^='";
        case TOKEN_SHIFT_LEFT_EQUAL:  return "'<<='";
        case TOKEN_SHIFT_RIGHT_EQUAL: return "'>>='";
        case TOKEN_IDENTIFIER:   return "identifier";
        case TOKEN_NUMBER:       return "number";
        case TOKEN_FLOAT:        return "float";
        case TOKEN_STRING:       return "string";
        case TOKEN_CHAR:         return "char";
        case TOKEN_NEWLINE:      return "newline";
        case TOKEN_ERROR:        return "invalid token";
        case TOKEN_EOF:          return "end of file";
    }
    return "token";
}