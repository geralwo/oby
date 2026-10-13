#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compiler/lexer.h"
#include "compiler/parser.h"
#include "compiler/ast_printer.h"
#include "compiler/diag.h"
#include "core/arena.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Parser state helpers                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *current;
    const char *start;
    int line;
    Token cur_tok;
    Token prev_tok;
} ParserState;

static ParserState save_state(Parser *p) {
    ParserState s;
    s.current = p->lexer->current;
    s.start = p->lexer->start;
    s.line = p->lexer->line;
    s.cur_tok = p->current;
    s.prev_tok = p->previous;
    return s;
}

static void restore_state(Parser *p, ParserState s) {
    p->lexer->current = s.current;
    p->lexer->start = s.start;
    p->lexer->line = s.line;
    p->current = s.cur_tok;
    p->previous = s.prev_tok;
}

/* Keywords are contextual: the lexer emits plain identifiers. */
static bool is_keyword(Token t, const char *kw) {
    size_t len = strlen(kw);
    return t.kind == TOKEN_IDENTIFIER && t.length == len && memcmp(t.start, kw, len) == 0;
}

/* True if a statement cannot end on `t`, so a following newline continues it.
   This is the only place newlines are ignored: after a comma, a binary/unary
   operator, an opening bracket, or a member-access/assignment operator. */
static bool continues_line(Token t) {
    switch (t.kind) {
        case TOKEN_COMMA:
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
        case TOKEN_SEMICOLON:
        case TOKEN_PLUS_EQUAL:
        case TOKEN_MINUS_EQUAL:
        case TOKEN_STAR_EQUAL:
        case TOKEN_SLASH_EQUAL:
        case TOKEN_PERCENT_EQUAL:
        case TOKEN_AMP:
        case TOKEN_PIPE:
        case TOKEN_CARET:
        case TOKEN_SHIFT_LEFT:
        case TOKEN_SHIFT_RIGHT:
        case TOKEN_AMP_EQUAL:
        case TOKEN_PIPE_EQUAL:
        case TOKEN_CARET_EQUAL:
        case TOKEN_SHIFT_LEFT_EQUAL:
        case TOKEN_SHIFT_RIGHT_EQUAL:
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_LT:
        case TOKEN_GT:
        case TOKEN_LEFT_PAREN:
        case TOKEN_LEFT_BRACKET:
        case TOKEN_DOT:
        case TOKEN_COLON_COLON:
        case TOKEN_ARROW:
        case TOKEN_EQUAL:
            return true;
        case TOKEN_IDENTIFIER:
            return is_keyword(t, "and") || is_keyword(t, "or") || is_keyword(t, "not");
        default:
            return false;
    }
}

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = scan_token(p->lexer);
    /* A newline after a token that cannot end a statement is just whitespace. */
    while (p->current.kind == TOKEN_NEWLINE && continues_line(p->previous)) {
        p->current = scan_token(p->lexer);
    }
    if (p->current.kind == TOKEN_ERROR && !p->quiet) {
        diag_error(p->diag, p->current, "%s",
                   p->current.message ? p->current.message : "invalid token");
        p->had_error = true;
    }
}

/* Consume any newline tokens (used at statement boundaries). */
static void skip_newlines(Parser *p) {
    while (p->current.kind == TOKEN_NEWLINE) advance(p);
}

static bool check(Parser *p, TokenKind kind) {
    return p->current.kind == kind;
}

static bool match(Parser *p, TokenKind kind) {
    if (p->current.kind == kind) {
        advance(p);
        return true;
    }
    return false;
}

/* Consume `kind` or report an error. `what` describes the construct, e.g.
   "to close the argument list". */
static void expect(Parser *p, TokenKind kind, const char *what) {
    if (match(p, kind)) return;
    if (p->quiet) return;
    diag_error(p->diag, p->current, "expected %s %s, found %s",
               token_kind_name(kind), what, token_kind_name(p->current.kind));
    p->had_error = true;
}

/* ------------------------------------------------------------------ */
/* String literals & interpolation                                    */
/* ------------------------------------------------------------------ */

static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_type_ref(Parser *p);

/* Consume the `>` that closes a type-argument list. A `>>` token closes two
   nested lists at once: it is consumed once and the second `>` is owed to the
   enclosing list (`pending_gt`), so it is only valid when one exists. */
static bool consume_type_close(Parser *p) {
    if (p->pending_gt) {
        p->pending_gt = false;
        return true;
    }
    if (match(p, TOKEN_GT)) return true;
    if (check(p, TOKEN_SHIFT_RIGHT) && p->type_arg_depth >= 2) {
        advance(p);
        p->pending_gt = true;
        return true;
    }
    return false;
}

/* `< typeRef { ',' typeRef } '>'` after a path segment. A segment with no list
   keeps `type_args` NULL. Arguments parse wherever a type name is written;
   what they mean is sema's job (`plans/done/generics.md`). */
static void parse_type_args(Parser *p, PathSegment *seg) {
    if (!check(p, TOKEN_LT)) return;
    p->type_arg_depth++;
    advance(p);                      /* '<' */
    Array *args = array_new(sizeof(ASTNode *));
    if (!check(p, TOKEN_GT) && !check(p, TOKEN_EOF)) {
        for (;;) {
            ASTNode *arg = parse_type_ref(p);
            if (arg) array_push(args, &arg);
            if (!match(p, TOKEN_COMMA)) break;
        }
    }
    bool closed = consume_type_close(p);
    if (!closed && !p->quiet) {
        diag_error(p->diag, p->current,
                   "expected '>' to close the type arguments, found %s",
                   token_kind_name(p->current.kind));
        p->had_error = true;
    }
    p->type_arg_depth--;
    if (p->type_arg_depth == 0) p->pending_gt = false;
    /* Only a list that actually closed is a type argument list: `i < size()`
       must stay a comparison, not become `i<...>` because of the `(`. */
    seg->type_args = closed ? args : NULL;
}

/* `<T, K, ...>` after `type`: the parameter names of a generic declaration. */
static Array *parse_type_params(Parser *p) {
    Array *names = array_new(sizeof(char *));
    advance(p);                      /* '<' */
    for (;;) {
        if (!check(p, TOKEN_IDENTIFIER)) {
            if (!p->quiet) {
                diag_error(p->diag, p->current,
                           "expected a type parameter name, found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
            }
            break;
        }
        Token n = p->current;
        advance(p);
        char *name = strndup(n.start, n.length);
        array_push(names, &name);
        if (!match(p, TOKEN_COMMA)) break;
    }
    if (!match(p, TOKEN_GT) && !p->quiet) {
        diag_error(p->diag, p->current,
                   "expected '>' to close the type parameters, found %s",
                   token_kind_name(p->current.kind));
        p->had_error = true;
    }
    return names;
}

bool contains_interpolation(Token t) {
    for (size_t i = 0; i + 1 < t.length; i++) {
        if (t.start[i] == '$' && t.start[i + 1] == '{') {
            return true;
        }
    }
    return false;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Expand escape sequences in `len` bytes of `src` into a fresh arena buffer. */
static char *unescape(Arena *arena, const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)arena_alloc(arena, len + 1);
    if (!buf) { *out_len = 0; return NULL; }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '\\' || i + 1 >= len) {
            buf[j++] = src[i];
            continue;
        }
        i++;
        switch (src[i]) {
            case 'n':  buf[j++] = '\n';  break;
            case 't':  buf[j++] = '\t';  break;
            case 'r':  buf[j++] = '\r';  break;
            case '0':  buf[j++] = '\0';  break;
            case 'e':  buf[j++] = '\x1b'; break;
            case '\\': buf[j++] = '\\';  break;
            case '"':  buf[j++] = '"';  break;
            case '$':  buf[j++] = '$';   break;
            case 'x': {
                int hi = hex_digit(i + 1 < len ? src[i + 1] : '\0');
                int lo = hex_digit(i + 2 < len ? src[i + 2] : '\0');
                if (hi >= 0 && lo >= 0) { buf[j++] = (char)((hi << 4) | lo); i += 2; }
                else buf[j++] = 'x';
                break;
            }
            default: buf[j++] = src[i]; break;
        }
    }
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

static ASTNode *make_string_literal(Parser *p, Token t, const char *src, size_t len) {
    ASTNode *node = ast_node_new(p->arena, AST_LITERAL, t);
    size_t out = 0;
    node->as.literal.value = unescape(p->arena, src, len, &out);
    node->as.literal.len = out;
    return node;
}

/* A character literal is one byte. Like a number literal, the value is kept as
   decimal text in the node, so codegen loads it as a `byte` constant. */
static ASTNode *make_char_literal(Parser *p, Token t) {
    size_t out = 0;
    char *bytes = unescape(p->arena, t.start + 1, t.length >= 2 ? t.length - 2 : 0, &out);
    if (out != 1) {
        diag_error(p->diag, t, "a character literal holds exactly one character, found %zu",
                   out);
        p->had_error = true;
    }
    char *digits = (char *)arena_alloc(p->arena, 8);
    snprintf(digits, 8, "%d", (int)(unsigned char)(out ? bytes[0] : 0));
    ASTNode *node = ast_node_new(p->arena, AST_LITERAL, t);
    node->as.literal.value = digits;
    node->as.literal.len = strlen(digits);
    return node;
}

/* Parse one `${ ... }` body. The text points into the original source, so the
   sub-lexer is bounded to it and diagnostics keep real positions. */
static ASTNode *parse_interp_expr(Parser *p, Token at, const char *text, size_t len) {
    Lexer sub;
    lexer_init(&sub, text);
    sub.end = text + len;
    sub.line = at.line;

    Parser sp;
    parser_init(&sp, &sub, p->arena, p->diag);
    ASTNode *expr = parse_expr(&sp);
    if (sp.had_error) { p->had_error = true; return NULL; }
    if (sp.current.kind != TOKEN_EOF || expr == NULL) {
        diag_error(p->diag, at, "invalid interpolation expression");
        p->had_error = true;
        return NULL;
    }
    return expr;
}

ASTNode *parse_string(Parser *p) {
    Token t = p->previous;
    if (!contains_interpolation(t)) {
        return make_string_literal(p, t, t.start + 1, t.length >= 2 ? t.length - 2 : 0);
    }

    ASTNode *node = ast_node_new(p->arena, AST_INTERPOLATED_STRING, t);
    node->as.interpolated.parts = array_new(sizeof(ASTNode *));

    const char *src = t.start + 1;
    size_t n = t.length >= 2 ? t.length - 2 : 0;
    size_t seg = 0;
    size_t i = 0;
    while (i < n) {
        if (src[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (src[i] != '$' || i + 1 >= n || src[i + 1] != '{') { i++; continue; }

        if (i > seg) {
            ASTNode *lit = make_string_literal(p, t, src + seg, i - seg);
            array_push(node->as.interpolated.parts, &lit);
        }

        size_t j = i + 2;
        int depth = 1;
        while (j < n) {
            if (src[j] == '{') depth++;
            else if (src[j] == '}') { depth--; if (depth == 0) break; }
            j++;
        }
        if (j >= n) {
            diag_error(p->diag, t, "unterminated '${' in string");
            p->had_error = true;
            return node;
        }

        ASTNode *expr = parse_interp_expr(p, t, src + i + 2, j - (i + 2));
        if (expr) array_push(node->as.interpolated.parts, &expr);
        i = j + 1;
        seg = i;
    }

    if (n > seg) {
        ASTNode *lit = make_string_literal(p, t, src + seg, n - seg);
        array_push(node->as.interpolated.parts, &lit);
    }
    return node;
}

/* ------------------------------------------------------------------ */
/* Expressions (precedence climbing)                                  */
/* ------------------------------------------------------------------ */

static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_stmt(Parser *p);
static ASTNode *parse_block(Parser *p);
static ASTNode *parse_type_ref(Parser *p);
static ASTNode *parse_term(Parser *p);
static ASTNode *try_parse_func_literal(Parser *p);
static ASTNode *try_parse_type_alias(Parser *p);
static ASTNode *parse_extern_def(Parser *p);

static ASTNode *make_binary(Parser *p, ASTNode *left, Token op, ASTNode *right) {
    ASTNode *node = ast_node_new(p->arena, AST_BINARY_EXPR, op);
    node->as.binary.left = left;
    node->as.binary.op = op;
    node->as.binary.right = right;
    return node;
}

/* primary ::= INT | STRING | NAME | '(' expr ')' | '{' ... '}' */
static ASTNode *parse_primary(Parser *p) {
    Token t = p->current;

    if (t.kind == TOKEN_NUMBER || t.kind == TOKEN_FLOAT) {
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_LITERAL, t);
        /* The lexer validated the digit separators; drop them so everything
           downstream sees a plain number. */
        char *text = strndup(t.start, t.length);
        strip_digit_separators(text, 0);
        node->as.literal.value = text;
        node->as.literal.len = strlen(text);
        return node;
    }

    if (t.kind == TOKEN_STRING) {
        advance(p);
        return parse_string(p);
    }

    if (t.kind == TOKEN_CHAR) {
        advance(p);
        return make_char_literal(p, t);
    }

    /* `$name` is an explicit global reference; the rest of the chain (`$x.y`,
       `$f()`, `$a[i]`) is parsed as usual by `parse_postfix`. */
    if (t.kind == TOKEN_DOLLAR) {
        advance(p);
        if (!check(p, TOKEN_IDENTIFIER)) {
            diag_error(p->diag, p->current, "expected a name after '$', found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        Token name = p->current;
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_PATH, t);
        node->as.path.parts = array_new(sizeof(PathSegment));
        node->as.path.syms = NULL;
        node->as.path.is_global = true;
        PathSegment seg = { strndup(name.start, name.length), false, NULL };
        array_push(node->as.path.parts, &seg);
        return node;
    }

    if (t.kind == TOKEN_IDENTIFIER) {
        /* `extern NAME(...)` is a native function declaration. */
        if (is_keyword(t, "extern")) {
            return parse_extern_def(p);
        }

        /* `null` is the one value with no data; it is a literal. */
        if (is_keyword(t, "null")) {
            advance(p);
            return ast_node_new(p->arena, AST_NULL, t);
        }

        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_PATH, t);
        node->as.path.parts = array_new(sizeof(PathSegment));
        PathSegment seg = { strndup(t.start, t.length), false, NULL };
        array_push(node->as.path.parts, &seg);
        return node;
    }

    if (t.kind == TOKEN_LEFT_PAREN) {
        /* Ambiguous: could be a function literal `(params) { ... }`
           or a parenthesised expression. Try the function literal first. */
        ASTNode *func = try_parse_func_literal(p);
        if (func) return func;

        advance(p);
        ASTNode *expr = parse_expr(p);
        expect(p, TOKEN_RIGHT_PAREN, "to close the group");
        return expr;
    }

    if (t.kind == TOKEN_LEFT_BRACE) {
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_TABLE_LITERAL, t);
        node->as.table.fields = array_new(sizeof(ASTNode *));
        skip_newlines(p);
        while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
            ASTNode *field = parse_stmt(p);
            if (field) array_push(node->as.table.fields, &field);
            skip_newlines(p);
        }
        expect(p, TOKEN_RIGHT_BRACE, "to close the table literal");
        return node;
    }

    if (t.kind == TOKEN_LEFT_BRACKET) {
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_ARRAY_LITERAL, t);
        node->as.array.elements = array_new(sizeof(ASTNode *));
        node->as.array.repeat = NULL;
        node->as.array.count_expr = NULL;
        node->as.array.count = 0;
        node->as.array.count_const = false;
        if (!check(p, TOKEN_RIGHT_BRACKET) && !check(p, TOKEN_EOF)) {
            ASTNode *first = parse_stmt(p);
            /* `[expr; n]` is n copies of expr. */
            if (match(p, TOKEN_SEMICOLON)) {
                node->as.array.repeat = first;
                node->as.array.count_expr = parse_expr(p);
                expect(p, TOKEN_RIGHT_BRACKET, "to close the repeat literal");
                return node;
            }
            if (first) array_push(node->as.array.elements, &first);
            while (match(p, TOKEN_COMMA)) {
                ASTNode *elem = parse_stmt(p);
                if (elem) array_push(node->as.array.elements, &elem);
            }
        }
        expect(p, TOKEN_RIGHT_BRACKET, "to close the array literal");
        return node;
    }

    if (t.kind == TOKEN_COLON_COLON) {
        diag_error(p->diag, t,
                   "'::' cannot start an expression; reference a static member "
                   "with 'Type::name'");
        p->had_error = true;
        advance(p);
        return NULL;
    }

    /* Error: unexpected token. Consume it to make progress. */
    diag_error(p->diag, t, "unexpected %s", token_kind_name(t.kind));
    p->had_error = true;
    advance(p);
    return NULL;
}

/* `Name<...>` in an expression: type arguments are kept only when the closing
   `>` is followed by `::` (static access) or `(` (a call). Parsed speculatively
   so a comparison chain such as `a < b` is untouched; `> ::` is a syntax error
   today, so committing on `::` reinterprets nothing. The arguments attach to the
   last segment: `Box<int>()` names a type, `xs.map<int>(f)` a generic method. */
static void try_expression_type_args(Parser *p, ASTNode *left) {
    if (!left || left->kind != AST_PATH || left->as.path.is_global) return;
    if (array_length(left->as.path.parts) == 0 || !check(p, TOKEN_LT)) return;
    ParserState st = save_state(p);
    bool saved_quiet = p->quiet;
    p->quiet = true;
    PathSegment *seg = (PathSegment *)array_get(left->as.path.parts,
                                                array_length(left->as.path.parts) - 1);
    parse_type_args(p, seg);
    bool committed = seg->type_args &&
                     (check(p, TOKEN_COLON_COLON) || check(p, TOKEN_LEFT_PAREN));
    p->quiet = saved_quiet;
    if (!committed) {
        seg->type_args = NULL;
        restore_state(p, st);
    }
}

/* postfix ::= primary { '.' NAME | '::' NAME | '(' args ')' | '[' expr ']' } */
static ASTNode *parse_postfix(Parser *p) {
    ASTNode *left = parse_primary(p);

    /* A function literal is a closed primary: it does not take postfix
       member access. A following `::` therefore starts a new statement
       (a static member of the enclosing scope), not a postfix chain. */
    if (left && left->kind == AST_FUNC_LITERAL) return left;

    /* `Name<...>` in an expression (see `try_expression_type_args`). */
    try_expression_type_args(p, left);

    for (;;) {
        if (check(p, TOKEN_DOT) || check(p, TOKEN_COLON_COLON)) {
            bool is_static = check(p, TOKEN_COLON_COLON);
            advance(p);
            Token name = p->current;
            advance(p);
            if (left && left->kind == AST_PATH) {
                PathSegment seg = { strndup(name.start, name.length), is_static, NULL };
                array_push(left->as.path.parts, &seg);
                /* `xs.map<int>(f)`: the arguments follow the last segment. */
                try_expression_type_args(p, left);
            } else if (left) {
                /* Member access on a non-path expression, e.g. `1.to_str`. */
                ASTNode *node = ast_node_new(p->arena, AST_MEMBER_ACCESS, name);
                node->as.member_access.object = left;
                node->as.member_access.member = strndup(name.start, name.length);
                node->as.member_access.is_static = is_static;
                left = node;
            }
        } else if (match(p, TOKEN_LEFT_PAREN)) {
            ASTNode *node = ast_node_new(p->arena, AST_CALL, p->previous);
            node->as.call.callee = left;
            node->as.call.args = array_new(sizeof(ASTNode *));
            node->as.call.builtin = BUILTIN_NONE;
            while (!check(p, TOKEN_RIGHT_PAREN) && !check(p, TOKEN_EOF)) {
                ASTNode *arg = parse_expr(p);
                if (arg) array_push(node->as.call.args, &arg);
                if (!match(p, TOKEN_COMMA)) break; /* trailing comma allowed */
            }
            expect(p, TOKEN_RIGHT_PAREN, "to close the argument list");
            left = node;
        } else if (match(p, TOKEN_LEFT_BRACKET)) {
            ASTNode *node = ast_node_new(p->arena, AST_INDEX, p->previous);
            node->as.index.target = left;
            node->as.index.index = parse_expr(p);
            expect(p, TOKEN_RIGHT_BRACKET, "to close the index");
            left = node;
        } else {
            break;
        }
    }

    return left;
}

/* The `op=` spelling for an operator that cannot start an expression, or NULL.
   `x =* 2` is a compound assignment typed the wrong way around. */
static const char *compound_assignment_spelling(TokenKind kind) {
    switch (kind) {
        case TOKEN_STAR:        return "*=";
        case TOKEN_SLASH:       return "/=";
        case TOKEN_PERCENT:     return "%=";
        case TOKEN_AMP:         return "&=";
        case TOKEN_PIPE:        return "|=";
        case TOKEN_CARET:       return "^=";
        case TOKEN_SHIFT_LEFT:  return "<<=";
        case TOKEN_SHIFT_RIGHT: return ">>=";
        default:                return NULL;
    }
}

/* True when the current token starts right where the previous one ended, so
   `=-1` (adjacent) can be told from `= -1` (spaced). */
static bool tokens_touch(const Parser *p) {
    return p->previous.start && p->current.start &&
           p->current.start == p->previous.start + p->previous.length;
}

/* unary ::= ( '-' | '~' | 'not' ) unary | postfix
   Unary '+' is not part of the language: `x =+ 1` would read as `= (+1)`,
   and `+=` is what such a line means. The same goes for the other reversed
   assignment spellings (`=*`, `=/`, ...): no operator but `-` can start an
   expression, and `=-` is flagged because `-=` is almost always meant. */
static ASTNode *parse_unary(Parser *p) {
    if (check(p, TOKEN_MINUS) && p->previous.kind == TOKEN_EQUAL && tokens_touch(p)) {
        if (!p->quiet)
            diag_warning(p->diag, p->current,
                         "'=-' assigns the negation; to subtract in place, write '-='");
    }
    if (check(p, TOKEN_PLUS)) {
        if (!p->quiet) {
            diag_error(p->diag, p->current,
                       p->previous.kind == TOKEN_EQUAL
                           ? "'+' cannot start an expression; did you mean '+='?"
                           : "'+' cannot start an expression");
            p->had_error = true;
        }
        advance(p);
        return parse_unary(p);
    }
    const char *compound = compound_assignment_spelling(p->current.kind);
    if (compound) {
        if (!p->quiet) {
            if (p->previous.kind == TOKEN_EQUAL)
                diag_error(p->diag, p->current,
                           "%s cannot start an expression; did you mean '%s'?",
                           token_kind_name(p->current.kind), compound);
            else
                diag_error(p->diag, p->current, "%s cannot start an expression",
                           token_kind_name(p->current.kind));
            p->had_error = true;
        }
        advance(p);
        return parse_unary(p);
    }
    if (check(p, TOKEN_MINUS) || check(p, TOKEN_TILDE) ||
        is_keyword(p->current, "not")) {
        Token op = p->current;
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_UNARY_EXPR, op);
        node->as.unary.op = op;
        node->as.unary.operand = parse_unary(p);
        return node;
    }
    return parse_postfix(p);
}

/* sum ::= term { ( '+' | '-' ) term } */
static ASTNode *parse_sum(Parser *p) {
    ASTNode *left = parse_term(p);
    while (check(p, TOKEN_PLUS) || check(p, TOKEN_MINUS)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_term(p));
    }
    return left;
}

/* term ::= unary { ( '*' | '/' | '%' ) unary } */
static ASTNode *parse_term(Parser *p) {
    ASTNode *left = parse_unary(p);
    while (check(p, TOKEN_STAR) || check(p, TOKEN_SLASH) || check(p, TOKEN_PERCENT)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_unary(p));
    }
    return left;
}

/* shift ::= sum { ( '<<' | '>>' ) sum } */
static ASTNode *parse_shift(Parser *p) {
    ASTNode *left = parse_sum(p);
    while (check(p, TOKEN_SHIFT_LEFT) || check(p, TOKEN_SHIFT_RIGHT)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_sum(p));
    }
    return left;
}

/* The bitwise levels sit tighter than comparisons (Rust-style), so
   `flags & MASK != 0` is `(flags & MASK) != 0`, not C's `flags & (MASK != 0)`. */
static ASTNode *parse_bitand(Parser *p) {
    ASTNode *left = parse_shift(p);
    while (check(p, TOKEN_AMP)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_shift(p));
    }
    return left;
}

static ASTNode *parse_bitxor(Parser *p) {
    ASTNode *left = parse_bitand(p);
    while (check(p, TOKEN_CARET)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_bitand(p));
    }
    return left;
}

static ASTNode *parse_bitor(Parser *p) {
    ASTNode *left = parse_bitxor(p);
    while (check(p, TOKEN_PIPE)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_bitxor(p));
    }
    return left;
}

/* cmp ::= bitor [ ( '==' | '!=' | '<' | '>' | '<=' | '>=' ) bitor ] */
static ASTNode *parse_cmp(Parser *p) {
    ASTNode *left = parse_bitor(p);
    if (check(p, TOKEN_EQUAL_EQUAL) || check(p, TOKEN_BANG_EQUAL) ||
        check(p, TOKEN_LT) || check(p, TOKEN_GT) ||
        check(p, TOKEN_LESS_EQUAL) || check(p, TOKEN_GREATER_EQUAL)) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_bitor(p));
    }
    return left;
}

/* and ::= cmp { 'and' cmp } */
static ASTNode *parse_and(Parser *p) {
    ASTNode *left = parse_cmp(p);
    while (is_keyword(p->current, "and")) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_cmp(p));
    }
    return left;
}

/* or ::= and { 'or' and } */
static ASTNode *parse_or(Parser *p) {
    ASTNode *left = parse_and(p);
    while (is_keyword(p->current, "or")) {
        Token op = p->current;
        advance(p);
        left = make_binary(p, left, op, parse_and(p));
    }
    return left;
}

static ASTNode *parse_expr(Parser *p) {
    return parse_or(p);
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

/* Try to parse an explicit binding: `Type name = value`.
   Distinguished from `name = value` by the extra identifier. A `(` can start a
   function type (`(int) -> int f = ...`), so it is tried too; the guess is
   restored if it does not pan out. */
static ASTNode *try_parse_explicit_bind(Parser *p) {
    if (!check(p, TOKEN_IDENTIFIER) && !check(p, TOKEN_LEFT_PAREN)) return NULL;

    ParserState s = save_state(p);
    bool saved_quiet = p->quiet;
    p->quiet = true;   /* a failed guess must not report diagnostics */
    ASTNode *type = parse_type_ref(p);
    p->quiet = saved_quiet;
    if (!type || !check(p, TOKEN_IDENTIFIER)) {
        restore_state(p, s);
        return NULL;
    }

    Token name = p->current;
    advance(p);
    if (!match(p, TOKEN_EQUAL)) {
        restore_state(p, s);
        return NULL;
    }

    ASTNode *value = parse_expr(p);
    ASTNode *node = ast_node_new(p->arena, AST_BIND, name);
    node->as.bind.type = type;
    node->as.bind.name = strndup(name.start, name.length);
    node->as.bind.value = value;
    return node;
}

static ASTNode *parse_stmt(Parser *p) {
    /* Static member declaration inside a table literal: `::name = value` */
    if (check(p, TOKEN_COLON_COLON)) {
        Token t = p->current;
        advance(p);
        if (!check(p, TOKEN_IDENTIFIER)) {
            diag_error(p->diag, p->current, "expected a name after '::', found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        Token name = p->current;
        advance(p);
        if (!match(p, TOKEN_EQUAL)) {
            diag_error(p->diag, p->current,
                       "expected '=' after static member name (to reference a static "
                       "member use 'Type::name'), found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        ASTNode *value = parse_expr(p);
        ASTNode *node = ast_node_new(p->arena, AST_MEMBER_ASSIGN, t);
        node->as.member_assign.target = NULL;
        node->as.member_assign.member = strndup(name.start, name.length);
        node->as.member_assign.value = value;
        node->as.member_assign.is_static = true;
        return node;
    }

    /* use "path" */
    if (is_keyword(p->current, "use")) {
        Token t = p->current;
        advance(p);
        if (!check(p, TOKEN_STRING)) {
            diag_error(p->diag, p->current, "expected a string after 'use', found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        Token path = p->current;
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_USE, t);
        /* Strip the surrounding quotes from the string token. */
        node->as.use.path = strndup(path.start + 1, path.length - 2);
        return node;
    }

    /* if expr block [ else ( if ... | block ) ] */
    if (is_keyword(p->current, "if")) {
        Token t = p->current;
        advance(p);
        ASTNode *cond = parse_expr(p);
        ASTNode *then_branch = parse_block(p);
        ASTNode *else_branch = NULL;
        skip_newlines(p);
        if (is_keyword(p->current, "else")) {
            advance(p);
            if (is_keyword(p->current, "if")) {
                else_branch = parse_stmt(p); /* else if */
            } else {
                else_branch = parse_block(p);
            }
        }
        ASTNode *node = ast_node_new(p->arena, AST_IF, t);
        node->as.if_stmt.cond = cond;
        node->as.if_stmt.then_branch = then_branch;
        node->as.if_stmt.else_branch = else_branch;
        return node;
    }

    /* while expr block */
    if (is_keyword(p->current, "while")) {
        Token t = p->current;
        advance(p);
        ASTNode *cond = parse_expr(p);
        ASTNode *body = parse_block(p);
        ASTNode *node = ast_node_new(p->arena, AST_WHILE, t);
        node->as.while_stmt.cond = cond;
        node->as.while_stmt.body = body;
        return node;
    }

    /* for ( init ; cond ; step ) block — C-style, with a mandatory header. */
    if (is_keyword(p->current, "for")) {
        Token t = p->current;
        advance(p);
        expect(p, TOKEN_LEFT_PAREN, "after 'for'");
        ASTNode *init = parse_stmt(p);
        expect(p, TOKEN_SEMICOLON, "after the for-loop initializer");
        ASTNode *cond = parse_expr(p);
        expect(p, TOKEN_SEMICOLON, "after the for-loop condition");
        ASTNode *step = parse_stmt(p);
        expect(p, TOKEN_RIGHT_PAREN, "to close the for-loop header");
        ASTNode *body = parse_block(p);
        ASTNode *node = ast_node_new(p->arena, AST_FOR, t);
        node->as.for_stmt.init = init;
        node->as.for_stmt.cond = cond;
        node->as.for_stmt.step = step;
        node->as.for_stmt.body = body;
        return node;
    }

    /* switch expr { case value block ... default block } — no fallthrough. */
    if (is_keyword(p->current, "switch")) {
        Token t = p->current;
        advance(p);
        ASTNode *expr = parse_expr(p);
        skip_newlines(p);
        expect(p, TOKEN_LEFT_BRACE, "to open the switch body");
        ASTNode *node = ast_node_new(p->arena, AST_SWITCH, t);
        node->as.switch_stmt.expr = expr;
        node->as.switch_stmt.cases = array_new(sizeof(ASTNode *));
        node->as.switch_stmt.default_body = NULL;
        skip_newlines(p);
        while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
            if (is_keyword(p->current, "case")) {
                Token ct = p->current;
                advance(p);
                ASTNode *value = parse_expr(p);
                ASTNode *range_hi = NULL;
                if (match(p, TOKEN_DOT_DOT)) range_hi = parse_expr(p);
                ASTNode *guard = NULL;
                if (is_keyword(p->current, "if")) {
                    advance(p);
                    guard = parse_expr(p);
                }
                ASTNode *body = parse_block(p);
                ASTNode *c = ast_node_new(p->arena, AST_CASE, ct);
                c->as.case_stmt.value = value;
                c->as.case_stmt.range_hi = range_hi;
                c->as.case_stmt.guard = guard;
                c->as.case_stmt.body = body;
                array_push(node->as.switch_stmt.cases, &c);
            } else if (is_keyword(p->current, "default")) {
                Token dt = p->current;
                advance(p);
                if (node->as.switch_stmt.default_body) {
                    diag_error(p->diag, dt, "duplicate 'default' in a switch body");
                    p->had_error = true;
                }
                ASTNode *dbody = parse_block(p);
                if (!node->as.switch_stmt.default_body)
                    node->as.switch_stmt.default_body = dbody;
            } else {
                diag_error(p->diag, p->current,
                           "expected 'case' or 'default' in a switch body, found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
                advance(p);
            }
            skip_newlines(p);
        }
        expect(p, TOKEN_RIGHT_BRACE, "to close the switch body");
        return node;
    }

    /* return [ expr ] */
    if (is_keyword(p->current, "return")) {
        Token t = p->current;
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_RETURN, t);
        node->as.ret.value = NULL;
        if (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF) &&
            !check(p, TOKEN_NEWLINE)) {
            node->as.ret.value = parse_expr(p);
        }
        return node;
    }

    /* break */
    if (is_keyword(p->current, "break")) {
        Token t = p->current;
        advance(p);
        return ast_node_new(p->arena, AST_BREAK, t);
    }

    /* continue */
    if (is_keyword(p->current, "continue")) {
        Token t = p->current;
        advance(p);
        return ast_node_new(p->arena, AST_CONTINUE, t);
    }

    /* local [ Type ] name = value — declares in the current block scope. */
    if (is_keyword(p->current, "local")) {
        advance(p);

        /* `local Type name = value` shares the explicit-bind grammar. */
        ASTNode *bind = try_parse_explicit_bind(p);
        if (bind) {
            bind->as.bind.is_local = true;
            return bind;
        }

        /* `local name = value` infers the type from the value. */
        if (!check(p, TOKEN_IDENTIFIER)) {
            diag_error(p->diag, p->current, "expected a name after 'local', found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        Token name = p->current;
        advance(p);
        if (!match(p, TOKEN_EQUAL)) {
            diag_error(p->diag, p->current,
                       "expected '=' after the local name, found %s",
                       token_kind_name(p->current.kind));
            p->had_error = true;
            return NULL;
        }
        ASTNode *value = parse_expr(p);
        ASTNode *node = ast_node_new(p->arena, AST_BIND, name);
        node->as.bind.type = NULL;
        node->as.bind.name = strndup(name.start, name.length);
        node->as.bind.value = value;
        node->as.bind.is_local = true;
        return node;
    }

    /* Explicit binding: `Type name = value`. */
    ASTNode *explicit_bind = try_parse_explicit_bind(p);
    if (explicit_bind) return explicit_bind;

    /* Function-type alias: `Name = <function type>`, or `Name<T> = ...` for a
       generic one (`plans/done/function_type_aliases.md`). */
    ASTNode *type_alias = try_parse_type_alias(p);
    if (type_alias) return type_alias;

    /* Otherwise: an expression, optionally followed by '=' (assignment). */
    ASTNode *expr = parse_expr(p);
    if (expr == NULL) return NULL;

    /* Compound assignment: `target op= value`. Sema rewrites it into an
       assignment whose value is `T::op=(target, value)` when the target's type
       defines that operator, else `target op value`. */
    if (check(p, TOKEN_PLUS_EQUAL) || check(p, TOKEN_MINUS_EQUAL) ||
        check(p, TOKEN_STAR_EQUAL) || check(p, TOKEN_SLASH_EQUAL) ||
        check(p, TOKEN_PERCENT_EQUAL) || check(p, TOKEN_AMP_EQUAL) ||
        check(p, TOKEN_PIPE_EQUAL) || check(p, TOKEN_CARET_EQUAL) ||
        check(p, TOKEN_SHIFT_LEFT_EQUAL) || check(p, TOKEN_SHIFT_RIGHT_EQUAL)) {
        Token op = p->current;
        advance(p);
        ASTNode *value = parse_expr(p);
        ASTNode *node = ast_node_new(p->arena, AST_COMPOUND_ASSIGN, expr->token);
        node->as.compound.target = expr;
        node->as.compound.op = op;
        node->as.compound.value = value;
        return node;
    }

    if (match(p, TOKEN_EQUAL)) {
        /* `name = <T, ...>(params) -> ret { ... }` defines a generic function
           (`plans/done/generics.md` Phase 7). */
        Array *fn_type_params = check(p, TOKEN_LT) ? parse_type_params(p) : NULL;

        /* `name = type { ... }` defines a nominal obj type; `name = type<T>` a
           generic one (parsed here, resolved in `plans/done/generics.md` Phase 3). */
        if (is_keyword(p->current, "type")) {
            Token t = p->current;
            advance(p);
            Array *type_params = check(p, TOKEN_LT) ? parse_type_params(p) : NULL;
            if (!check(p, TOKEN_LEFT_BRACE)) {
                diag_error(p->diag, p->current, "expected '{' after 'type', found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
                return NULL;
            }
            ASTNode *body = parse_primary(p);
            ASTNode *node = ast_node_new(p->arena, AST_TYPE_DEF, t);
            node->as.type_def.target = expr;
            node->as.type_def.body = body;
            node->as.type_def.type_params = type_params;
            return node;
        }

        /* `name = enum { ... }` defines an enum type. */
        if (is_keyword(p->current, "enum")) {
            Token t = p->current;
            advance(p);
            if (!check(p, TOKEN_LEFT_BRACE)) {
                diag_error(p->diag, p->current, "expected '{' after 'enum', found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
                return NULL;
            }
            advance(p); /* '{' */
            Array *items = array_new(sizeof(EnumItem));
            skip_newlines(p);
            while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
                if (!check(p, TOKEN_IDENTIFIER)) {
                    diag_error(p->diag, p->current,
                               "expected an enum constant name, found %s",
                               token_kind_name(p->current.kind));
                    p->had_error = true;
                    break;
                }
                Token name = p->current;
                advance(p);
                EnumItem item;
                item.name = strndup(name.start, name.length);
                item.value = NULL;
                item.auto_value = 0;
                if (match(p, TOKEN_EQUAL)) {
                    item.value = parse_expr(p);
                }
                array_push(items, &item);
                match(p, TOKEN_COMMA); /* optional comma */
                skip_newlines(p);
            }
            expect(p, TOKEN_RIGHT_BRACE, "to close the enum");
            ASTNode *node = ast_node_new(p->arena, AST_ENUM_DEF, t);
            node->as.enum_def.target = expr;
            node->as.enum_def.items = items;
            return node;
        }

        /* `name = packed BASE[N] { ... }` defines a packed type. */
        if (is_keyword(p->current, "packed")) {
            Token t = p->current;
            advance(p);
            ASTNode *typeref = parse_type_ref(p);
            if (!typeref || typeref->kind != AST_ARRAY_TYPE ||
                !typeref->as.array_type.has_length) {
                diag_error(p->diag, t, "expected 'BASE[N]' after 'packed', found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
                return NULL;
            }
            ASTNode *body = NULL;
            if (check(p, TOKEN_LEFT_BRACE)) body = parse_primary(p);
            ASTNode *node = ast_node_new(p->arena, AST_PACKED_DEF, t);
            node->as.packed_def.target = expr;
            node->as.packed_def.element = typeref->as.array_type.element;
            node->as.packed_def.count = typeref->as.array_type.length;
            node->as.packed_def.body = body;
            return node;
        }

        ASTNode *value = parse_expr(p);

        if (fn_type_params) {
            if (!value || value->kind != AST_FUNC_LITERAL) {
                diag_error(p->diag, p->current,
                           "expected a function literal after the type parameters, found %s",
                           token_kind_name(p->current.kind));
                p->had_error = true;
                return NULL;
            }
            value->as.func.type_params = fn_type_params;
        }

        if (expr->kind == AST_PATH && array_length(expr->as.path.parts) > 1) {
            /* Member assignment: target.member = value */
            PathSegment seg = { NULL, false, NULL };
            array_pop(expr->as.path.parts, &seg);
            ASTNode *node = ast_node_new(p->arena, AST_MEMBER_ASSIGN, expr->token);
            node->as.member_assign.target = expr;
            node->as.member_assign.member = seg.name ? seg.name : "unknown";
            node->as.member_assign.value = value;
            node->as.member_assign.is_static = seg.is_static;
            return node;
        }

        if (expr->kind == AST_MEMBER_ACCESS) {
            /* Member assignment on a non-path target: `expr.member = value`. */
            ASTNode *node = ast_node_new(p->arena, AST_MEMBER_ASSIGN, expr->token);
            node->as.member_assign.target = expr->as.member_access.object;
            node->as.member_assign.member = expr->as.member_access.member;
            node->as.member_assign.value = value;
            node->as.member_assign.is_static = expr->as.member_access.is_static;
            return node;
        }

        /* Simple assignment: name = value */
        ASTNode *node = ast_node_new(p->arena, AST_ASSIGN, expr->token);
        node->as.assign.target = expr;
        node->as.assign.value = value;
        return node;
    }

    return expr;
}

/* ------------------------------------------------------------------ */
/* Function literals                                                  */
/* ------------------------------------------------------------------ */

/* typeRef ::= qname [ '<' typeRef { ',' typeRef } '>' ] [ '[' NUMBER? ']' ]
             | '(' [ typeRef { ',' typeRef } ] ')' '->' typeRef   (a function type)

   Deliberately NOT parse_postfix: a type reference must not swallow calls,
   literals or `extern` declarations, or `try_parse_explicit_bind` would
   misread `extern foo()` as the type of a binding. */
/* Array type suffix: `T[n]` (fixed) or `T[]` (growable). Shared by the path and
   grouped-type readings so a parenthesized function type can be suffixed
   (`((int) -> int)[]`). */
static ASTNode *parse_type_suffix(Parser *p, ASTNode *element) {
    if (!check(p, TOKEN_LEFT_BRACKET)) return element;
    Token lb = p->current;
    advance(p);
    ASTNode *arr = ast_node_new(p->arena, AST_ARRAY_TYPE, lb);
    arr->as.array_type.element = element;
    arr->as.array_type.length = 0;
    arr->as.array_type.has_length = false;
    if (check(p, TOKEN_NUMBER)) {
        Token num = p->current;
        advance(p);
        arr->as.array_type.length = (size_t)atoi(num.start);
        arr->as.array_type.has_length = true;
    }
    expect(p, TOKEN_RIGHT_BRACKET, "to close the array type");
    return arr;
}

static ASTNode *parse_type_ref(Parser *p) {
    /* Function type: `(T, U) -> R`. Parens not followed by an arrow group one
       type instead, so a function type can be parenthesized and suffixed. */
    if (check(p, TOKEN_LEFT_PAREN)) {
        Token lp = p->current;
        advance(p);
        ASTNode *node = ast_node_new(p->arena, AST_FUNC_TYPE, lp);
        node->as.func_type.params = array_new(sizeof(ASTNode *));
        node->as.func_type.ret = NULL;
        bool saw_comma = false;
        if (!check(p, TOKEN_RIGHT_PAREN) && !check(p, TOKEN_EOF)) {
            for (;;) {
                ASTNode *pt = parse_type_ref(p);
                if (!pt) break;
                array_push(node->as.func_type.params, &pt);
                if (!match(p, TOKEN_COMMA)) break;
                saw_comma = true;
            }
        }
        expect(p, TOKEN_RIGHT_PAREN, "to close the function type");
        if (!check(p, TOKEN_ARROW) && !saw_comma &&
            array_length(node->as.func_type.params) == 1) {
            /* No arrow: these parens grouped a single type. */
            ASTNode *inner = *(ASTNode **)array_get(node->as.func_type.params, 0);
            return parse_type_suffix(p, inner);
        }
        expect(p, TOKEN_ARROW, "before the function type's return type");
        node->as.func_type.ret = parse_type_ref(p);
        return node;
    }

    if (!check(p, TOKEN_IDENTIFIER)) return NULL;

    Token t = p->current;
    advance(p);
    ASTNode *node = ast_node_new(p->arena, AST_PATH, t);
    node->as.path.parts = array_new(sizeof(PathSegment));
    PathSegment seg = { strndup(t.start, t.length), false, NULL };
    parse_type_args(p, &seg);
    array_push(node->as.path.parts, &seg);

    while (check(p, TOKEN_COLON_COLON) || check(p, TOKEN_DOT)) {
        bool is_static = check(p, TOKEN_COLON_COLON);
        advance(p);
        if (!check(p, TOKEN_IDENTIFIER)) break;
        Token n = p->current;
        advance(p);
        PathSegment s = { strndup(n.start, n.length), is_static, NULL };
        parse_type_args(p, &s);
        array_push(node->as.path.parts, &s);
    }

    /* Array type: `T[n]` (fixed) or `T[]` (growable). */
    return parse_type_suffix(p, node);
}

/* typeAlias ::= NAME [ '<' TYPE_PARAM { ',' TYPE_PARAM } '>' ] '=' funcType
   The right side must be a full function type — an arrow and a result — that
   ends the statement. No expression can look like that (a parenthesised value
   cannot hold a top-level comma, and `->` is not an operator), and a function
   literal has a `{` body, so an alias is unambiguous
   (`plans/done/function_type_aliases.md`). */
static ASTNode *try_parse_type_alias(Parser *p) {
    if (!check(p, TOKEN_IDENTIFIER)) return NULL;

    ParserState s = save_state(p);
    bool saved_quiet = p->quiet;
    p->quiet = true;

    Token name = p->current;
    advance(p);
    Array *params = NULL;
    if (check(p, TOKEN_LT)) params = parse_type_params(p);

    ASTNode *typeref = NULL;
    bool ok = params == NULL || array_length(params) > 0;
    if (ok && match(p, TOKEN_EQUAL)) {
        typeref = parse_type_ref(p);
        bool ends = check(p, TOKEN_NEWLINE) || check(p, TOKEN_RIGHT_BRACE) ||
                    check(p, TOKEN_EOF);
        ok = typeref && typeref->kind == AST_FUNC_TYPE &&
             typeref->as.func_type.ret && ends;
    } else {
        ok = false;
    }
    p->quiet = saved_quiet;
    if (!ok) { restore_state(p, s); return NULL; }

    ASTNode *alias = ast_node_new(p->arena, AST_TYPE_ALIAS, name);
    alias->as.type_alias.name = strndup(name.start, name.length);
    alias->as.type_alias.type = typeref;
    alias->as.type_alias.type_params = params;
    return alias;
}

/* param ::= [ typeRef ] NAME */
static ASTNode *parse_param(Parser *p) {
    ASTNode *first = parse_type_ref(p);
    if (!first) return NULL;

    const char *name = NULL;
    ASTNode *type = NULL;

    if (check(p, TOKEN_IDENTIFIER)) {
        /* `type name` */
        Token n = p->current;
        advance(p);
        name = strndup(n.start, n.length);
        type = first;
    } else if (first->kind == AST_PATH && array_length(first->as.path.parts) == 1) {
        /* bare `name` */
        PathSegment *seg = (PathSegment *)array_get(first->as.path.parts, 0);
        name = seg->name;
    } else {
        /* receiver type only, e.g. `(int)` */
        type = first;
    }

    ASTNode *node = ast_node_new(p->arena, AST_PARAM, first->token);
    node->as.param.type = type;
    node->as.param.name = name;
    return node;
}

/* block ::= '{' { stmt } '}' */
static ASTNode *parse_block(Parser *p) {
    skip_newlines(p);
    Token t = p->current;
    if (!match(p, TOKEN_LEFT_BRACE)) return NULL;

    ASTNode *node = ast_node_new(p->arena, AST_BLOCK, t);
    node->as.block.stmts = array_new(sizeof(ASTNode *));

    skip_newlines(p);
    while (!check(p, TOKEN_RIGHT_BRACE) && !check(p, TOKEN_EOF)) {
        ASTNode *stmt = parse_stmt(p);
        if (stmt) array_push(node->as.block.stmts, &stmt);
        skip_newlines(p);
    }
    expect(p, TOKEN_RIGHT_BRACE, "to close the block");
    return node;
}

/* funcDef ::= '(' [ param { ',' param } ] ')' [ '->' typeRef ] block */
static ASTNode *try_parse_func_literal(Parser *p) {
    if (!check(p, TOKEN_LEFT_PAREN)) return NULL;

    ParserState s = save_state(p);
    bool saved_quiet = p->quiet;
    p->quiet = true;   /* a failed guess must not report diagnostics */
    Token start = p->current;
    advance(p); /* '(' */

    Array *params = array_new(sizeof(ASTNode *));
    if (!check(p, TOKEN_RIGHT_PAREN)) {
        do {
            ASTNode *param = parse_param(p);
            if (!param) { p->quiet = saved_quiet; restore_state(p, s); return NULL; }
            array_push(params, &param);
        } while (match(p, TOKEN_COMMA));
    }

    if (!match(p, TOKEN_RIGHT_PAREN)) { p->quiet = saved_quiet; restore_state(p, s); return NULL; }

    ASTNode *ret_type = NULL;
    if (match(p, TOKEN_ARROW)) {
        ret_type = parse_type_ref(p);
    }

    if (!check(p, TOKEN_LEFT_BRACE)) { p->quiet = saved_quiet; restore_state(p, s); return NULL; }
    p->quiet = saved_quiet;
    ASTNode *body = parse_block(p);

    ASTNode *node = ast_node_new(p->arena, AST_FUNC_LITERAL, start);
    node->as.func.params = params;
    node->as.func.ret_type = ret_type;
    node->as.func.body = body;
    return node;
}

/* externDef ::= 'extern' NAME '(' [ param { ',' param } ] ')' [ '->' typeRef ] */
static ASTNode *parse_extern_def(Parser *p) {
    Token start = p->current; /* 'extern' */
    advance(p);

    if (!check(p, TOKEN_IDENTIFIER)) {
        diag_error(p->diag, p->current, "expected a name after 'extern', found %s",
                   token_kind_name(p->current.kind));
        p->had_error = true;
        return NULL;
    }
    Token name = p->current;
    advance(p);

    if (!match(p, TOKEN_LEFT_PAREN)) {
        diag_error(p->diag, p->current, "expected '(' after extern name, found %s",
                   token_kind_name(p->current.kind));
        p->had_error = true;
        return NULL;
    }

    Array *params = array_new(sizeof(ASTNode *));
    if (!check(p, TOKEN_RIGHT_PAREN)) {
        do {
            ASTNode *param = parse_param(p);
            if (!param) break;
            array_push(params, &param);
        } while (match(p, TOKEN_COMMA));
    }
    expect(p, TOKEN_RIGHT_PAREN, "to close the parameter list");

    ASTNode *ret_type = NULL;
    if (match(p, TOKEN_ARROW)) {
        ret_type = parse_type_ref(p);
    }

    ASTNode *node = ast_node_new(p->arena, AST_EXTERN_DEF, start);
    node->as.extern_def.name = strndup(name.start, name.length);
    node->as.extern_def.params = params;
    node->as.extern_def.ret_type = ret_type;
    return node;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

void parser_init(Parser *p, Lexer *lexer, Arena *arena, Diagnostics *diag) {
    p->lexer = lexer;
    p->arena = arena;
    p->diag = diag;
    p->had_error = false;
    p->quiet = false;
    p->type_arg_depth = 0;
    p->pending_gt = false;
    /* Seed both tokens with a safe sentinel: the first advance copies
       `current` into `previous`, so `current` must not be garbage. */
    Token sentinel;
    sentinel.kind = TOKEN_EOF;
    sentinel.start = NULL;
    sentinel.length = 0;
    sentinel.line = 0;
    sentinel.message = NULL;
    p->current = sentinel;
    p->previous = sentinel;
    advance(p); /* Load first token */
}

/* Error recovery: skip the rest of the current line so one mistake does not
   cascade into a flood of follow-on errors. */
static void synchronize(Parser *p) {
    while (p->current.kind != TOKEN_EOF && p->current.kind != TOKEN_NEWLINE) {
        advance(p);
    }
    if (p->current.kind == TOKEN_NEWLINE) advance(p);
}

ASTNode *parse_program(Parser *p) {
    ASTNode *program = ast_node_new(p->arena, AST_PROGRAM, p->current);
    program->as.program.stmts = array_new(sizeof(ASTNode *));

    skip_newlines(p);
    while (p->current.kind != TOKEN_EOF) {
        int before = p->diag ? p->diag->error_count : 0;
        ASTNode *stmt = parse_stmt(p);
        if (stmt) {
            array_push(program->as.program.stmts, &stmt);
        }
        if (p->diag && p->diag->error_count > before) {
            synchronize(p);
        }
        skip_newlines(p);
    }
    return program;
}