#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "compiler/sema.h"
#include "compiler/parser.h"
#include "compiler/diag.h"
#include "native/native.h"
#include "core/embed.h"
#include "util.h"

/* Directory holding the prelude: `.oby` files always injected before the
   user script. Overridable at build time. */
#ifndef OBY_PRELUDE_DIR
#define OBY_PRELUDE_DIR "src/prelude"
#endif

static Type *check_expr(Sema *s, ASTNode *node, Scope *scope);
static Type *lookup_builtin(Sema *s, const char *name);

/* Used by the numeric rules, which sit above these definitions. */
static bool is_numeric(Type *t);
static bool is_float_type(Type *t);

/* If `node` is a constant integer expression (a literal, optionally negated),
   return its value. Used for enum auto-increment and constant counts. */
static bool int_const_value(ASTNode *node, long long *out) {
    if (!node) return false;
    if (node->kind == AST_LITERAL && node->token.kind == TOKEN_NUMBER) {
        unsigned long long v;
        if (!parse_int_literal(node->as.literal.value, &v)) return false;
        *out = (long long)v;
        return true;
    }
    if (node->kind == AST_UNARY_EXPR && node->as.unary.op.kind == TOKEN_MINUS) {
        long long v;
        if (int_const_value(node->as.unary.operand, &v)) { *out = -v; return true; }
    }
    /* A resolved enum constant. */
    if (node->kind == AST_PATH && node->sym && node->sym->has_const) {
        *out = (long)node->sym->const_value;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/* How deep generic instantiation may nest before it is taken for runaway
   recursion (`Foo<T>` whose body names `Foo<T[]>` again, and so on). */
#define SEMA_MAX_INST_DEPTH 64

/* Instantiation (`plans/done/generics.md` Phase 3). Defined below, once the type
   body collectors are available. */
static Type *resolve_type_args(Sema *s, Symbol *sym, Array *args, Scope *scope, Token at);
static Type *segment_type(Sema *s, Symbol *sym, PathSegment *seg, Scope *scope, Token at);
static void collect_table_members(Sema *s, ASTNode *table, Type *owner, Scope *scope);
static void check_table(Sema *s, ASTNode *table, Type *owner, Scope *scope);

/* Generic functions (`plans/done/generics.md` Phase 7). */
static Symbol *resolve_generic_call(Sema *s, Symbol *tmpl, Array *written_args,
                                    Type **arg_types, size_t argc,
                                    Scope *scope, Token at, Type *owner_inst);

static void sema_error(Sema *s, Token tok, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_error(s->diag, tok, "%s", buf);
    s->had_error = true;
}

static void sema_warning(Sema *s, Token tok, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_warning(s->diag, tok, "%s", buf);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *simple_path_name(ASTNode *node) {
    if (!node || node->kind != AST_PATH) return NULL;
    if (array_length(node->as.path.parts) != 1) return NULL;
    PathSegment *seg = (PathSegment *)array_get(node->as.path.parts, 0);
    return seg->name;
}

/* The member name of a compound assignment's operator (`+=` -> "+="). */
static const char *compound_op_name(TokenKind k) {
    switch (k) {
        case TOKEN_PLUS_EQUAL:    return "+=";
        case TOKEN_MINUS_EQUAL:   return "-=";
        case TOKEN_STAR_EQUAL:    return "*=";
        case TOKEN_SLASH_EQUAL:   return "/=";
        case TOKEN_PERCENT_EQUAL: return "%=";
        case TOKEN_AMP_EQUAL:     return "&=";
        case TOKEN_PIPE_EQUAL:    return "|=";
        case TOKEN_CARET_EQUAL:   return "^=";
        case TOKEN_SHIFT_LEFT_EQUAL:  return "<<=";
        case TOKEN_SHIFT_RIGHT_EQUAL: return ">>=";
        default:                  return "?=";
    }
}

/* The binary operator a compound assignment resolves (`+=` -> `+`). */
static TokenKind compound_base_op(TokenKind k) {
    switch (k) {
        case TOKEN_PLUS_EQUAL:    return TOKEN_PLUS;
        case TOKEN_MINUS_EQUAL:   return TOKEN_MINUS;
        case TOKEN_STAR_EQUAL:    return TOKEN_STAR;
        case TOKEN_SLASH_EQUAL:   return TOKEN_SLASH;
        case TOKEN_PERCENT_EQUAL: return TOKEN_PERCENT;
        case TOKEN_AMP_EQUAL:     return TOKEN_AMP;
        case TOKEN_PIPE_EQUAL:    return TOKEN_PIPE;
        case TOKEN_CARET_EQUAL:   return TOKEN_CARET;
        case TOKEN_SHIFT_LEFT_EQUAL:  return TOKEN_SHIFT_LEFT;
        case TOKEN_SHIFT_RIGHT_EQUAL: return TOKEN_SHIFT_RIGHT;
        default:                  return TOKEN_PLUS;
    }
}

/* A path node naming the first `count` segments of `path`. */
static ASTNode *path_prefix(Sema *s, ASTNode *path, size_t count) {
    ASTNode *n = ast_node_new(s->arena, AST_PATH, path->token);
    n->as.path.parts = array_new(sizeof(PathSegment));
    n->as.path.syms = array_new(sizeof(Symbol *));
    n->as.path.is_global = path->as.path.is_global;
    for (size_t i = 0; i < count; i++) {
        PathSegment *seg = (PathSegment *)array_get(path->as.path.parts, i);
        array_push(n->as.path.parts, seg);
        if (path->as.path.syms && i < array_length(path->as.path.syms)) {
            Symbol *sym = *(Symbol **)array_get(path->as.path.syms, i);
            array_push(n->as.path.syms, &sym);
        }
    }
    size_t n_syms = array_length(n->as.path.syms);
    if (n_syms > 0) {
        Symbol *last = *(Symbol **)array_get(n->as.path.syms, n_syms - 1);
        n->sym = last;
        n->type = last->type;
    }
    return n;
}

/* Signedness is a property of the type's name in OBY: `byte` and names
   starting with `u` are unsigned, everything else packed is signed. */
/* True if every value of `from` is representable in `to` — the conversion
   cannot change the value. Same signedness only widens; an unsigned source
   fits a strictly wider signed one (byte -> int, uint -> int64); a signed
   source never fits an unsigned one (its negatives do not). */
static bool int_nests(Type *to, Type *from) {
    bool to_u = type_is_unsigned(to), from_u = type_is_unsigned(from);
    if (to_u == from_u) return to->size >= from->size;
    if (!from_u) return false;               /* signed -> unsigned */
    return from->size < to->size;            /* unsigned -> signed */
}

/* An integer literal (optionally negated): `*neg` is the sign, `*mag` the
   magnitude. Returns false when `node` is not exactly that, or the value does
   not fit 64 bits. Unlike `int_const_value` this is unsigned, so it reaches the
   whole `uint64` range. */
static bool literal_int_value(ASTNode *node, bool *neg, unsigned long long *mag) {
    bool n = false;
    if (node && node->kind == AST_UNARY_EXPR && node->as.unary.op.kind == TOKEN_MINUS) {
        node = node->as.unary.operand;
        n = true;
    }
    if (!node || node->kind != AST_LITERAL || node->token.kind != TOKEN_NUMBER)
        return false;
    unsigned long long u;
    if (!parse_int_literal(node->as.literal.value, &u)) return false;
    *neg = n;
    *mag = u;
    return true;
}

/* The largest magnitude a scalar integer type holds. */
static unsigned long long int_type_max(Type *t) {
    if (type_is_unsigned(t))
        return t->size >= 8 ? 18446744073709551615ull
             : t->size >= 4 ? 4294967295ull : 255ull;
    return t->size >= 8 ? 9223372036854775807ull
         : t->size >= 4 ? 2147483647ull : 127ull;
}

/* True if an integer literal of this sign and magnitude is representable in a
   scalar integer type. */
static bool literal_fits(Type *t, bool neg, unsigned long long mag) {
    if (!t || t->kind != TYPE_PACKED || t->is_float || t->count > 0) return false;
    unsigned long long max = int_type_max(t);
    if (type_is_unsigned(t)) return !neg && mag <= max;
    return neg ? mag <= max + 1 : mag <= max;
}

/* The type an integer literal takes in the position `expect`: `expect` itself
   when the value fits, `unknown` (with a diagnostic) when it does not, and
   NULL when this is not an integer literal or `expect` is not an integer
   type — the ordinary rules then apply. */
static Type *adapt_literal_to(Sema *s, ASTNode *node, Type *expect) {
    bool neg;
    unsigned long long mag;
    if (!literal_int_value(node, &neg, &mag)) return NULL;
    if (!expect || expect->kind == TYPE_ENUM || !is_numeric(expect) ||
        is_float_type(expect))
        return NULL;
    if (!literal_fits(expect, neg, mag)) {
        char tn[64];
        type_str(expect, tn, sizeof(tn));
        sema_error(s, node->token, "value %s%llu does not fit '%s'",
                   neg ? "-" : "", mag, tn);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    node->type = expect;
    return expect;
}

/* A literal operand takes the other operand's integer type when the two types
   are not already compatible (`u + 1`); otherwise the ordinary promotion
   handles the widening (`i + i64`). */
static void adapt_operand_literal(Sema *s, ASTNode *side, Type **type, Type *other) {
    Type *t = *type;
    if (!t || !other) return;
    if (t->kind == TYPE_UNKNOWN || other->kind == TYPE_UNKNOWN) return;
    if (!is_numeric(t) || !is_numeric(other)) return;
    if (is_float_type(t) || is_float_type(other)) return;
    if (int_nests(t, other) || int_nests(other, t)) return;
    Type *nt = adapt_literal_to(s, side, other);
    if (nt) *type = nt;
}

static bool is_float_type(Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_ENUM && t->base) return is_float_type(t->base);
    return t->kind == TYPE_PACKED && t->is_float;
}

static bool is_numeric(Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_ENUM) return is_numeric(t->base);
    /* A packed aggregate (a user packed type) is storage, not a number. */
    return t->kind == TYPE_PACKED && t->count == 0;
}

/* A user packed type: `count` elements of `base`, stored inline. */
static bool is_packed_agg(Type *t) {
    return t && t->kind == TYPE_PACKED && t->count > 0;
}

/* True for a type stored as a tagged `Value` on the stack, so `null` fits it.
   Packed values are raw bytes and can never be null. */
static bool is_tagged_type(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TYPE_OBJ:
        case TYPE_STRING:
        case TYPE_ARRAY:
        case TYPE_FUNC:
        case TYPE_NULL:
            return true;
        default:
            return false;
    }
}

/* An inferred type that came from `null` alone is unusable — nothing is
   assignable to it — so such a declaration has to name a type. */
static bool null_inferred(Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_NULL) return true;
    if (t->kind == TYPE_ARRAY) return t->base && t->base->kind == TYPE_NULL;
    return false;
}

/* ------------------------------------------------------------------ */
/* Path / file helpers                                                */
/* ------------------------------------------------------------------ */

static const char *path_dir(const char *path, Arena *arena) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (!slash) return ".";
    size_t len = (size_t)(slash - path);
    char *dir = (char *)arena_alloc(arena, len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char *path_join(Arena *arena, const char *dir, const char *rel) {
    size_t dl = strlen(dir), rl = strlen(rel);
    char *out = (char *)arena_alloc(arena, dl + rl + 2);
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, rel, rl);
    out[dl + 1 + rl] = '\0';
    return out;
}

typedef struct {
    size_t start;
    size_t len;
} path_seg_t;

/* Fold the spellings of one path onto one string (`\` -> `/`, drop `.` and
   empty segments, cancel `segment/..`) so the pragma-once check sees the same
   file through `./`, `..` and mixed separators. Purely lexical: a `..` that
   has nothing to cancel is kept, so `../a` stays `../a`. */
static char *path_normalize(Arena *arena, const char *path) {
    size_t len = strlen(path);
    bool absolute = len > 0 && (path[0] == '/' || path[0] == '\\');
    path_seg_t *segs = (path_seg_t *)arena_alloc(arena, (len + 1) * sizeof(path_seg_t));
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (path[i] == '/' || path[i] == '\\')) i++;
        size_t start = i;
        while (i < len && path[i] != '/' && path[i] != '\\') i++;
        size_t slen = i - start;
        if (slen == 0) continue;                               /* "//" */
        if (slen == 1 && path[start] == '.') continue;         /* "." */
        if (slen == 2 && path[start] == '.' && path[start + 1] == '.') {
            path_seg_t *prev = n > 0 ? &segs[n - 1] : NULL;
            bool prev_is_dotdot = prev && prev->len == 2 &&
                                  path[prev->start] == '.' && path[prev->start + 1] == '.';
            if (prev && !prev_is_dotdot) {                     /* `a/..` cancels */
                n--;
                continue;
            }
        }
        segs[n].start = start;
        segs[n].len = slen;
        n++;
    }

    size_t out_len = absolute ? 1 : 0;
    for (size_t k = 0; k < n; k++) out_len += segs[k].len + (k > 0 ? 1 : 0);
    if (out_len == 0) {
        char *dot = (char *)arena_alloc(arena, 2);
        dot[0] = '.';
        dot[1] = '\0';
        return dot;
    }
    char *out = (char *)arena_alloc(arena, out_len + 1);
    size_t o = 0;
    if (absolute) out[o++] = '/';
    for (size_t k = 0; k < n; k++) {
        if (k > 0) out[o++] = '/';
        memcpy(out + o, path + segs[k].start, segs[k].len);
        o += segs[k].len;
    }
    out[o] = '\0';
    return out;
}

/* Sources are looked up in the embedded table first (the prelude is baked
   into the binary, so it is found from any working directory), then on disk.
   The copy is malloc'd and NUL-terminated, like `read_file`'s. */
static char *source_read(const char *path) {
    const EmbeddedFile *file = embed_lookup(path);
    if (!file) return read_file(path);
    char *copy = (char *)malloc(file->len + 1);
    if (!copy) return NULL;
    memcpy(copy, file->data, file->len);
    copy[file->len] = '\0';
    return copy;
}

static bool source_exists(const char *path) {
    if (embed_lookup(path)) return true;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* Type resolution                                                    */
/* ------------------------------------------------------------------ */

static Type *resolve_path(Sema *s, ASTNode *node, Scope *scope);
static Symbol *lookup_type_name(Scope *scope, const char *name);

/* Inside a method, a bare name that names one of the receiver's members means
   `receiver.member`: a body can only reach its own instance. */
static Symbol *receiver_member(Sema *s, const char *name) {
    Symbol *recv = s->cur_receiver;
    if (!recv || !recv->type || !recv->type->members) return NULL;
    Symbol *m = scope_lookup_local(recv->type->members, name);
    return (m && m->kind == SYM_MEMBER && !m->is_static) ? m : NULL;
}

/* Rewrite a bare `name` into the `receiver.name` path shape, so the rest of the
   check walks the receiver like any other value. */
static void bind_bare_to_receiver(Sema *s, ASTNode *path) {
    PathSegment seg;
    seg.name = (char *)s->cur_receiver->name;
    seg.is_static = false;
    seg.type_args = NULL;
    array_insert(path->as.path.parts, 0, &seg);
}

/* The name a receiver is written with. An instance is written as its
   template's name — `(Box b)` inside `Box<T>` — not its display name
   (`Box<int>`), which is not an identifier. */
static const char *owner_decl_name(Type *owner) {
    if (!owner) return NULL;
    return owner->origin ? owner->origin->name : owner->name;
}

/* True when the enclosing owner is a `{}` literal's structural type: unlike a
   nominal type, its name cannot be written in source. */
static bool owner_is_nameless(Sema *s) {
    if (!s->cur_owner) return true;
    /* An instance is named exactly when its template is. */
    Type *decl = s->cur_owner->origin ? s->cur_owner->origin : s->cur_owner;
    if (!decl->name) return true;
    Symbol *t = lookup_type_name(s->global, decl->name);
    return !(t && t->kind == SYM_TYPE && t->type == decl);
}

/* A member of an instance reached by bare name where this function has no
   receiver: there is no `self` to read or call it from. */
static void no_receiver_error(Sema *s, Token at, const char *name) {
    if (owner_is_nameless(s)) {
        /* A `{}` literal's type has no name, so a receiver cannot be written
           for its function members at all. */
        sema_error(s, at,
                   "'%s' means 'self.%s' here, but a `{}` literal's type has no "
                   "name, so its functions cannot take a receiver",
                   name, name);
        return;
    }
    const char *type = owner_decl_name(s->cur_owner);
    sema_error(s, at,
               "'%s' means 'self.%s' here, but this function has no receiver; "
               "write 'value.%s', or take one as in '(%s value)'",
               name, name, name, type ? type : "T");
}

/* `name(...)` inside a method, where `name` is a member of the receiver and not
   a local or a function, means `receiver.name(...)`. Rewrites the callee in
   place so the method-call sugar picks it up. */
static void rewrite_bare_callee(Sema *s, ASTNode *callee, Scope *scope) {
    if (!callee || callee->kind != AST_PATH || callee->as.path.is_global) return;
    if (array_length(callee->as.path.parts) != 1) return;
    PathSegment *first = (PathSegment *)array_get(callee->as.path.parts, 0);
    Symbol *found = scope_lookup(scope, first->name);
    if (found && !(found->kind == SYM_MEMBER && !found->is_static)) return;
    if (!receiver_member(s, first->name)) {
        if (found) no_receiver_error(s, callee->token, first->name);
        return;
    }
    bind_bare_to_receiver(s, callee);
}

/* A path node naming the receiver, for a rewritten bare member reference. */
static ASTNode *receiver_path(Sema *s, Token at) {
    ASTNode *recv = ast_node_new(s->arena, AST_PATH, at);
    recv->as.path.parts = array_new(sizeof(PathSegment));
    PathSegment seg;
    seg.name = (char *)s->cur_receiver->name;
    seg.is_static = false;
    seg.type_args = NULL;
    array_push(recv->as.path.parts, &seg);
    recv->as.path.is_global = false;
    recv->as.path.syms = NULL;
    return recv;
}

/* `name = value` inside a method, where `name` is a member of the receiver and
   not a local, is `receiver.name = value`. Rewrites the node in place and
   returns true (the caller re-checks it as a member assign). */
static bool rewrite_bare_assign(Sema *s, ASTNode *node, Scope *scope) {
    ASTNode *target = node->as.assign.target;
    ASTNode *value = node->as.assign.value;
    if (!target || target->kind != AST_PATH || target->as.path.is_global) return false;
    if (array_length(target->as.path.parts) != 1) return false;
    PathSegment *first = (PathSegment *)array_get(target->as.path.parts, 0);
    Symbol *found = scope_lookup(scope, first->name);
    if (found && !(found->kind == SYM_MEMBER && !found->is_static)) return false;
    if (!receiver_member(s, first->name)) return false;

    node->kind = AST_MEMBER_ASSIGN;
    node->as.member_assign.target = receiver_path(s, target->token);
    node->as.member_assign.member = first->name;
    node->as.member_assign.value = value;
    node->as.member_assign.is_static = false;
    node->sym = NULL;
    return true;
}

/* Define a value binding. A local or parameter remembers the function that
   declared it, so a reference from a nested function can be recognised as a
   capture (see `note_capture`). */
/* Record a declaration's name token on its symbol, for tooling (the language
   server's go-to-definition). */
static Symbol *sym_tok(Symbol *sym, Token tok) {
    if (sym) sym->decl_tok = tok;
    return sym;
}

static Symbol *define_value(Sema *s, Scope *scope, const char *name, Type *type,
                            Token tok) {
    Symbol *sym = scope_define(s->arena, scope, name, type, SYM_VALUE, false);
    if (sym) {
        sym->def_func = (scope == s->global) ? -1 : s->cur_func;
        sym->decl_tok = tok;
    }
    return sym;
}

/* True if `sym` is a local of an enclosing function seen from the function being
   checked — i.e. a captured variable. */
static bool is_captured(Sema *s, Symbol *sym) {
    return sym && sym->kind == SYM_VALUE && !sym->is_global && !sym->has_const &&
           sym->def_func >= 0 && sym->def_func != s->cur_func;
}

/* Record that `sym` — a local of an enclosing function — is captured. Every
   function literal between the declaration and here must carry it, so that the
   innermost creation site can copy its value. */
static void note_capture(Sema *s, Symbol *sym) {
    if (!is_captured(s, sym)) return;
    for (size_t i = array_length(s->func_stack); i > 0; i--) {
        ASTNode *lit = *(ASTNode **)array_get(s->func_stack, i - 1);
        if (lit->as.func.func_id <= sym->def_func) break;   /* not nested inside it */
        if (!lit->as.func.upvalues) lit->as.func.upvalues = array_new(sizeof(Symbol *));
        bool seen = false;
        for (size_t j = 0; j < array_length(lit->as.func.upvalues); j++) {
            if (*(Symbol **)array_get(lit->as.func.upvalues, j) == sym) { seen = true; break; }
        }
        if (!seen) array_push(lit->as.func.upvalues, &sym);
    }
}

static Type *resolve_type_node(Sema *s, ASTNode *node, Scope *scope) {
    if (!node) return s->types.void_type;
    if (node->kind == AST_PATH) return resolve_path(s, node, scope);
    if (node->kind == AST_ARRAY_TYPE) {
        Type *elem = resolve_type_node(s, node->as.array_type.element, scope);
        size_t len = node->as.array_type.has_length ? node->as.array_type.length : 0;
        node->type = type_array(&s->types, elem, len);
        return node->type;
    }
    if (node->kind == AST_FUNC_TYPE) {
        size_t n = array_length(node->as.func_type.params);
        Type **pts = (Type **)arena_alloc(s->arena, sizeof(Type *) * (n ? n : 1));
        for (size_t i = 0; i < n; i++)
            pts[i] = resolve_type_node(s, *(ASTNode **)array_get(node->as.func_type.params, i),
                                       scope);
        Type *ret = node->as.func_type.ret
                        ? resolve_type_node(s, node->as.func_type.ret, scope)
                        : s->types.void_type;
        node->type = type_func(&s->types, pts, n, ret);
        return node->type;
    }
    return s->types.unknown;
}

static Type *resolve_path(Sema *s, ASTNode *node, Scope *scope) {
    Array *parts = node->as.path.parts;
    size_t n = array_length(parts);
    if (n == 0) return s->types.unknown;

    /* Record the resolved symbol for each segment so codegen can emit loads
       without re-resolving. */
    node->as.path.syms = array_new(sizeof(Symbol *));

    PathSegment *first = (PathSegment *)array_get(parts, 0);
    /* The segment that carries any `<...>` arguments (a rewrite below may
       insert a receiver in front of it). */
    PathSegment *type_seg = first;
    /* `$name` is an explicit global: it resolves in the global scope only. */
    Symbol *sym = node->as.path.is_global ? scope_lookup_local(s->global, first->name)
                                          : scope_lookup(scope, first->name);
    if (sym && node->as.path.is_global && sym->kind == SYM_TYPE) {
        sema_error(s, node->token, "'%s' is a type, and '$' names a global value",
                   first->name);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    /* The enclosing type's members are visible inside a member function body,
       so a sibling by bare name resolves. With a receiver it means that
       receiver's member; without one there is nothing to read it from. */
    if (sym && !node->as.path.is_global && s->func_depth > 0 &&
        sym->kind == SYM_MEMBER && !sym->is_static) {
        if (!receiver_member(s, first->name)) {
            no_receiver_error(s, node->token, first->name);
            node->type = s->types.unknown;
            return s->types.unknown;
        }
        bind_bare_to_receiver(s, node);
        n++;
        first = (PathSegment *)array_get(parts, 0);
        sym = scope_lookup(scope, first->name);
    }
    /* Inside a function a bare name never reaches a global *variable*: those are
       written `$name`. Global functions stay visible (a call is not state). */
    if (sym && !node->as.path.is_global && s->func_depth > 0 &&
        sym->is_global && sym->kind == SYM_VALUE) {
        sema_error(s, node->token,
                   "'%s' is a global; inside a function read it as '$%s'",
                   first->name, first->name);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    if (!sym) {
        /* A bare name that names a member of the receiver means that member
           (the members of the enclosing type are not always in scope, e.g. for
           `T::m = ...` outside the type body). */
        if (!node->as.path.is_global && s->func_depth > 0 &&
            receiver_member(s, first->name)) {
            bind_bare_to_receiver(s, node);
            n++;
            first = (PathSegment *)array_get(parts, 0);
            sym = scope_lookup(scope, first->name);
        }
    }
    if (!sym) {
        sema_error(s, node->token,
                   node->as.path.is_global ? "unknown global '%s'" : "unknown name '%s'",
                   first->name);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    /* A generic function is a template, not a value: it has no storage. */
    if (sym->is_generic) {
        sema_error(s, node->token,
                   "'%s' is a generic function; call it (its type arguments are "
                   "inferred from the arguments, or name them: '%s<int>(...)').",
                   first->name, first->name);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    array_push(node->as.path.syms, &sym);
    /* An instance member's declared value is evaluated at construction time,
       where there is no instance to read a sibling from yet. */
    if (s->in_default > 0 && sym->kind == SYM_MEMBER && !sym->is_static) {
        sema_error(s, node->token,
                   "a member default cannot read another member ('%s')", first->name);
        node->type = s->types.unknown;
        return s->types.unknown;
    }
    note_capture(s, sym);

    /* Inside a `{}` literal's field values the owner's members are searched
       first, so a member that shares a name with a type or a local/parameter of
       the enclosing scope silently wins. Flag that. */
    if (s->field_enclosing && sym->kind == SYM_MEMBER) {
        Symbol *outer = scope_lookup(s->field_enclosing, first->name);
        if (outer && (outer->kind == SYM_TYPE ||
                      (outer->kind == SYM_VALUE && !outer->is_global)))
            sema_error(s, node->token, "member '%s' shadows %s of the same name",
                       first->name, outer->kind == SYM_TYPE ? "a type" : "a local");
    }

    Type *cur = segment_type(s, sym, type_seg, scope, node->token);
    for (size_t i = 1; i < n; i++) {
        PathSegment *seg = (PathSegment *)array_get(parts, i);
        Token tok = (seg->tok.start && seg->tok.length) ? seg->tok : node->token;
        /* `.` follows a value, `::` follows a type. (A value receiver keeps
           both: `v::static_fn` calls the static without the receiver.) */
        if (sym->kind == SYM_TYPE && !seg->is_static) {
            sema_error(s, tok, "use '::' to access a member of type '%s'",
                       sym->name);
            node->type = s->types.unknown;
            return s->types.unknown;
        }
        if (!cur || !cur->members) {
            const char *tname = (cur && cur->name) ? cur->name : "value";
            sema_error(s, tok, "type '%s' has no member '%s'", tname, seg->name);
            cur = s->types.unknown;
            break;
        }
        Symbol *m = scope_lookup_local(cur->members, seg->name);
        if (!m) {
            const char *tname = (cur && cur->name) ? cur->name : "value";
            sema_error(s, tok, "type '%s' has no member '%s'", tname, seg->name);
            cur = s->types.unknown;
            break;
        }
        array_push(node->as.path.syms, &m);
        sym = m;
        cur = segment_type(s, m, seg, scope, node->token);
    }

    node->sym = sym;
    node->type = cur;
    return cur;
}

/* ------------------------------------------------------------------ */
/* Assignability / promotion                                          */
/* ------------------------------------------------------------------ */

/* Numeric coercion is width-based: a value fits a target if the target is
   at least as wide. Signedness only matters for arithmetic (see promote). */
static bool is_void_type(Type *t) {
    return t && t->kind == TYPE_VOID;
}

static bool assignable(Sema *s, Type *target, Type *value) {
    (void)s;
    if (!target || !value) return true;
    if (target == value) return true;
    if (target->kind == TYPE_UNKNOWN || value->kind == TYPE_UNKNOWN) return true;
    /* `void` is not a value: only a `void` expression fits a `void` target
       (`return f()` inside a `-> void` function). */
    if (value->kind == TYPE_VOID) return target->kind == TYPE_VOID;
    /* `null` fits any tagged type; nothing but `null` fits the `null` type. */
    if (value->kind == TYPE_NULL) return is_tagged_type(target);
    if (target->kind == TYPE_NULL) return value->kind == TYPE_NULL;
    if (target->kind == TYPE_OBJ && target->name && strcmp(target->name, "object") == 0)
        return !is_packed_agg(value); /* the base object accepts any tagged value */
    /* An enum target accepts only its own constants (not a bare int). */
    if (target->kind == TYPE_ENUM) return value->kind == TYPE_ENUM && target == value;
    /* An enum value is assignable to its underlying type. */
    if (value->kind == TYPE_ENUM) return assignable(s, target, value->base);
    if (is_numeric(target) && is_numeric(value)) {
        bool tf = is_float_type(target);
        bool vf = is_float_type(value);
        if (tf && vf) return true;                          /* float <-> double */
        if (tf && !vf) return target->size >= value->size;  /* int -> float */
        if (!tf && vf) return false;                        /* float -> int is lossy */
        return int_nests(target, value);                    /* value-preserving only */
    }
    if (target->kind == TYPE_ARRAY && value->kind == TYPE_ARRAY) {
        /* A known length must match; 0 means unknown (growable). */
        if (target->size != 0 && value->size != 0 && target->size != value->size) return false;
        return assignable(s, target->base, value->base);
    }
    /* Function types are structural: same arity, and each parameter and the
       return type assignable (lenient, matching numeric coercion). */
    if (target->kind == TYPE_FUNC && value->kind == TYPE_FUNC) {
        if (target->param_count != value->param_count) return false;
        for (size_t i = 0; i < target->param_count; i++)
            if (!assignable(s, target->params[i], value->params[i])) return false;
        return assignable(s, target->ret, value->ret);
    }
    return false;
}

static Type *promote(Sema *s, Type *a, Type *b, Token tok) {
    if (!a || !b) return s->types.unknown;
    if (a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return s->types.unknown;
    if ((tok.kind == TOKEN_PERCENT || tok.kind == TOKEN_AMP || tok.kind == TOKEN_PIPE ||
         tok.kind == TOKEN_CARET) &&
        (is_float_type(a) || is_float_type(b))) {
        sema_error(s, tok, "'%.*s' needs integer operands", (int)tok.length, tok.start);
        return s->types.unknown;
    }
    if (a == b) return a;
    if (is_numeric(a) && is_numeric(b)) {
        bool af = is_float_type(a);
        bool bf = is_float_type(b);
        if (af || bf) {
            if (af && bf) return a->size >= b->size ? a : b;
            Type *f = af ? a : b;
            Type *i = af ? b : a;
            /* Widen to double when the integer is wider than the float. */
            if (i->size > f->size) return s->types.double_type;
            return f;
        }
        if (int_nests(a, b)) return a;
        if (int_nests(b, a)) return b;
        char an[64], bn[64];
        type_str(a, an, sizeof(an));
        type_str(b, bn, sizeof(bn));
        sema_error(s, tok, "cannot mix '%s' and '%s'; convert one side explicitly",
                   an, bn);
        return s->types.unknown;
    }
    return s->types.unknown;
}

/* ------------------------------------------------------------------ */
/* Function type construction                                         */
/* ------------------------------------------------------------------ */

static Type *func_type_from_params(Sema *s, Array *params, ASTNode *ret_node,
                                   Scope *scope) {
    size_t n = array_length(params);
    Type **ptypes = (Type **)arena_alloc(s->arena, sizeof(Type *) * (n ? n : 1));
    for (size_t i = 0; i < n; i++) {
        ASTNode *p = *(ASTNode **)array_get(params, i);
        Type *pt;
        if (p->as.param.type) {
            pt = resolve_type_node(s, p->as.param.type, scope);
        } else if (p->as.param.name) {
            /* A bare name that matches a type is an implicit receiver. */
            Symbol *sym = scope_lookup(scope, p->as.param.name);
            pt = (sym && sym->kind == SYM_TYPE) ? sym->type : s->types.unknown;
        } else {
            pt = s->types.unknown;
        }
        p->type = pt;
        ptypes[i] = pt;
    }
    Type *ret = ret_node ? resolve_type_node(s, ret_node, scope) : s->types.void_type;
    return type_func(&s->types, ptypes, n, ret);
}

/* Natives cross the tagged boundary, so their signatures cannot mention packed
   aggregates yet (there is no boxed representation for them). */
static void check_native_signature(Sema *s, Type *ft, Token tok) {
    if (!ft || ft->kind != TYPE_FUNC) return;
    for (size_t i = 0; i < ft->param_count; i++) {
        if (is_packed_agg(ft->params[i]))
            sema_error(s, tok, "native parameters cannot be packed types yet ('%s')",
                       ft->params[i]->name);
    }
    if (is_packed_agg(ft->ret))
        sema_error(s, tok, "native return types cannot be packed types yet ('%s')",
                   ft->ret->name);
}

/* ------------------------------------------------------------------ */
/* Phase A0: load imports and register all type names                 */
/* ------------------------------------------------------------------ */

static void collect_types(Sema *s, ASTNode *program, const char *dir);

/* Load an already-resolved file: parse it and collect its types. `from_tok`
   describes the `use` that pulled it in (line 0 = not from a `use`, e.g. the
   prelude). */
static void load_resolved(Sema *s, const char *resolved, Token from_tok) {
    for (size_t i = 0; i < array_length(s->loaded); i++) {
        char *seen = *(char **)array_get(s->loaded, i);
        if (strcmp(seen, resolved) == 0) return; /* pragma once */
    }
    size_t rl = strlen(resolved);
    char *stored = (char *)arena_alloc(s->arena, rl + 1);
    memcpy(stored, resolved, rl + 1);
    array_push(s->loaded, &stored);

    char *src = source_read(resolved);
    if (!src) {
        diag_error(s->diag, from_tok, "cannot read import \"%s\"", resolved);
        s->had_error = true;
        return;
    }

    const char *saved_file = s->diag->file;
    const char *saved_src = s->diag->source;
    if (from_tok.line > 0) diag_push_include(s->diag, saved_file, from_tok.line);
    diag_set_source(s->diag, resolved, src);

    Lexer lexer;
    lexer_init(&lexer, src);
    Parser parser;
    parser_init(&parser, &lexer, s->arena, s->diag);
    ASTNode *prog = parse_program(&parser);
    if (parser.had_error) {
        s->had_error = true;
        diag_set_source(s->diag, saved_file, saved_src);
        if (from_tok.line > 0) diag_pop_include(s->diag);
        return;
    }

    ProgramUnit *u = (ProgramUnit *)arena_alloc(s->arena, sizeof(ProgramUnit));
    u->program = prog;
    u->file = resolved;
    u->source = src;
    /* Collect this file's types first: that loads its `use`s, which are pushed
       ahead of it, so a file is compiled after everything it imports. */
    collect_types(s, prog, path_dir(resolved, s->arena));
    array_push(s->programs, &u);

    diag_set_source(s->diag, saved_file, saved_src);
    if (from_tok.line > 0) diag_pop_include(s->diag);
}

static void load_use(Sema *s, ASTNode *use_node, const char *dir) {
    const char *rel = use_node->as.use.path;
    Token tok = use_node->token;

    /* An import is relative to the file that writes it — never to the working
       directory — and normalized so pragma-once sees one spelling per file. */
    char *resolved = path_normalize(s->arena, path_join(s->arena, dir, rel));
    if (!source_exists(resolved)) {
        diag_error(s->diag, tok, "cannot resolve import \"%s\"", rel);
        s->had_error = true;
        return;
    }
    load_resolved(s, resolved, tok);
}

/* Load the prelude entry point, `core.oby`. It `use`s whatever else it needs,
   so the prelude is one file plus its imports. Always injected before the user
   script. */
static void load_prelude(Sema *s) {
    char *path = path_normalize(s->arena, path_join(s->arena, OBY_PRELUDE_DIR, "core.oby"));
    Token tok;
    tok.kind = TOKEN_EOF;
    tok.start = path;
    tok.length = strlen(path);
    tok.line = 0;
    tok.message = NULL;
    load_resolved(s, path, tok);
}

static Symbol *resolve_path_symbol(Array *parts, size_t count, Scope *scope);

/* A single uppercase letter is reserved for generic type parameters (`Box<T>`,
   `Pair<A, B>`), so it cannot be defined as a type name. */
static bool type_name_is_reserved(Sema *s, const char *name, Token at) {
    if (name && name[0] >= 'A' && name[0] <= 'Z' && name[1] == '\0') {
        sema_error(s, at, "type name '%s' is reserved for generic parameters", name);
        return true;
    }
    return false;
}

/* `NAME = packed BASE[N] { alias = NAME[K] }` defines a packed type: `N`
   elements of `BASE`, byte-tight. The body is optional; each entry maps a
   member name to an element index. */
static void collect_packed_def(Sema *s, ASTNode *stmt) {
    const char *name = simple_path_name(stmt->as.packed_def.target);
    if (!name) return;
    if (type_name_is_reserved(s, name, stmt->as.packed_def.target->token)) return;
    if (scope_lookup_local(s->global, name)) {
        sema_error(s, stmt->as.packed_def.target->token, "cannot redefine '%s'", name);
        return;
    }

    ASTNode *elem_node = stmt->as.packed_def.element;
    Symbol *esym = NULL;
    if (elem_node && elem_node->kind == AST_PATH) {
        esym = resolve_path_symbol(elem_node->as.path.parts,
                                   array_length(elem_node->as.path.parts), s->global);
    }
    if (!esym || esym->kind != SYM_TYPE) {
        sema_error(s, stmt->token, "packed type '%s' needs a type as its element", name);
        return;
    }
    Type *elem = esym->type;
    if (elem->kind != TYPE_PACKED && elem->kind != TYPE_ENUM) {
        sema_error(s, stmt->token, "packed element type must be packed, got '%s'", elem->name);
        return;
    }

    size_t count = stmt->as.packed_def.count;
    if (count == 0) {
        sema_error(s, stmt->token, "packed type '%s' must have at least one element", name);
        return;
    }
    if (count * type_size(elem) > 255) {
        sema_error(s, stmt->token, "packed type '%s' is too large (%zu bytes, max 255)",
                   name, count * type_size(elem));
        return;
    }

    Type *t = type_packed_aggregate(&s->types, name, elem, count);
    stmt->type = t;
    sym_tok(scope_define(s->arena, s->global, name, t, SYM_TYPE, false),
            stmt->as.packed_def.target->token);

    /* Member aliases: `x = NAME[0]` maps `x` to element 0. */
    ASTNode *body = stmt->as.packed_def.body;
    if (!body || body->kind != AST_TABLE_LITERAL) return;
    Array *fields = body->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;
        const char *mname = field->kind == AST_ASSIGN
                                ? simple_path_name(field->as.assign.target) : NULL;
        ASTNode *value = field->kind == AST_ASSIGN ? field->as.assign.value : NULL;
        if (!mname || !value || value->kind != AST_INDEX ||
            value->as.index.target->kind != AST_PATH) {
            sema_error(s, field->token, "packed member aliases look like 'name = %s[index]'", name);
            continue;
        }
        long long v;
        if (!int_const_value(value->as.index.index, &v)) {
            sema_error(s, value->token, "packed member index must be an integer literal");
            continue;
        }
        if (v < 0 || (size_t)v >= count) {
            sema_error(s, value->token, "index %lld is out of range for '%s' (%zu elements)",
                       v, name, count);
            continue;
        }
        long index = (long)v;
        Symbol *isym = resolve_path_symbol(value->as.index.target->as.path.parts,
                                           array_length(value->as.index.target->as.path.parts),
                                           s->global);
        if (!isym || isym->kind != SYM_TYPE || isym->type != t) {
            sema_error(s, value->token, "packed member alias must index '%s'", name);
            continue;
        }
        Symbol *sym = scope_define(s->arena, t->members, mname, elem, SYM_MEMBER, false);
        if (sym) {
            sym->slot = (int)index;
            sym->decl_tok = field->token;
        }
        value->as.index.const_index = index;
        value->as.index.has_const_index = true;
        field->type = elem;
        field->sym = sym;
    }
}

static Type *resolve_prefix_type(Sema *s, Array *parts, size_t count, Scope *scope,
                                 Token at);

/* A type parameter list must be unique, and must not reuse a name that is
   already in scope — the same rule a variable follows for type names. Validated
   where the declaration is registered, so "in scope" means what is declared
   before it (a later type named `T` does not clash). */
static void validate_type_params(Sema *s, Token at, Array *params) {
    for (size_t i = 0; i < array_length(params); i++) {
        const char *pn = *(char **)array_get(params, i);
        for (size_t j = 0; j < i; j++) {
            if (strcmp(pn, *(char **)array_get(params, j)) == 0) {
                sema_error(s, at, "duplicate type parameter '%s'", pn);
                break;
            }
        }
        if (scope_lookup_local(s->global, pn))
            sema_error(s, at, "cannot reuse the name '%s' as a type parameter", pn);
    }
}

/* Declaration-time check for a generic alias (`plans/done/function_type_aliases.md`):
   the aliased type is not built — its parameters have no arguments yet — but
   every type name on the right side must be a parameter or a known type,
   `Name<...>` must match the target's arity, and the alias must not name
   itself. Resolving it would build placeholder instantiations, so this walks
   the type syntax instead. */
static void check_alias_type_names(Sema *s, ASTNode *node, Scope *params, Symbol *self) {
    if (!node) return;

    if (node->kind == AST_ARRAY_TYPE) {
        check_alias_type_names(s, node->as.array_type.element, params, self);
        return;
    }
    if (node->kind == AST_FUNC_TYPE) {
        Array *ps = node->as.func_type.params;
        for (size_t i = 0; i < array_length(ps); i++)
            check_alias_type_names(s, *(ASTNode **)array_get(ps, i), params, self);
        check_alias_type_names(s, node->as.func_type.ret, params, self);
        return;
    }
    if (node->kind != AST_PATH) return;

    Array *parts = node->as.path.parts;
    if (array_length(parts) == 0) return;
    PathSegment *first = (PathSegment *)array_get(parts, 0);
    Symbol *sym = scope_lookup(params, first->name);
    if (!sym || sym->kind != SYM_TYPE) {
        sema_error(s, node->token, "unknown type '%s'", first->name);
        return;
    }
    if (self && sym == self) {
        sema_error(s, node->token, "alias '%s' cannot refer to itself", first->name);
        return;
    }
    Type *t = sym->type;
    if (t && t->origin) t = t->origin;
    bool generic = t && t->is_template;
    if (first->type_args) {
        if (!generic) {
            sema_error(s, node->token, "'%s' is not generic", first->name);
        } else {
            size_t want = array_length(t->type_params);
            size_t got = array_length(first->type_args);
            if (want != got)
                sema_error(s, node->token,
                           "'%s' expects %zu type argument%s, got %zu", first->name,
                           want, want == 1 ? "" : "s", got);
        }
        for (size_t i = 0; i < array_length(first->type_args); i++)
            check_alias_type_names(s, *(ASTNode **)array_get(first->type_args, i),
                                   params, self);
        return;
    }
    if (generic)
        sema_error(s, node->token, "'%s' expects type arguments ('%s<...>')",
                   first->name, first->name);
    /* Deeper segments (`A::B`) are left to resolution. */
}

static void collect_types(Sema *s, ASTNode *program, const char *dir) {
    Array *stmts = program->as.program.stmts;
    for (size_t i = 0; i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        if (!stmt) continue;

        if (stmt->kind == AST_USE) {
            load_use(s, stmt, dir);
            if (s->had_error) return; /* a failed import is fatal */
        } else if (stmt->kind == AST_PACKED_DEF) {
            collect_packed_def(s, stmt);
        } else if (stmt->kind == AST_TYPE_DEF) {
            const char *name = simple_path_name(stmt->as.type_def.target);
            if (!name) continue;
            if (type_name_is_reserved(s, name, stmt->as.type_def.target->token)) continue;
            if (scope_lookup_local(s->global, name)) {
                sema_error(s, stmt->as.type_def.target->token,
                           "cannot redefine '%s'", name);
                continue;
            }
            Array *params = stmt->as.type_def.type_params;
            if (params && array_length(params) > 0) {
                /* A generic declaration: a template with no members until it is
                   instantiated (`plans/done/generics.md` Phase 3). */
                Type *t = type_template(&s->types, name, params);
                if (!t) continue;
                t->inst_body = stmt->as.type_def.body;
                t->src_file = s->diag->file;
                t->src_source = s->diag->source;
                stmt->type = t;
                t->decl_tok = stmt->as.type_def.target->token;
                sym_tok(scope_define(s->arena, s->global, name, t, SYM_TYPE, false),
                        stmt->as.type_def.target->token);
                validate_type_params(s, stmt->token, params);
                continue;
            }
            if (params) {
                sema_error(s, stmt->token, "'type<>' needs at least one type parameter");
                /* fall through, so `Box` stays usable as a plain type */
            }
            Type *t = type_obj(&s->types, name);
            stmt->type = t;
            t->decl_tok = stmt->as.type_def.target->token;
            sym_tok(scope_define(s->arena, s->global, name, t, SYM_TYPE, false),
                    stmt->as.type_def.target->token);
        } else if (stmt->kind == AST_ASSIGN) {
            /* A generic function's parameters are validated here, with the
               types: this pass runs before the later units' globals are
               registered, so "fresh" means the same as for a generic type. */
            ASTNode *gvalue = stmt->as.assign.value;
            if (gvalue && gvalue->kind == AST_FUNC_LITERAL && gvalue->as.func.type_params)
                validate_type_params(s, stmt->token, gvalue->as.func.type_params);
        } else if (stmt->kind == AST_ENUM_DEF) {
            ASTNode *target = stmt->as.enum_def.target;
            if (!target || target->kind != AST_PATH) continue;
            Array *parts = target->as.path.parts;
            size_t n = array_length(parts);
            if (n == 0) continue;
            const char *name = ((PathSegment *)array_get(parts, n - 1))->name;
            if (type_name_is_reserved(s, name, target->token)) continue;
            /* The underlying type is inferred from the values in collect_defs. */
            Type *t = type_enum(&s->types, name, s->types.unknown);
            stmt->type = t;
            t->decl_tok = target->token;
            /* `Type::Name = enum { ... }` defines a static member type. */
            Scope *scope = s->global;
            if (n > 1) {
                Type *owner = resolve_prefix_type(s, parts, n - 1, s->global, target->token);
                scope = (owner && owner->members) ? owner->members : NULL;
            }
            if (!scope) continue;
            if (scope_lookup_local(scope, name)) {
                sema_error(s, target->token, "cannot redefine '%s'", name);
                continue;
            }
            sym_tok(scope_define(s->arena, scope, name, t, SYM_TYPE, n > 1), target->token);
        }
    }

    /* Function-type aliases (`plans/done/function_type_aliases.md`) resolve in a
       second pass, once every nominal type in this unit (and every import) is
       registered, so an alias may name a type declared later in the file. They
       resolve in declaration order, so an earlier alias is visible to a later
       one; the right side must be a function type. A `Name<T>` alias is a
       template instead, instantiated per use (`instantiate_alias`). */
    for (size_t i = 0; i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        if (!stmt || stmt->kind != AST_TYPE_ALIAS) continue;
        const char *name = stmt->as.type_alias.name;
        if (!name) continue;
        if (type_name_is_reserved(s, name, stmt->token)) continue;
        if (scope_lookup_local(s->global, name)) {
            sema_error(s, stmt->token, "cannot redefine '%s'", name);
            continue;
        }
        Array *params = stmt->as.type_alias.type_params;
        if (params && array_length(params) > 0) {
            Type *tmpl = type_template(&s->types, name, params);
            if (!tmpl) continue;
            stmt->type = tmpl;
            Symbol *sym = scope_define(s->arena, s->global, name, tmpl, SYM_TYPE, false);
            if (sym) {
                sym->is_alias = true;
                sym->decl = stmt;
                sym->decl_tok = stmt->token;
            }
            validate_type_params(s, stmt->token, params);
            continue;
        }
        Type *t = resolve_type_node(s, stmt->as.type_alias.type, s->global);
        if (!t || t->kind == TYPE_UNKNOWN) continue;
        if (t->kind != TYPE_FUNC) {
            sema_error(s, stmt->token, "a type alias must name a function type");
            continue;
        }
        stmt->type = t;
        Symbol *sym = scope_define(s->arena, s->global, name, t, SYM_TYPE, false);
        if (sym) {
            sym->is_alias = true;
            sym->decl = stmt;
            sym->decl_tok = stmt->token;
        }
    }

    /* A generic alias's right side has no meaning until its parameters are
       bound, so it is not resolved at declaration — but every type name it
       mentions is checked here, after all types and aliases are registered, so
       a typo (or a bad arity, or naming itself) is caught at the declaration
       instead of at the first use. */
    for (size_t i = 0; i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        if (!stmt || stmt->kind != AST_TYPE_ALIAS) continue;
        Array *params = stmt->as.type_alias.type_params;
        if (!params || array_length(params) == 0) continue;
        Scope *pnames = scope_new(s->arena, s->global);
        for (size_t j = 0; j < array_length(params); j++)
            scope_define(s->arena, pnames, *(char **)array_get(params, j),
                         s->types.unknown, SYM_TYPE, false);
        Symbol *self = stmt->as.type_alias.name
                           ? scope_lookup_local(s->global, stmt->as.type_alias.name)
                           : NULL;
        /* Only the symbol this declaration registered counts as `self`; after a
           redefinition error the name may be someone else's symbol. */
        if (!(self && self->is_alias && self->decl == stmt)) self = NULL;
        check_alias_type_names(s, stmt->as.type_alias.type, pnames, self);
    }
}

/* ------------------------------------------------------------------ */
/* Phase A1: register functions and members                           */
/* ------------------------------------------------------------------ */

/* True when `p` is written as the receiver (`(T t)` or just `(T)`). */
static bool param_names_owner(ASTNode *p, Type *owner) {
    const char *decl = owner_decl_name(owner);
    if (!p || !p->as.param.type || !decl) return false;
    ASTNode *t = p->as.param.type;
    if (t->kind != AST_PATH || array_length(t->as.path.parts) != 1) return false;
    PathSegment *seg = (PathSegment *)array_get(t->as.path.parts, 0);
    return strcmp(seg->name, decl) == 0;
}

/* A nominal type's instance function members are **methods**: each gets the
   receiver as an implicit first parameter, named after the type (the same
   shape as writing `(T t)` / `(T)`), so the body can use siblings by bare name
   and `self` without a receiver parameter. Function values in a `{}` literal
   stay plain values (callbacks), and `::` members stay statics. */
static void add_implicit_receivers(Sema *s, ASTNode *table, Type *owner) {
    const char *decl = owner_decl_name(owner);
    if (!decl) return;
    Array *fields = table->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;
        ASTNode *value = NULL;
        bool is_static = false;
        if (field->kind == AST_ASSIGN) {
            value = field->as.assign.value;
        } else if (field->kind == AST_BIND) {
            value = field->as.bind.value;
        } else if (field->kind == AST_MEMBER_ASSIGN) {
            value = field->as.member_assign.value;
            is_static = field->as.member_assign.is_static;
        }
        if (is_static || !value || value->kind != AST_FUNC_LITERAL) continue;
        Array *params = value->as.func.params;
        if (array_length(params) > 0 &&
            param_names_owner(*(ASTNode **)array_get(params, 0), owner))
            continue;   /* the receiver is written out */
        ASTNode *self = ast_node_new(s->arena, AST_PARAM, field->token);
        self->as.param.type = NULL;
        self->as.param.name = decl;   /* bound as the receiver */
        array_insert(params, 0, &self);
    }
}

/* Define a global symbol. Its byte offset is assigned later, once every type
   is final (see `assign_global_slots`). */
static Symbol *define_global(Sema *s, const char *name, Type *type, SymbolKind kind,
                             Token tok) {
    Symbol *sym = scope_define(s->arena, s->global, name, type, kind, false);
    if (sym) {
        sym->is_global = true;
        sym->decl_tok = tok;
    }
    return sym;
}

/* Mark a member symbol: statics live in global storage. Instance members of an
   object get a byte offset later, in `assign_field_offsets`; the built-in
   primitives (String, Array) address their intrinsic fields by index. */
static void assign_member_slot(Sema *s, Type *owner, Symbol *sym, bool is_static) {
    (void)s;
    /* A method template has no storage: its instantiations do. */
    if (!sym || sym->is_generic) return;
    if (is_static) {
        sym->is_global = true;
    } else if (owner && owner->kind != TYPE_OBJ) {
        sym->slot = (int)owner->field_count++;
    }
}

/* A function member's signature. A member's own type parameters (a generic
   method) are bound as unknown types here: they are placeholders, since the
   real signature is built per method instantiation (`instantiate_func`). */
static Type *member_func_type(Sema *s, ASTNode *value, Scope *scope) {
    Array *own = value->as.func.type_params;
    Scope *fnscope = scope;
    if (own && array_length(own) > 0) {
        fnscope = scope_new(s->arena, scope);
        for (size_t k = 0; k < array_length(own); k++)
            scope_define(s->arena, fnscope, *(char **)array_get(own, k),
                         s->types.unknown, SYM_TYPE, false);
    }
    return func_type_from_params(s, value->as.func.params, value->as.func.ret_type, fnscope);
}

/* True when a receiver can be written for `owner`: a named nominal type (an
   instance counts through its template). A `{}` literal's type has no name. */
static bool owner_has_name(Sema *s, Type *owner) {
    if (!owner) return false;
    Type *decl = owner->origin ? owner->origin : owner;
    if (!decl->name) return false;
    Symbol *sym = scope_lookup_local(s->global, decl->name);
    return sym && sym->kind == SYM_TYPE && sym->type == decl;
}

/* True when a member's value is a generic function: a template, checked per
   call, with no field or global of its own (`instantiate_func`). */
static bool member_is_template(ASTNode *value) {
    return value && value->kind == AST_FUNC_LITERAL && value->as.func.type_params &&
           array_length(value->as.func.type_params) > 0;
}

/* `::==` and `::<` are the comparison members: two parameters of the owner type
   and an `int` result (0/1), like the built-in comparisons. The other four
   comparisons are derived from these two. */
static void check_comparison_member(Sema *s, Type *owner, const char *member,
                                    Type *mt, Token at) {
    if (strcmp(member, "==") != 0 && strcmp(member, "<") != 0) return;
    if (!owner || owner->kind != TYPE_OBJ) {
        sema_error(s, at, "'::%s' can only be defined inside a type body", member);
        return;
    }
    if (!mt || mt->kind != TYPE_FUNC) {
        sema_error(s, at, "'::%s' must be a function", member);
        return;
    }
    if (mt->param_count != 2 || mt->params[0] != owner || mt->params[1] != owner) {
        sema_error(s, at, "'::%s' takes two '%s' parameters", member,
                   owner->name ? owner->name : "?");
        return;
    }
    if (mt->ret != lookup_builtin(s, "int"))
        sema_error(s, at, "'::%s' must return 'int'", member);
}

static void collect_member_assign(Sema *s, ASTNode *node, Type *owner, Scope *scope) {
    const char *member = node->as.member_assign.member;
    bool is_static = node->as.member_assign.is_static;
    ASTNode *value = node->as.member_assign.value;

    Type *mt = s->types.unknown;
    SymbolKind kind = SYM_MEMBER;
    int native_id = -1;
    bool is_func = false;
    if (value && value->kind == AST_FUNC_LITERAL) {
        mt = member_func_type(s, value, scope);
        value->type = mt;
        is_func = true;
        if (member_is_template(value) && !owner_has_name(s, owner))
            sema_error(s, value->token,
                       "a generic function member needs a named type to take a receiver");
    } else if (value && value->kind == AST_EXTERN_DEF) {
        mt = func_type_from_params(s, value->as.extern_def.params, value->as.extern_def.ret_type, scope);
        value->type = mt;
        check_native_signature(s, mt, value->token);
        kind = SYM_FUNC;
        is_func = true;
        native_id = native_find(value->as.extern_def.name);
        if (native_id < 0)
            sema_error(s, value->token, "unknown native '%s'", value->as.extern_def.name);
        value->as.extern_def.native_id = native_id;
    }
    node->type = mt;
    if (is_static) check_comparison_member(s, owner, member, mt, node->token);
    /* A member whose owner's type is not known yet cannot be collected: the
       check phase resolves it once the type is final. (Defining it here would
       pollute the shared unknown type.) */
    if (!owner || owner->kind == TYPE_UNKNOWN) return;

    /* A plain value member is reused on re-assignment, so `obj.m = v` twice
       addresses one field. Function members overload, so each is its own
       symbol. */
    Symbol *sym = is_func ? NULL : scope_lookup_local(owner->members, member);
    if (!sym) {
        sym = scope_define(s->arena, owner->members, member, mt, kind, is_static);
        if (sym) assign_member_slot(s, owner, sym, is_static);
    }
    if (sym) sym->decl_tok = node->token;
    if (sym) {
        sym->native_id = native_id;
        if (is_func) sym->kind = kind;
        if (sym->type == s->types.unknown && mt != s->types.unknown) sym->type = mt;
        if (is_func && member_is_template(value)) {
            /* A generic method: a template checked per call, with nothing of its
               own (its instantiations own the globals). */
            sym->is_generic = true;
            sym->decl = value;
        }
    }
    node->sym = sym;
}

/* The member name of a table field — `name = value`, `Type name = value`, or
   `::name = value` — or NULL when the field is none of those. */
static const char *table_field_name(ASTNode *field) {
    if (!field) return NULL;
    if (field->kind == AST_ASSIGN) return simple_path_name(field->as.assign.target);
    if (field->kind == AST_BIND) return field->as.bind.name;
    if (field->kind == AST_MEMBER_ASSIGN) return field->as.member_assign.member;
    return NULL;
}

/* The value expression of a table field, matching `table_field_name`. */
static ASTNode *table_field_value(ASTNode *field) {
    if (!field) return NULL;
    if (field->kind == AST_ASSIGN) return field->as.assign.value;
    if (field->kind == AST_BIND) return field->as.bind.value;
    if (field->kind == AST_MEMBER_ASSIGN) return field->as.member_assign.value;
    return NULL;
}

static void collect_table_members(Sema *s, ASTNode *table, Type *owner, Scope *scope) {
    Array *fields = table->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;

        if (field->kind == AST_BIND) {
            /* `Type name = value` declares a member with an explicit type. */
            if (field->as.bind.is_local) {
                sema_error(s, field->token, "'local' is not allowed in a table member");
                continue;
            }
            const char *name = field->as.bind.name;
            if (!name || !owner || !owner->members) continue;
            Type *declared = resolve_type_node(s, field->as.bind.type, scope);
            field->type = declared;
            Symbol *sym = scope_define(s->arena, owner->members, name, declared, SYM_MEMBER, false);
            assign_member_slot(s, owner, sym, false);
            field->sym = sym;
            if (sym) {
                sym->init = field->as.bind.value;
                sym->decl_tok = field->token;
            }
        } else if (field->kind == AST_ASSIGN) {
            const char *name = simple_path_name(field->as.assign.target);
            ASTNode *value = field->as.assign.value;
            if (!name) continue;
            if (value && value->kind == AST_TABLE_LITERAL) {
                /* A nested `{}` is an object with its own structural type. */
                Type *t = type_obj(&s->types, name);
                value->type = t;
                field->type = t;
                collect_table_members(s, value, t, scope);
                Symbol *sym = scope_define(s->arena, owner->members, name, t, SYM_MEMBER, false);
                assign_member_slot(s, owner, sym, false);
                field->sym = sym;
                if (sym) sym->decl_tok = field->token;
            } else if (value && value->kind == AST_FUNC_LITERAL) {
                Type *mt = member_func_type(s, value, scope);
                value->type = mt;
                field->type = mt;
                Symbol *sym = scope_define(s->arena, owner->members, name, mt, SYM_MEMBER, false);
                if (sym) sym->decl_tok = field->token;
                if (sym && member_is_template(value)) {
                    if (!owner_has_name(s, owner))
                        sema_error(s, value->token,
                                   "a generic function member needs a named type to "
                                   "take a receiver");
                    sym->is_generic = true;
                    sym->decl = value;
                } else {
                    assign_member_slot(s, owner, sym, false);
                }
                field->sym = sym;
            } else {
                Symbol *sym = scope_define(s->arena, owner->members, name, s->types.unknown, SYM_MEMBER, false);
                assign_member_slot(s, owner, sym, false);
                field->sym = sym;
                if (sym) sym->decl_tok = field->token;
            }
            /* The declared value is also the member's default, applied when a
               `{}` literal omits it. A method template has none. */
            if (field->sym && !field->sym->is_generic) field->sym->init = value;
        } else if (field->kind == AST_MEMBER_ASSIGN) {
            collect_member_assign(s, field, owner, scope);
        } else {
            /* Anything else would be silently dropped from the type, which is
               how `String s` (no initializer) used to disappear. That input
               parses as two bare paths — the type, then the name — so skip the
               type half and report the name once. */
            const char *bare = simple_path_name(field);
            if (bare && lookup_type_name(scope, bare)) continue;
            sema_error(s, field->token,
                       "expected 'name = value' or 'Type name = value' in a table member");
        }
    }
}

static void collect_defs(Sema *s, ASTNode *program) {
    Array *stmts = program->as.program.stmts;

    /* A file-scope declared binding declares a variable. Register it before
       anything else, so a later `name = ...` writes that variable instead of
       becoming a second symbol of the same name (which would look like an
       ambiguous overload). */
    for (size_t i = 0; i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        if (!stmt || stmt->kind != AST_BIND || stmt->as.bind.is_local) continue;
        const char *name = stmt->as.bind.name;
        if (!name || !stmt->as.bind.type) continue;   /* inferred: done while checking */
        if (scope_lookup(s->global, name)) continue;  /* already declared, or a type */
        Type *declared = resolve_type_node(s, stmt->as.bind.type, s->global);
        stmt->sym = define_global(s, name, declared, SYM_VALUE, stmt->token);
        stmt->type = declared;
    }
    for (size_t i = 0; i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        if (!stmt) continue;

        switch (stmt->kind) {
            case AST_TYPE_DEF: {
                const char *name = simple_path_name(stmt->as.type_def.target);
                Symbol *sym = name ? scope_lookup_local(s->global, name) : NULL;
                /* A generic template's body is checked per instantiation. */
                if (sym && sym->type && !sym->type->is_template) {
                    stmt->as.type_def.body->type = sym->type;
                    add_implicit_receivers(s, stmt->as.type_def.body, sym->type);
                    collect_table_members(s, stmt->as.type_def.body, sym->type, s->global);
                }
                break;
            }

            case AST_ENUM_DEF: {
                Type *et = stmt->type;
                if (!et || et->kind != TYPE_ENUM) break;
                Symbol *int_sym = scope_lookup(s->global, "int");
                Type *int_type = (int_sym && int_sym->kind == SYM_TYPE) ? int_sym->type : s->types.unknown;

                long long next = 0;
                Type *underlying = NULL;
                Array *items = stmt->as.enum_def.items;
                for (size_t i = 0; i < array_length(items); i++) {
                    EnumItem *item = (EnumItem *)array_get(items, i);
                    Type *vt;
                    bool is_const = false;
                    long long value = 0;
                    if (item->value) {
                        vt = check_expr(s, item->value, s->global);
                        /* A literal item takes the type the earlier items
                           established (`A = uint(1)`, `B = 2`). */
                        if (underlying) {
                            Type *at = adapt_literal_to(s, item->value, underlying);
                            if (at) vt = at;
                        }
                        long long v;
                        if (int_const_value(item->value, &v)) { next = v + 1; value = v; is_const = true; }
                    } else {
                        item->auto_value = (long)next;
                        value = next;
                        next++;
                        vt = int_type;
                        is_const = true;
                    }
                    if (!underlying) underlying = vt;
                    else if (underlying != vt) {
                        sema_error(s, stmt->token, "enum values must all have the same type");
                    }
                    Symbol *isym = scope_define(s->arena, et->members, item->name, et, SYM_MEMBER, true);
                    if (isym) {
                        isym->is_enum_item = true;
                        if (is_const) {
                            isym->has_const = true;
                            isym->const_value = value;
                        } else {
                            /* A non-constant item (a constructor call, a
                               string, ...) is a static like any other: it
                               has storage and an emitted initializer. */
                            isym->is_global = true;
                            isym->init = item->value;
                        }
                    }
                }
                et->base = underlying ? underlying : int_type;
                et->size = et->base ? et->base->size : 0;
                break;
            }

            case AST_ASSIGN: {
                const char *name = simple_path_name(stmt->as.assign.target);
                ASTNode *value = stmt->as.assign.value;
                if (!name) break;
                Symbol *existing = scope_lookup(s->global, name);
                if (existing && existing->kind == SYM_TYPE) {
                    /* Assigning to a type name is an error; leave the symbol
                       alone and let the check phase report it. */
                    break;
                }
                if (existing && existing->kind == SYM_VALUE) {
                    /* The name is a declared variable (`Type name = value`),
                       so this statement writes it rather than defining a
                       function overload of the same name. */
                    stmt->sym = existing;
                    stmt->type = existing->type;
                    break;
                }
                if (value && value->kind == AST_FUNC_LITERAL && value->as.func.type_params) {
                    /* A generic function: a template with no storage of its own.
                       Each call instantiates it for its argument types. */
                    if (scope_lookup_local(s->global, name))
                        sema_error(s, stmt->token,
                                   "cannot overload a generic function '%s'", name);
                    /* The names were validated with the types
                       (`collect_types`); here they only bind the placeholder
                       signature. */
                    Scope *tparams = scope_new(s->arena, s->global);
                    Array *tnames = value->as.func.type_params;
                    for (size_t i = 0; i < array_length(tnames); i++) {
                        const char *pn = *(char **)array_get(tnames, i);
                        scope_define(s->arena, tparams, pn, s->types.unknown, SYM_TYPE, false);
                    }
                    Type *ft = func_type_from_params(s, value->as.func.params,
                                                     value->as.func.ret_type, tparams);
                    value->type = ft;
                    stmt->type = ft;
                    Symbol *sym = define_global(s, name, ft, SYM_FUNC, stmt->token);
                    if (sym) {
                        sym->is_global = false;   /* a template holds no value */
                        sym->is_generic = true;
                        sym->decl = value;
                    }
                    stmt->sym = sym;
                } else if (value && value->kind == AST_FUNC_LITERAL) {
                    /* A concrete function cannot join a generic one of the same
                       name: overset resolution has no template to compare. */
                    Symbol *existing_fn = scope_lookup_local(s->global, name);
                    if (existing_fn && existing_fn->is_generic)
                        sema_error(s, stmt->token,
                                   "cannot overload a generic function '%s'", name);
                    Type *ft = func_type_from_params(s, value->as.func.params, value->as.func.ret_type, s->global);
                    value->type = ft;
                    stmt->type = ft;
                    stmt->sym = define_global(s, name, ft, SYM_FUNC, stmt->token);
                } else if (value && value->kind == AST_EXTERN_DEF) {
                    Type *ft = func_type_from_params(s, value->as.extern_def.params,
                                                     value->as.extern_def.ret_type, s->global);
                    value->type = ft;
                    check_native_signature(s, ft, value->token);
                    stmt->type = ft;
                    int id = native_find(value->as.extern_def.name);
                    if (id < 0)
                        sema_error(s, value->token, "unknown native '%s'", value->as.extern_def.name);
                    value->as.extern_def.native_id = id;
                    Symbol *sym = define_global(s, name, ft, SYM_FUNC, stmt->token);
                    if (sym) sym->native_id = id;
                    stmt->sym = sym;
                } else if (value && value->kind == AST_TABLE_LITERAL) {
                    /* A `{}` literal is an object with its own structural type. */
                    Type *t = type_obj(&s->types, name);
                    value->type = t;
                    stmt->type = t;
                    collect_table_members(s, value, t, s->global);
                    stmt->sym = define_global(s, name, t, SYM_VALUE, stmt->token);
                } else {
                    stmt->sym = define_global(s, name, s->types.unknown, SYM_VALUE,
                                              stmt->token);
                }
                break;
            }

            case AST_MEMBER_ASSIGN: {
                ASTNode *target = stmt->as.member_assign.target;
                Type *owner = NULL;
                /* Only collect members when the owner's type is already known.
                   A value whose type is inferred in the check phase is handled
                   there (resolving it here would fail on the unknown type). */
                if (target && target->kind == AST_PATH &&
                    array_length(target->as.path.parts) > 0) {
                    PathSegment *first = (PathSegment *)array_get(target->as.path.parts, 0);
                    Symbol *fs = scope_lookup(s->global, first->name);
                    if (fs && fs->type && fs->type->kind != TYPE_UNKNOWN)
                        owner = resolve_type_node(s, target, s->global);
                }
                collect_member_assign(s, stmt, owner, s->global);
                break;
            }

            case AST_EXTERN_DEF: {
                Type *ft = func_type_from_params(s, stmt->as.extern_def.params,
                                                 stmt->as.extern_def.ret_type, s->global);
                stmt->type = ft;
                check_native_signature(s, ft, stmt->token);
                int id = native_find(stmt->as.extern_def.name);
                if (id < 0)
                    sema_error(s, stmt->token, "unknown native '%s'", stmt->as.extern_def.name);
                stmt->as.extern_def.native_id = id;
                Symbol *sym = define_global(s, stmt->as.extern_def.name, ft, SYM_FUNC,
                                            stmt->token);
                if (sym) sym->native_id = id;
                stmt->sym = sym;
                break;
            }

            default:
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Phase B: check bodies                                              */
/* ------------------------------------------------------------------ */

static Type *check_expr(Sema *s, ASTNode *node, Scope *scope);
static Type *check_expr_as(Sema *s, ASTNode *node, Scope *scope, Type *expect);
static void check_stmt(Sema *s, ASTNode *node, Scope *scope);
static void check_block(Sema *s, ASTNode *block, Scope *scope);

static Type *lookup_builtin(Sema *s, const char *name) {
    Symbol *sym = scope_lookup(s->global, name);
    if (sym && sym->kind == SYM_TYPE) return sym->type;
    return s->types.unknown;
}

/* The type named `name` visible from `scope`, if any. A binding that reuses
   such a name would hide the type, so it is rejected. */
static Symbol *lookup_type_name(Scope *scope, const char *name) {
    for (Scope *sc = scope; sc; sc = sc->parent) {
        for (size_t i = array_length(sc->symbols); i > 0; i--) {
            Symbol *sym = *(Symbol **)array_get(sc->symbols, i - 1);
            if (sym && sym->kind == SYM_TYPE && sym->name && strcmp(sym->name, name) == 0)
                return sym;
        }
    }
    return NULL;
}

/* Collect every symbol named `name` defined directly in `scope`. */
static void collect_scope_symbols(Scope *scope, const char *name, Array *out) {
    for (size_t i = 0; i < array_length(scope->symbols); i++) {
        Symbol *sym = *(Symbol **)array_get(scope->symbols, i);
        if (sym && sym->name && strcmp(sym->name, name) == 0) array_push(out, &sym);
    }
}

/* Resolve the type denoted by the first `count` segments of a path. */
/* Resolve the type denoted by the first `count` segments of a path. A segment
   carrying type arguments instantiates (`Box<int>::new`), and a template
   written without them is an error. */
static Type *resolve_prefix_type(Sema *s, Array *parts, size_t count, Scope *scope,
                                 Token at) {
    if (count == 0) return NULL;
    PathSegment *first = (PathSegment *)array_get(parts, 0);
    Symbol *sym = scope_lookup(scope, first->name);
    if (!sym) return NULL;
    Type *cur = segment_type(s, sym, first, scope, at);
    for (size_t i = 1; i < count; i++) {
        PathSegment *seg = (PathSegment *)array_get(parts, i);
        if (!cur || !cur->members) return NULL;
        Symbol *m = scope_lookup_local(cur->members, seg->name);
        if (!m) return NULL;
        cur = segment_type(s, m, seg, scope, at);
    }
    return cur;
}

/* Resolve a path to its final symbol (the last segment). */
static Symbol *resolve_path_symbol(Array *parts, size_t count, Scope *scope) {
    if (count == 0) return NULL;
    PathSegment *first = (PathSegment *)array_get(parts, 0);
    Symbol *sym = scope_lookup(scope, first->name);
    if (!sym) return NULL;
    for (size_t i = 1; i < count; i++) {
        PathSegment *seg = (PathSegment *)array_get(parts, i);
        if (!sym->type || !sym->type->members) return NULL;
        Symbol *m = scope_lookup_local(sym->type->members, seg->name);
        if (!m) return NULL;
        sym = m;
    }
    return sym;
}

/* A user object type (as opposed to the open base `object`). */
static bool is_nominal_obj(Sema *s, Type *t) {
    return t && t->kind == TYPE_OBJ && t != s->types.object;
}

static bool table_literal_matches(Sema *s, ASTNode *node, Type *expect);

/* True if an argument matches a parameter. An array literal can build a packed
   aggregate when the element type and count match; a `{}` literal can build a
   nominal object when every member it writes fits. */
static bool arg_matches(Sema *s, Type *param, ASTNode *arg, Type *argt) {
    if (param == argt) return true;
    /* An integer literal fits a parameter by value (`f(2)` for `f(uint)`). */
    if (arg) {
        bool neg;
        unsigned long long mag;
        if (literal_int_value(arg, &neg, &mag) && literal_fits(param, neg, mag))
            return true;
    }
    if (is_packed_agg(param) && arg && arg->kind == AST_ARRAY_LITERAL) {
        Array *elems = arg->as.array.elements;
        if (array_length(elems) != param->count) return false;
        for (size_t i = 0; i < array_length(elems); i++) {
            ASTNode *e = *(ASTNode **)array_get(elems, i);
            if (!assignable(s, param->base, e->type)) return false;
        }
        return true;
    }
    if (is_nominal_obj(s, param) && arg && arg->kind == AST_TABLE_LITERAL &&
        table_literal_matches(s, arg, param))
        return true;
    if (param && param->kind == TYPE_ARRAY && arg && arg->kind == AST_ARRAY_LITERAL) {
        if (arg->as.array.repeat)
            return (param->size == 0 || param->size == arg->as.array.count) &&
                   arg_matches(s, param->base, arg->as.array.repeat,
                               arg->as.array.repeat->type);
        Array *elems = arg->as.array.elements;
        size_t n = array_length(elems);
        if (param->size != 0 && param->size != n) return false;
        for (size_t i = 0; i < n; i++) {
            ASTNode *e = *(ASTNode **)array_get(elems, i);
            if (!arg_matches(s, param->base, e, e->type)) return false;
        }
        return true;
    }
    return assignable(s, param, argt);
}

/* True if a `{}` literal can be built as `expect`: every member it writes
   exists and each value is assignable, recursing into nested literals. Used
   while resolving overloads, so it reports no diagnostics. */
static bool table_literal_matches(Sema *s, ASTNode *node, Type *expect) {
    if (!expect || expect->kind != TYPE_OBJ) return false;
    if (expect == s->types.object) return true; /* the base object accepts any literal */
    Array *fields = node->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;
        const char *name = table_field_name(field);
        ASTNode *value = table_field_value(field);
        if (field->kind != AST_ASSIGN && field->kind != AST_BIND &&
            field->kind != AST_MEMBER_ASSIGN)
            continue;
        if (!name) return false;
        Symbol *m = expect->members ? scope_lookup_local(expect->members, name) : NULL;
        if (!m) return false;
        if (value && value->kind == AST_TABLE_LITERAL) {
            if (!table_literal_matches(s, value, m->type)) return false;
        } else if (!arg_matches(s, m->type, value, value ? value->type : NULL)) {
            return false;
        }
    }
    return true;
}

/* Pick the overload in `cands` matching the argument types. Exact matches win
   over coercible ones. Returns NULL if none match. */
/* Pick the overload in `cands` matching the argument types. Exact matches win
   over coercible ones. Two candidates that match **exactly** are ambiguous (a
   mistake) and reported. Coercible ties keep the first match, because numeric
   widening routinely makes several overloads fit (e.g. `int(byte)`). `at`,
   `what` and `name` describe the construct for the diagnostic. */
static Symbol *pick_overload(Sema *s, Array *cands, Array *args, Type **arg_types,
                             size_t argc, Token at, const char *what, const char *name) {
    /* Pass 1: exact argument types (or an array literal that builds the param). */
    Symbol *best = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < array_length(cands); i++) {
        Symbol *sym = *(Symbol **)array_get(cands, i);
        Type *ft = sym->type;
        if (!ft || ft->kind != TYPE_FUNC || ft->param_count != argc) continue;
        bool ok = true;
        for (size_t j = 0; j < argc; j++) {
            if (ft->params[j] == arg_types[j]) continue;
            ASTNode *arg = args ? *(ASTNode **)array_get(args, j) : NULL;
            if (is_packed_agg(ft->params[j]) && arg && arg->kind == AST_ARRAY_LITERAL &&
                arg_matches(s, ft->params[j], arg, arg_types[j])) continue;
            ok = false;
            break;
        }
        if (ok) {
            if (matches == 0) best = sym;
            matches++;
        }
    }
    if (matches > 1) sema_error(s, at, "ambiguous %s for '%s'", what, name);
    if (matches >= 1) return best;

    /* Pass 2: coercible argument types (first match wins). */
    for (size_t i = 0; i < array_length(cands); i++) {
        Symbol *sym = *(Symbol **)array_get(cands, i);
        Type *ft = sym->type;
        if (!ft || ft->kind != TYPE_FUNC || ft->param_count != argc) continue;
        bool ok = true;
        for (size_t j = 0; j < argc; j++) {
            ASTNode *arg = args ? *(ASTNode **)array_get(args, j) : NULL;
            if (!arg_matches(s, ft->params[j], arg, arg_types[j])) { ok = false; break; }
        }
        if (ok) return sym;
    }

    return NULL;
}

/* Pick the overload of a called path that matches the argument types.
   `T(args)` is sugar for `T::new(args)` when the callee names a type. */
static Type *resolve_call_overload(Sema *s, ASTNode *callee, Array *args,
                                   Type **arg_types, size_t argc, Scope *scope) {
    Array *parts = callee->as.path.parts;
    size_t n = array_length(parts);
    if (n == 0) return NULL;

    /* A generic function: `f<...>(...)` names the type arguments, otherwise
       they are inferred from the call's arguments. */
    PathSegment *first = (PathSegment *)array_get(parts, 0);
    if (n == 1) {
        Symbol *named = callee->as.path.is_global
                            ? scope_lookup_local(s->global, first->name)
                            : scope_lookup(scope, first->name);
        if (named && named->is_generic) {
            Symbol *isym = resolve_generic_call(s, named, first->type_args, arg_types,
                                                argc, scope, callee->token, NULL);
            if (!isym) return s->types.unknown;
            callee->sym = isym;
            callee->type = isym->type;
            return isym->type;
        }
    }

    /* `Box<int>(...)`: the arguments name the instance to construct. */
    Type *inst = NULL;
    if (n == 1 && first->type_args) {
        Symbol *fsym = callee->as.path.is_global
                           ? scope_lookup_local(s->global, first->name)
                           : scope_lookup(scope, first->name);
        inst = resolve_type_args(s, fsym, first->type_args, scope, callee->token);
        if (!inst || inst->kind == TYPE_UNKNOWN) return s->types.unknown;
    }

    /* `T(args)` is sugar for `T::new(args)`. A type that defines no `::new`
       has no constructor, and a call with no matching overload is an error.
       A packed type is the exception: `T()` with no matching `::new` is its
       intrinsic zero value. */
    Symbol *callee_sym = resolve_path_symbol(parts, n, scope);
    Type *ctor_owner = inst
                           ? inst
                           : ((callee_sym && callee_sym->kind == SYM_TYPE) ? callee_sym->type
                                                                           : NULL);
    if (ctor_owner) {
        if (callee->as.path.is_global) {
            sema_error(s, callee->token,
                       "'%s' is a type, and '$' names a global value",
                       callee_sym ? callee_sym->name : "?");
            return s->types.unknown;
        }
        Type *tt = ctor_owner;
        if (tt->is_template) {
            sema_error(s, callee->token, "'%s' expects type arguments ('%s<...>')",
                       tt->name, tt->name);
            return s->types.unknown;
        }
        const char *tname = tt->name ? tt->name : (callee_sym ? callee_sym->name : "?");
        Array *ctors = array_new(sizeof(Symbol *));
        if (tt->members) collect_scope_symbols(tt->members, "new", ctors);
        Symbol *ctor = array_length(ctors)
                           ? pick_overload(s, ctors, args, arg_types, argc,
                                           callee->token, "constructor", tname)
                           : NULL;
        if (ctor) {
            callee->sym = ctor;
            return ctor->type;
        }
        if (tt->kind == TYPE_PACKED && argc == 0) return tt; /* zero-init */
        if (array_length(ctors) == 0) {
            sema_error(s, callee->token, "type '%s' has no constructor", tname);
        } else {
            sema_error(s, callee->token, "no matching constructor for '%s'", tname);
        }
        return tt; /* report once; keep the type for error recovery */
    }

    Array *cands = array_new(sizeof(Symbol *));
    if (n == 1) {
        const char *name = ((PathSegment *)array_get(parts, 0))->name;
        if (callee->as.path.is_global) {
            /* `$f(...)` names a global function, whatever shadows it locally. */
            collect_scope_symbols(s->global, name, cands);
        } else {
            for (Scope *sc = scope; sc; sc = sc->parent) {
                size_t before = array_length(cands);
                collect_scope_symbols(sc, name, cands);
                if (array_length(cands) > before) break; /* nearest scope shadows */
            }
        }
    } else {
        /* `value::member` reads the value's type: inside a function a global
           value still needs `$` (a type name does not). */
        if (!callee->as.path.is_global && s->func_depth > 0) {
            PathSegment *f0 = (PathSegment *)array_get(parts, 0);
            Symbol *fs = scope_lookup(scope, f0->name);
            if (fs && fs->is_global && fs->kind == SYM_VALUE) {
                sema_error(s, callee->token,
                           "'%s' is a global; inside a function read it as '$%s'",
                           f0->name, f0->name);
                return s->types.unknown;
            }
        }
        Type *owner = resolve_prefix_type(s, parts, n - 1, scope, callee->token);
        if (owner && owner->members) {
            const char *name = ((PathSegment *)array_get(parts, n - 1))->name;
            collect_scope_symbols(owner->members, name, cands);
        }
    }
    /* A generic function member is instantiated, never picked directly. (Its own
       call forms are handled above; the `value::name(...)` path has no fallback
       yet, so excluding it gives a clear error rather than a call to a
       template.) */
    for (size_t i = array_length(cands); i > 0; i--) {
        Symbol *cs = *(Symbol **)array_get(cands, i - 1);
        if (cs && cs->is_generic) array_remove(cands, i - 1, NULL);
    }
    if (array_length(cands) == 0) return NULL;
    const char *cname = ((PathSegment *)array_get(parts, n - 1))->name;
    Symbol *chosen = pick_overload(s, cands, args, arg_types, argc,
                                   callee->token, "overload", cname);
    if (chosen) callee->sym = chosen;
    return chosen ? chosen->type : NULL;
}

/* True for the comparison operators. */
static bool is_comparison(TokenKind k) {
    return k == TOKEN_EQUAL_EQUAL || k == TOKEN_BANG_EQUAL ||
           k == TOKEN_LT || k == TOKEN_GT ||
           k == TOKEN_LESS_EQUAL || k == TOKEN_GREATER_EQUAL;
}

/* The static member that implements `op` for `t`, or NULL when the type has
   none — then the built-in comparison applies. */
static Symbol *comparison_member(Type *t, TokenKind op) {
    if (!t || t->kind != TYPE_OBJ || !t->members) return NULL;
    const char *name = (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) ? "==" : "<";
    Symbol *m = scope_lookup_local(t->members, name);
    if (!m || m->kind != SYM_MEMBER || !m->is_static) return NULL;
    if (!m->type || m->type->kind != TYPE_FUNC) return NULL;
    return m;
}

/* Rewrite `a op b` into the call that implements it: `T::==(a, b)`, `T::<(b, a)`
   for `>`, and `not` around the call for the negated operators. The operands are
   reused as they are (they are already checked), and codegen sees an ordinary
   call to a static member. */
static void rewrite_comparison(Sema *s, ASTNode *node, Symbol *member) {
    TokenKind op = node->as.binary.op.kind;
    ASTNode *left = node->as.binary.left;
    ASTNode *right = node->as.binary.right;
    bool negate = op == TOKEN_BANG_EQUAL || op == TOKEN_LESS_EQUAL ||
                  op == TOKEN_GREATER_EQUAL;
    if (op == TOKEN_GT || op == TOKEN_LESS_EQUAL) {
        ASTNode *swap = left;
        left = right;
        right = swap;
    }

    ASTNode *callee = ast_node_new(s->arena, AST_PATH, node->token);
    callee->as.path.parts = array_new(sizeof(PathSegment));
    PathSegment seg;
    seg.name = (char *)member->name;
    seg.is_static = true;
    seg.type_args = NULL;
    array_push(callee->as.path.parts, &seg);
    callee->as.path.is_global = false;
    callee->as.path.syms = NULL;
    callee->sym = member;
    callee->type = member->type;

    ASTNode *call = ast_node_new(s->arena, AST_CALL, node->token);
    call->as.call.callee = callee;
    call->as.call.args = array_new(sizeof(ASTNode *));
    array_push(call->as.call.args, &left);
    array_push(call->as.call.args, &right);
    call->as.call.builtin = BUILTIN_NONE;
    call->type = lookup_builtin(s, "int");

    if (!negate) {
        node->kind = AST_CALL;
        node->as.call = call->as.call;
        node->type = call->type;
        return;
    }

    /* `a != b` is `not T::==(a, b)`, and the same shape for `<=` and `>=`. */
    Token not_tok;
    not_tok.kind = TOKEN_IDENTIFIER;
    not_tok.start = "not";
    not_tok.length = 3;
    not_tok.line = node->token.line;
    not_tok.message = NULL;
    node->kind = AST_UNARY_EXPR;
    node->as.unary.op = not_tok;
    node->as.unary.operand = call;
    node->type = lookup_builtin(s, "int");
}

/* True if `t` is the contextual keyword `kw`. */
static bool token_is_word(Token t, const char *kw) {
    size_t len = strlen(kw);
    return t.kind == TOKEN_IDENTIFIER && t.length == len && memcmp(t.start, kw, len) == 0;
}

/* The member name of a binary operator (`+` -> "+"). */
static const char *binary_op_name(TokenKind k) {
    switch (k) {
        case TOKEN_PLUS:    return "+";
        case TOKEN_MINUS:   return "-";
        case TOKEN_STAR:    return "*";
        case TOKEN_SLASH:   return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_AMP:     return "&";
        case TOKEN_PIPE:    return "|";
        case TOKEN_CARET:   return "^";
        case TOKEN_SHIFT_LEFT:  return "<<";
        case TOKEN_SHIFT_RIGHT: return ">>";
        default:            return "?";
    }
}

/* A call node `T::name(arg0, arg1)` for an operator overload. */
static ASTNode *make_op_call(Sema *s, Type *owner, const char *name, Symbol *ov,
                             ASTNode *arg0, ASTNode *arg1, Token tok) {
    ASTNode *callee = ast_node_new(s->arena, AST_PATH, tok);
    callee->as.path.parts = array_new(sizeof(PathSegment));
    PathSegment tseg = { (char *)owner->name, false, NULL, tok };
    array_push(callee->as.path.parts, &tseg);
    PathSegment oseg = { (char *)name, true, NULL, tok };
    array_push(callee->as.path.parts, &oseg);
    callee->as.path.syms = array_new(sizeof(Symbol *));
    array_push(callee->as.path.syms, &ov);
    callee->sym = ov;
    callee->type = ov->type;

    ASTNode *call = ast_node_new(s->arena, AST_CALL, tok);
    call->as.call.callee = callee;
    call->as.call.args = array_new(sizeof(ASTNode *));
    array_push(call->as.call.args, &arg0);
    array_push(call->as.call.args, &arg1);
    call->type = (ov->type && ov->type->kind == TYPE_FUNC) ? ov->type->ret
                                                           : s->types.unknown;
    return call;
}

/* Resolve a binary expression in place: a call `T::op(l, r)` when the left
   operand's type defines the operator, else the built-in operator. `err_tok`
   and `err_name` name the operator in the "cannot apply" message (a compound
   assignment reports `+=` even though it resolves the binary `+`). */
static void resolve_binary(Sema *s, ASTNode *node, Type *lt, Type *rt,
                           Token err_tok, const char *err_name) {
    Token op = node->as.binary.op;
    ASTNode *l = node->as.binary.left;
    ASTNode *r = node->as.binary.right;
    const char *opname = binary_op_name(op.kind);

    Symbol *ov = NULL;
    bool has_op = false;
    if (lt && lt->members) {
        Array *cands = array_new(sizeof(Symbol *));
        collect_scope_symbols(lt->members, opname, cands);
        if (array_length(cands) > 0) {
            has_op = true;
            Type *argt[2] = { lt, rt };
            ov = pick_overload(s, cands, NULL, argt, 2, op, "operator", opname);
            if (!ov) {
                char got[64];
                type_str(rt, got, sizeof(got));
                sema_error(s, op, "no matching '%s' operator for '%s' and '%s'",
                           opname, lt->name, got);
            }
        }
    }

    if (ov) {
        ASTNode *call = make_op_call(s, lt, opname, ov, l, r, op);
        node->kind = AST_CALL;
        node->as.call = call->as.call;
        node->type = call->type;
        node->sym = NULL;
        return;
    }
    if (has_op) {
        node->type = s->types.unknown; /* reported above */
        return;
    }

    /* Shifts: the result is the left operand's type — a shift never widens the
       value. The count may be any integer; it is read as a 64-bit value at run
       time, so `x << 63` needs a 64-bit value. */
    if (op.kind == TOKEN_SHIFT_LEFT || op.kind == TOKEN_SHIFT_RIGHT) {
        if (lt && lt->kind != TYPE_UNKNOWN && (!is_numeric(lt) || is_float_type(lt))) {
            char got[64];
            type_str(lt, got, sizeof(got));
            sema_error(s, err_tok, "'%s' needs an integer value, got '%s'",
                       opname, got);
            node->type = s->types.unknown;
            return;
        }
        if (rt && rt->kind != TYPE_UNKNOWN && (!is_numeric(rt) || is_float_type(rt))) {
            char got[64];
            type_str(rt, got, sizeof(got));
            sema_error(s, err_tok, "'%s' needs an integer shift count, got '%s'",
                       opname, got);
            node->type = s->types.unknown;
            return;
        }
        /* A constant count outside the value's width is a compile error; a
           computed one is a runtime error (see the VM). */
        if (lt && lt->kind != TYPE_UNKNOWN) {
            bool neg;
            unsigned long long mag;
            if (literal_int_value(r, &neg, &mag)) {
                unsigned long long width = (unsigned long long)lt->size * 8;
                if (neg || mag >= width) {
                    char tn[64];
                    type_str(lt, tn, sizeof(tn));
                    sema_error(s, err_tok,
                               "shift count %s%llu is out of range for '%s' (0..%llu)",
                               neg ? "-" : "", mag, tn, width - 1);
                }
            }
        }
        node->type = lt ? lt : s->types.unknown;
        return;
    }

    if (lt && lt->kind != TYPE_UNKNOWN && rt && rt->kind != TYPE_UNKNOWN &&
        (!is_numeric(lt) || !is_numeric(rt))) {
        char a[64], b[64];
        type_str(lt, a, sizeof(a));
        type_str(rt, b, sizeof(b));
        sema_error(s, err_tok, "cannot apply '%s' to '%s' and '%s'", err_name, a, b);
        node->type = s->types.unknown;
        return;
    }
    node->type = promote(s, lt, rt, op);
}

/* A `break` that leaves this loop: a nested loop owns its own breaks, and a
   nested function is a separate body. */
static bool stmt_has_break(ASTNode *node) {
    if (!node) return false;
    switch (node->kind) {
        case AST_BREAK:
            return true;
        case AST_BLOCK: {
            Array *stmts = node->as.block.stmts;
            for (size_t i = 0; i < array_length(stmts); i++)
                if (stmt_has_break(*(ASTNode **)array_get(stmts, i))) return true;
            return false;
        }
        case AST_IF:
            return stmt_has_break(node->as.if_stmt.then_branch) ||
                   stmt_has_break(node->as.if_stmt.else_branch);
        case AST_FOR:
            return stmt_has_break(node->as.for_stmt.body);
        case AST_SWITCH: {
            /* A `break` in a case body leaves an enclosing loop: a switch is not
               a loop, so it does not own its breaks. */
            Array *cases = node->as.switch_stmt.cases;
            for (size_t i = 0; i < array_length(cases); i++) {
                ASTNode *c = *(ASTNode **)array_get(cases, i);
                if (stmt_has_break(c->as.case_stmt.body)) return true;
            }
            return stmt_has_break(node->as.switch_stmt.default_body);
        }
        default:
            return false;
    }
}

static bool stmt_terminates(Sema *s, ASTNode *stmt);

/* Defined with the switch checks below; `stmt_terminates` needs it sooner. */
static bool enum_cases_cover(Type *et, Array *cases, Symbol **missing);

/* A condition that is a constant true (`while 1`), so the loop never falls
   through. Understands the literal spellings (`0x1`, `0b1`). */
static bool cond_is_true(ASTNode *cond) {
    bool neg;
    unsigned long long mag;
    if (!literal_int_value(cond, &neg, &mag)) return false;
    return mag != 0;
}

/* True if control cannot fall out of `stmt`: it always returns, or it is a
   `while` over a constant true with no `break` leaving it, or a switch whose
   every case leaves (see the AST_SWITCH case for what counts as total). */
static bool stmt_terminates(Sema *s, ASTNode *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case AST_RETURN:
            return true;
        case AST_BLOCK: {
            Array *stmts = stmt->as.block.stmts;
            for (size_t i = 0; i < array_length(stmts); i++)
                if (stmt_terminates(s, *(ASTNode **)array_get(stmts, i))) return true;
            return false;
        }
        case AST_IF: {
            ASTNode *els = stmt->as.if_stmt.else_branch;
            return els && stmt_terminates(s, stmt->as.if_stmt.then_branch) &&
                   stmt_terminates(s, els);
        }
        case AST_WHILE: {
            return cond_is_true(stmt->as.while_stmt.cond) &&
                   !stmt_has_break(stmt->as.while_stmt.body);
        }
        case AST_FOR: {
            return cond_is_true(stmt->as.for_stmt.cond) &&
                   !stmt_has_break(stmt->as.for_stmt.body);
        }
        case AST_SWITCH: {
            /* Every case body must leave, and no value may fall through: a
               `default`, or every item of an enum value covered. */
            Array *cases = stmt->as.switch_stmt.cases;
            for (size_t i = 0; i < array_length(cases); i++) {
                ASTNode *c = *(ASTNode **)array_get(cases, i);
                if (!stmt_terminates(s, c->as.case_stmt.body)) return false;
            }
            ASTNode *dflt = stmt->as.switch_stmt.default_body;
            if (dflt) return stmt_terminates(s, dflt);
            Type *st = stmt->as.switch_stmt.expr ? stmt->as.switch_stmt.expr->type : NULL;
            return st && st->kind == TYPE_ENUM && enum_cases_cover(st, cases, NULL);
        }
        default:
            return false;
    }
}

static void check_func_body(Sema *s, ASTNode *func, Scope *parent) {
    /* A literal nested in another body is not visited by `collect_defs`, so its
       type is computed here. Without it the return type would default to
       `void` and every `return expr` would be an error. */
    if (!func->type)
        func->type = func_type_from_params(s, func->as.func.params,
                                           func->as.func.ret_type, parent);
    Type *ft = func->type;
    Scope *scope = scope_new(s->arena, parent);

    /* Give this body an id and put its literal on the stack, so that a capture
       is attributed to every function nested inside the declared variable. */
    int my_id = ++s->func_seq;
    func->as.func.func_id = my_id;
    array_push(s->func_stack, &func);
    int saved_func = s->cur_func;
    s->cur_func = my_id;

    Array *params = func->as.func.params;
    for (size_t i = 0; i < array_length(params); i++) {
        ASTNode *p = *(ASTNode **)array_get(params, i);
        Type *pt = (ft && i < ft->param_count) ? ft->params[i] : s->types.unknown;
        p->type = pt;
        if (p->as.param.name) {
            /* A bare name that names a type is a receiver (bound on purpose);
               an explicit-type parameter may not reuse a type name. */
            if (p->as.param.type && lookup_type_name(scope, p->as.param.name))
                sema_error(s, p->token, "cannot reuse the type name '%s' as a parameter",
                           p->as.param.name);
            p->sym = define_value(s, scope, p->as.param.name, pt, p->token);
        }
    }

    /* A method's receiver is its first parameter when that parameter is the
       enclosing type; bare sibling names inside the body mean its members. A
       nested function inherits the receiver unless it declares one itself. */
    Symbol *saved_receiver = s->cur_receiver;
    if (s->cur_owner && array_length(params) > 0) {
        ASTNode *p0 = *(ASTNode **)array_get(params, 0);
        if (p0->sym && p0->type == s->cur_owner) s->cur_receiver = p0->sym;
    }

    Type *saved = s->current_ret;
    s->current_ret = ft ? ft->ret : s->types.void_type;
    int saved_loops = s->loop_depth;
    s->loop_depth = 0;   /* `break` cannot cross a function boundary */
    int saved_default = s->in_default;
    s->in_default = 0;   /* a body is code, not a member's initializer */
    s->func_depth++;
    check_block(s, func->as.func.body, scope);
    s->func_depth--;
    s->loop_depth = saved_loops;
    s->in_default = saved_default;
    s->current_ret = saved;
    s->cur_func = saved_func;
    s->cur_receiver = saved_receiver;
    array_pop(s->func_stack, NULL);

    /* A function returning a value must not be able to reach its end without
       returning one (the caller would read an uninitialized slot). */
    if (ft && ft->ret && ft->ret->kind != TYPE_VOID && ft->ret->kind != TYPE_UNKNOWN &&
        func->as.func.body && func->as.func.body->kind == AST_BLOCK &&
        !stmt_terminates(s, func->as.func.body)) {
        char ret[64];
        type_str(ft->ret, ret, sizeof(ret));
        sema_error(s, func->token,
                   "function returning '%s' may fall off the end without returning a value", ret);
    }
}

/* If `owner` has a member `name` whose type is still unknown, set it to `vt`.
   Refines members declared with a plain value (e.g. `::stdout = 1`), which the
   collect phase could only record as unknown. */
static void refine_member(Type *owner, const char *name, Type *vt) {
    if (!owner || !owner->members || !name || !vt) return;
    Symbol *sym = scope_lookup_local(owner->members, name);
    if (sym && sym->type && sym->type->kind == TYPE_UNKNOWN) sym->type = vt;
}

/* Lay out an array literal as a packed aggregate: check the element count and
   types, then give the literal the packed type. */
static void coerce_packed_literal(Sema *s, ASTNode *node, Type *expect) {
    if (node->as.array.repeat) return;   /* `[expr; n]` is always a heap array */
    Array *elems = node->as.array.elements;
    size_t n = array_length(elems);
    if (n != expect->count) {
        sema_error(s, node->token, "'%s' needs %zu element%s, got %zu",
                   expect->name, expect->count, expect->count == 1 ? "" : "s", n);
        node->type = expect;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        ASTNode *e = *(ASTNode **)array_get(elems, i);
        if (!assignable(s, expect->base, e->type)) {
            char want[64], got[64];
            type_str(expect->base, want, sizeof(want));
            type_str(e->type, got, sizeof(got));
            sema_error(s, e->token, "element %zu expects '%s', got '%s'", i + 1, want, got);
        }
    }
    node->type = expect;
}

/* Re-type an array literal as the expected array type. Each element is checked
   against the expected element type (so nested literals coerce), and the count
   must match when the expected length is known. */
static void coerce_array_literal(Sema *s, ASTNode *node, Type *expect, Scope *scope) {
    node->type = expect;
    Type *et = expect->base;

    if (node->as.array.repeat) {
        ASTNode *e = node->as.array.repeat;
        long long cval = -1;
        if (expect->size != 0 &&
            !(int_const_value(node->as.array.count_expr, &cval) && cval >= 0))
            sema_error(s, node->token,
                       "a fixed-size array needs a constant repeat count");
        else if (expect->size != 0 && expect->size != (size_t)cval)
            sema_error(s, node->token, "expected %zu element%s, got %lld",
                       expect->size, expect->size == 1 ? "" : "s", cval);
        Type *at = (e->kind == AST_ARRAY_LITERAL || e->kind == AST_TABLE_LITERAL)
                       ? check_expr_as(s, e, scope, et) : e->type;
        if (!assignable(s, et, at)) {
            char want[64], got[64];
            type_str(et, want, sizeof(want));
            type_str(at, got, sizeof(got));
            sema_error(s, e->token, "element expects '%s', got '%s'", want, got);
        }
        return;
    }

    Array *elems = node->as.array.elements;
    size_t n = array_length(elems);
    if (expect->size != 0 && expect->size != n)
        sema_error(s, node->token, "expected %zu element%s, got %zu",
                   expect->size, expect->size == 1 ? "" : "s", n);
    for (size_t i = 0; i < n; i++) {
        ASTNode *e = *(ASTNode **)array_get(elems, i);
        Type *at = (e->kind == AST_ARRAY_LITERAL || e->kind == AST_TABLE_LITERAL)
                       ? check_expr_as(s, e, scope, et) : e->type;
        if (!assignable(s, et, at)) {
            char want[64], got[64];
            type_str(et, want, sizeof(want));
            type_str(at, got, sizeof(got));
            sema_error(s, e->token, "element %zu expects '%s', got '%s'", i + 1, want, got);
        }
    }
}

/* Re-type a `{}` literal as the expected nominal object and point every field
   at the target's member, so it is built with the target's layout. A member the
   literal omits is left zeroed by OP_NEWOBJ, which makes `{}` a default value. */
static void coerce_table_literal(Sema *s, ASTNode *node, Type *expect) {
    node->type = expect;
    Array *fields = node->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;
        const char *name = table_field_name(field);
        if (!name) continue;
        Symbol *m = expect->members ? scope_lookup_local(expect->members, name) : NULL;
        if (!m) {
            sema_error(s, field->token, "type '%s' has no member '%s'", expect->name, name);
            continue;
        }
        field->sym = m;
        field->type = m->type;
    }
}

/* Every value a coerced literal writes must be assignable to its member. */
static void check_coerced_members(Sema *s, ASTNode *node) {
    Array *fields = node->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field || !field->sym) continue;
        ASTNode *value = table_field_value(field);
        if (!value || !value->type) continue;
        if (!assignable(s, field->sym->type, value->type)) {
            char want[64], got[64];
            type_str(field->sym->type, want, sizeof(want));
            type_str(value->type, got, sizeof(got));
            sema_error(s, value->token, "cannot assign '%s' to '%s'", got, want);
        }
    }
}

/* Check `node` against an expected type. An array literal whose expected type
   is a packed aggregate is laid out element-wise and takes that type, so
   `vec2i v = [3, 4]` and `return [x, y]` build packed values. A `{}` literal
   whose expected type is an object takes that type too, so a nominal object can
   be constructed (`test t = { x = 1 }`) with no runtime conversion. */
static Type *check_expr_as(Sema *s, ASTNode *node, Scope *scope, Type *expect) {
    bool coerce_obj = node && node->kind == AST_TABLE_LITERAL && is_nominal_obj(s, expect);
    if (coerce_obj) coerce_table_literal(s, node, expect);

    Type *t = check_expr(s, node, scope);
    if (coerce_obj) check_coerced_members(s, node);

    /* An integer literal takes the expected integer type when the value fits
       (`byte b = 65`, `uint u = 2`); a value that does not fit is reported. */
    Type *lt = adapt_literal_to(s, node, expect);
    if (lt) return lt;

    if (!node || node->kind != AST_ARRAY_LITERAL) return t;
    if (is_packed_agg(expect)) {
        coerce_packed_literal(s, node, expect);
        return expect;
    }
    if (expect && expect->kind == TYPE_ARRAY) {
        coerce_array_literal(s, node, expect, scope);
        return expect;
    }
    return t;
}

static void check_table(Sema *s, ASTNode *table, Type *owner, Scope *scope) {
    /* A table's members are visible to each other, so a member function body
       can refer to a sibling member (e.g. `test4` returning `test3`). Expose
       the owner's members on top of the enclosing scope while checking the
       field values, then restore the member scope's parent. */
    Scope *field_scope = scope;
    Scope *saved_parent = NULL;
    Scope *saved_enclosing = s->field_enclosing;
    Type *saved_owner = s->cur_owner;
    s->cur_owner = owner;
    if (owner && owner->members) {
        saved_parent = owner->members->parent;
        owner->members->parent = scope;
        field_scope = owner->members;
        s->field_enclosing = scope;
    }

    Array *fields = table->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (!field) continue;
        if (field->sym && field->sym->is_generic) continue;   /* checked per call */
        if (field->kind == AST_ASSIGN) {
            Type *expect = field->sym ? field->sym->type : NULL;
            s->in_default++;
            Type *vt = check_expr_as(s, field->as.assign.value, field_scope, expect);
            s->in_default--;
            if (!field->type) field->type = vt;
            /* An inferred member's type comes from this value; `null` cannot
               supply one (a coerced literal's member is already typed). */
            if (null_inferred(vt) && expect &&
                (expect->kind == TYPE_UNKNOWN || expect->kind == TYPE_NULL))
                sema_error(s, field->token,
                           "cannot infer a type from 'null'; write 'Type name = null'");
            refine_member(owner, simple_path_name(field->as.assign.target), vt);
        } else if (field->kind == AST_BIND) {
            /* `Type name = value`: the declared type is the authority. */
            Type *declared = field->sym ? field->sym->type : NULL;
            s->in_default++;
            Type *vt = check_expr_as(s, field->as.bind.value, field_scope, declared);
            s->in_default--;
            if (declared && !assignable(s, declared, vt)) {
                char want[64], got[64];
                type_str(declared, want, sizeof(want));
                type_str(vt, got, sizeof(got));
                Token at = field->as.bind.value ? field->as.bind.value->token : field->token;
                sema_error(s, at, "cannot bind '%s' to '%s'", got, want);
            }
            if (!field->type) field->type = vt;
        } else if (field->kind == AST_MEMBER_ASSIGN) {
            Type *vt = check_expr(s, field->as.member_assign.value, field_scope);
            if (!field->type) field->type = vt;
            refine_member(owner, field->as.member_assign.member, vt);
        }
    }

    if (owner && owner->members) owner->members->parent = saved_parent;
    s->field_enclosing = saved_enclosing;
    s->cur_owner = saved_owner;
}

/* ------------------------------------------------------------------ */
/* Generic instantiation                                              */
/* ------------------------------------------------------------------ */

/* The display name of an instance or an instantiated function: `Box<int>`,
   `Pair<String, int[]>`. */
static const char *instance_display_name(Sema *s, const char *base, Array *args) {
    char buf[192];
    buf[0] = '\0';
    strncat(buf, base ? base : "?", sizeof(buf) - 1);
    strncat(buf, "<", sizeof(buf) - strlen(buf) - 1);
    for (size_t i = 0; i < array_length(args); i++) {
        char ab[64];
        ab[0] = '\0';
        type_str(*(Type **)array_get(args, i), ab, sizeof(ab));
        if (i) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, ab, sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, ">", sizeof(buf) - strlen(buf) - 1);
    size_t len = strlen(buf);
    char *out = (char *)arena_alloc(s->arena, len + 1);
    if (!out) return base ? base : "?";
    memcpy(out, buf, len + 1);
    return out;
}

/* Check the body of an already-created instance: clone the template's body,
   substitute the parameters, and check it as an ordinary nominal body. */
static void check_instance_body(Sema *s, Type *tmpl, Type *inst) {
    if (!tmpl || !inst || inst->inst_body) return;

    /* Clone the body, then mark the instance as in progress *before* checking
       it: a body that names its own instance (directly or through another
       instantiation) finds this one instead of recursing. */
    ASTNode *body = ast_clone(s->arena, tmpl->inst_body);
    inst->inst_body = body ? body : tmpl->inst_body;

    /* `T` is the argument, and the template's own name is the instance, so a
       return type can say `-> Box`. */
    Scope *subst = scope_new(s->arena, s->global);
    if (tmpl->name) scope_define(s->arena, subst, tmpl->name, inst, SYM_TYPE, false);
    for (size_t i = 0; i < array_length(inst->type_args); i++) {
        const char *pname = *(char **)array_get(tmpl->type_params, i);
        Type *a = *(Type **)array_get(inst->type_args, i);
        scope_define(s->arena, subst, pname, a, SYM_TYPE, false);
    }

    /* An error inside the body belongs to the template's own source. */
    const char *saved_file = s->diag->file;
    const char *saved_source = s->diag->source;
    if (tmpl->src_file) diag_set_source(s->diag, tmpl->src_file, tmpl->src_source);

    s->inst_depth++;
    add_implicit_receivers(s, body, inst);
    collect_table_members(s, body, inst, subst);
    check_table(s, body, inst, subst);
    s->inst_depth--;

    if (tmpl->src_file) diag_set_source(s->diag, saved_file, saved_source);
}

/* Instances created while definitions were still being registered get their
   body checked here, once every unit's types, globals and functions exist. */
static void check_pending_instances(Sema *s) {
    for (size_t i = 0; i < array_length(s->pending_insts); i++) {
        Type *inst = *(Type **)array_get(s->pending_insts, i);
        if (!inst || !inst->origin) continue;
        check_instance_body(s, inst->origin, inst);
    }
}

/* The instance of `tmpl` for `args`, checking its body on first use. Instances
   are interned, so one argument list is one type — which object identity
   (`assignable`'s `target == value`) needs. */
static Type *instantiate_template(Sema *s, Type *tmpl, Array *args, Token at) {
    Type *existing = type_instance_find(&s->types, tmpl, args);
    if (existing) return existing;

    const char *name = instance_display_name(s, tmpl->name, args);
    if (s->inst_depth >= SEMA_MAX_INST_DEPTH) {
        sema_error(s, at, "'%s' is nested too deeply to instantiate", name);
        return s->types.unknown;
    }

    Type *inst = type_instance(&s->types, tmpl, args, name);
    if (!inst) return s->types.unknown;

    /* While definitions are still being registered the body has to wait: it
       may name a global declared later in the program. */
    if (s->defer_inst) {
        array_push(s->pending_insts, &inst);
        return inst;
    }
    check_instance_body(s, tmpl, inst);
    return inst;
}

/* Resolve a written `<...>` list into types: the arity must match `want`, and
   every entry must name a type. Returns NULL (after reporting) on a problem. */
static Array *resolve_type_arg_list(Sema *s, const char *what, Array *param_names,
                                    size_t want, Array *written, Scope *scope, Token at) {
    size_t got = array_length(written);
    if (want != got) {
        sema_error(s, at, "'%s' expects %zu type argument%s, got %zu", what, want,
                   want == 1 ? "" : "s", got);
        return NULL;
    }
    Array *resolved = array_new(sizeof(Type *));
    for (size_t i = 0; i < got; i++) {
        ASTNode *arg = *(ASTNode **)array_get(written, i);
        /* An argument written as a bare name must name a type: `Box<int>` is
           fine, `Box<n>` for a value is a mistake worth naming. */
        if (arg && arg->kind == AST_PATH && array_length(arg->as.path.parts) == 1) {
            PathSegment *seg0 = (PathSegment *)array_get(arg->as.path.parts, 0);
            if (!seg0->type_args) {
                Symbol *asym = scope_lookup(scope, seg0->name);
                if (!asym || asym->kind != SYM_TYPE) {
                    const char *pname = (param_names && i < array_length(param_names))
                                            ? *(char **)array_get(param_names, i)
                                            : "T";
                    sema_error(s, arg->token, "'%s' is not a type; '%s' expects '%s'",
                               seg0->name, what, pname);
                    return NULL;
                }
            }
        }
        Type *at = resolve_type_node(s, arg, scope);
        if (!at || at->kind == TYPE_UNKNOWN) return NULL;
        array_push(resolved, &at);
    }
    return resolved;
}

/* The function type a generic alias names for these arguments: the aliased
   function type is resolved with the parameters bound (`type_func` interns the
   result, so every use of one argument list is one `Type*`). */
static Type *instantiate_alias(Sema *s, Symbol *sym, Array *args, Token at) {
    ASTNode *alias = sym ? sym->decl : NULL;
    if (!alias || alias->kind != AST_TYPE_ALIAS) return s->types.unknown;
    if (s->inst_depth >= SEMA_MAX_INST_DEPTH) {
        sema_error(s, at, "'%s' is nested too deeply to instantiate", sym->name);
        return s->types.unknown;
    }
    Array *names = sym->type ? sym->type->type_params : NULL;
    Scope *subst = scope_new(s->arena, s->global);
    for (size_t i = 0; i < array_length(args); i++) {
        if (!names || i >= array_length(names)) break;
        scope_define(s->arena, subst, *(char **)array_get(names, i),
                     *(Type **)array_get(args, i), SYM_TYPE, false);
    }
    s->inst_depth++;
    Type *t = resolve_type_node(s, alias->as.type_alias.type, subst);
    s->inst_depth--;
    if (!t || t->kind != TYPE_FUNC) return s->types.unknown;
    return t;
}

/* `Name<...>`: the instance `sym` names for these arguments. */
static Type *resolve_type_args(Sema *s, Symbol *sym, Array *args, Scope *scope, Token at) {
    /* In a method body the receiver names the enclosing type (`Box<U>` written
       inside a `Box<T>` method), and it is a value, not a type symbol. */
    bool is_type = sym && sym->kind == SYM_TYPE;
    bool is_receiver = sym && s->cur_receiver && sym == s->cur_receiver;
    if (!is_type && !is_receiver) {
        sema_error(s, at, "type arguments on '%s', which is not a type",
                   sym ? sym->name : "?");
        return s->types.unknown;
    }
    /* A function-type alias: bind the written arguments to its parameters and
       resolve the aliased function type (`plans/done/function_type_aliases.md`). A
       non-generic alias has no parameters, so arguments here are an arity error. */
    if (sym->is_alias) {
        Array *names = sym->type ? sym->type->type_params : NULL;
        Array *resolved = resolve_type_arg_list(s, sym->name, names,
                                                array_length(names), args, scope, at);
        if (!resolved) return s->types.unknown;
        return instantiate_alias(s, sym, resolved, at);
    }
    Type *tmpl = sym->type;
    /* Inside an instance's own body the template's name denotes the instance,
       so `Box<T[]>` written there has to reach the template behind it. */
    if (tmpl && tmpl->origin) tmpl = tmpl->origin;
    if (!tmpl || !tmpl->is_template) {
        sema_error(s, at, "'%s' is not a generic type", sym->name);
        return s->types.unknown;
    }
    Array *resolved = resolve_type_arg_list(s, sym->name, tmpl->type_params,
                                            array_length(tmpl->type_params), args, scope, at);
    if (!resolved) return s->types.unknown;
    return instantiate_template(s, tmpl, resolved, at);
}

/* The type a path segment denotes: arguments instantiate, and a template
   written without them is an error up front rather than a confusion later. */
static Type *segment_type(Sema *s, Symbol *sym, PathSegment *seg, Scope *scope, Token at) {
    if (seg && seg->type_args) return resolve_type_args(s, sym, seg->type_args, scope, at);
    Type *t = sym ? sym->type : NULL;
    if (t && t->is_template) {
        sema_error(s, at, "'%s' expects type arguments ('%s<...>')", sym->name, sym->name);
        return s->types.unknown;
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Generic functions                                                  */
/* ------------------------------------------------------------------ */

/* The index of a generic function's parameter among its names, or -1. */
static int type_param_index(Array *names, const char *name) {
    for (size_t i = 0; i < array_length(names); i++) {
        const char *pn = *(char **)array_get(names, i);
        if (pn && strcmp(pn, name) == 0) return (int)i;
    }
    return -1;
}

/* Bind a generic function's type parameters by matching one declared parameter
   type against the type of the argument written for it. */
static bool unify_param(Sema *s, Array *names, Type **binds, ASTNode *decl,
                        Type *actual, Scope *scope, Token at) {
    if (!decl || !actual) return true;
    if (decl->kind == AST_PATH) {
        PathSegment *first = (PathSegment *)array_get(decl->as.path.parts, 0);
        int idx = type_param_index(names, first->name);
        if (idx >= 0) {
            if (array_length(decl->as.path.parts) != 1) {
                sema_error(s, at, "type parameter '%s' cannot have members", first->name);
                return false;
            }
            if (binds[idx]) {
                if (binds[idx] != actual) {
                    char a[64], b[64];
                    type_str(binds[idx], a, sizeof(a));
                    type_str(actual, b, sizeof(b));
                    sema_error(s, at, "'%s' cannot be both '%s' and '%s'", first->name, a, b);
                    return false;
                }
                return true;
            }
            binds[idx] = actual;
            return true;
        }
        /* A generic type name: the argument must be one of its instances, whose
           own arguments bind this function's parameters (`List<T>` against
           `List<int>`). */
        if (first->type_args) {
            Symbol *gsym = scope_lookup(scope, first->name);
            Type *gtmpl = (gsym && gsym->kind == SYM_TYPE) ? gsym->type : NULL;
            if (gtmpl && gtmpl->is_template) {
                if (!actual->origin || actual->origin != gtmpl) {
                    char got[64];
                    type_str(actual, got, sizeof(got));
                    sema_error(s, at, "expected '%s<...>', got '%s'", first->name, got);
                    return false;
                }
                Array *decl_args = first->type_args;
                if (array_length(decl_args) != array_length(actual->type_args)) return false;
                for (size_t i = 0; i < array_length(decl_args); i++) {
                    ASTNode *da = *(ASTNode **)array_get(decl_args, i);
                    Type *aa = *(Type **)array_get(actual->type_args, i);
                    if (!unify_param(s, names, binds, da, aa, scope, at)) return false;
                }
                return true;
            }
        }
        /* Any other type: the argument has to fit it. */
        Type *want = resolve_type_node(s, decl, scope);
        if (want && want->kind != TYPE_UNKNOWN && !assignable(s, want, actual)) {
            char a[64], b[64];
            type_str(want, a, sizeof(a));
            type_str(actual, b, sizeof(b));
            sema_error(s, at, "expected '%s', got '%s'", a, b);
            return false;
        }
        return true;
    }
    if (decl->kind == AST_ARRAY_TYPE) {
        if (actual->kind != TYPE_ARRAY) {
            char got[64];
            type_str(actual, got, sizeof(got));
            sema_error(s, at, "expected an array, got '%s'", got);
            return false;
        }
        if (decl->as.array_type.has_length && actual->size != decl->as.array_type.length) {
            sema_error(s, at, "expected %zu elements, got %zu", decl->as.array_type.length,
                       actual->size);
            return false;
        }
        return unify_param(s, names, binds, decl->as.array_type.element, actual->base, scope, at);
    }
    if (decl->kind == AST_FUNC_TYPE) {
        if (actual->kind != TYPE_FUNC) {
            char got[64];
            type_str(actual, got, sizeof(got));
            sema_error(s, at, "expected a function, got '%s'", got);
            return false;
        }
        size_t n = array_length(decl->as.func_type.params);
        if (actual->param_count != n) {
            sema_error(s, at, "expected %zu parameter%s, got %zu", n, n == 1 ? "" : "s",
                       actual->param_count);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            ASTNode *dp = *(ASTNode **)array_get(decl->as.func_type.params, i);
            if (!unify_param(s, names, binds, dp, actual->params[i], scope, at)) return false;
        }
        if (decl->as.func_type.ret)
            return unify_param(s, names, binds, decl->as.func_type.ret, actual->ret, scope, at);
        return true;
    }
    return true;
}

/* The instantiation of the generic function `tmpl` for `args`: a clone of its
   body checked with the parameters bound, living in a global of its own. */
static Symbol *instantiate_func(Sema *s, Symbol *tmpl, Array *args, Type *owner_inst, Token at) {
    ASTNode *decl = tmpl->decl;
    if (!decl) return NULL;
    if (decl->as.func.insts) {
        for (size_t i = 0; i < array_length(decl->as.func.insts); i++) {
            ASTNode *clone = *(ASTNode **)array_get(decl->as.func.insts, i);
            if (!clone || !clone->as.func.type_args) continue;
            if (array_length(clone->as.func.type_args) != array_length(args)) continue;
            bool same = true;
            for (size_t j = 0; j < array_length(args); j++) {
                Type *a = *(Type **)array_get(clone->as.func.type_args, j);
                Type *b = *(Type **)array_get(args, j);
                if (a != b) { same = false; break; }
            }
            if (same) return clone->sym;
        }
    } else {
        decl->as.func.insts = array_new(sizeof(ASTNode *));
    }

    const char *name = instance_display_name(s, tmpl->name, args);
    if (s->inst_depth >= SEMA_MAX_INST_DEPTH) {
        sema_error(s, at, "'%s' is nested too deeply to instantiate", name);
        return NULL;
    }

    ASTNode *clone = ast_clone(s->arena, decl);
    if (!clone) return NULL;
    clone->as.func.type_args = args;

    /* Each instantiation owns a global: codegen stores its function value there
       (`gen_func_insts`). */
    Symbol *sym = scope_define(s->arena, s->global, name, NULL, SYM_FUNC, false);
    if (!sym) return NULL;
    sym->is_global = true;
    sym->decl_tok = decl->token;
    clone->sym = sym;
    array_push(decl->as.func.insts, &clone);

    Scope *subst = scope_new(s->arena, s->global);
    /* A method's body also needs the enclosing type's bindings, exactly as the
       type's own instantiation provided them. */
    if (owner_inst && owner_inst->origin) {
        Type *otmpl = owner_inst->origin;
        if (otmpl->name) scope_define(s->arena, subst, otmpl->name, owner_inst, SYM_TYPE, false);
        for (size_t i = 0; i < array_length(owner_inst->type_args); i++) {
            const char *pn = *(char **)array_get(otmpl->type_params, i);
            Type *a = *(Type **)array_get(owner_inst->type_args, i);
            scope_define(s->arena, subst, pn, a, SYM_TYPE, false);
        }
    }
    Array *names = decl->as.func.type_params;
    for (size_t i = 0; i < array_length(args); i++) {
        const char *pn = *(char **)array_get(names, i);
        Type *a = *(Type **)array_get(args, i);
        scope_define(s->arena, subst, pn, a, SYM_TYPE, false);
    }

    /* The signature comes first: a call inside the body (recursion) needs it. */
    clone->type = func_type_from_params(s, clone->as.func.params,
                                        clone->as.func.ret_type, subst);
    sym->type = clone->type;

    /* A method's body is checked as if it were a member of the receiver's type,
       so bare sibling names mean the receiver's members. */
    Type *saved_owner = s->cur_owner;
    if (owner_inst) s->cur_owner = owner_inst;
    s->inst_depth++;
    check_func_body(s, clone, subst);
    s->inst_depth--;
    s->cur_owner = saved_owner;

    return sym;
}

/* A call of a generic function: the type arguments are written (`f<int>(...)`)
   or inferred from the call's own arguments. Returns the instantiation. */
static Symbol *resolve_generic_call(Sema *s, Symbol *tmpl, Array *written_args,
                                    Type **arg_types, size_t argc,
                                    Scope *scope, Token at, Type *owner_inst) {
    Array *names = tmpl->decl ? tmpl->decl->as.func.type_params : NULL;
    size_t want = array_length(names);
    Array *targs = NULL;
    if (written_args) {
        targs = resolve_type_arg_list(s, tmpl->name, names, want, written_args, scope, at);
        if (!targs) return NULL;
    } else {
        /* A method's first declared parameter is its receiver. Declared types
           resolve where the declaration lives: for a method, the enclosing
           type's bindings (so `T` means the receiver's argument). */
        Scope *decl_scope = s->global;
        if (owner_inst && owner_inst->origin) {
            Type *otmpl = owner_inst->origin;
            decl_scope = scope_new(s->arena, s->global);
            if (otmpl->name)
                scope_define(s->arena, decl_scope, otmpl->name, owner_inst, SYM_TYPE, false);
            for (size_t i = 0; i < array_length(owner_inst->type_args); i++)
                scope_define(s->arena, decl_scope,
                             *(char **)array_get(otmpl->type_params, i),
                             *(Type **)array_get(owner_inst->type_args, i), SYM_TYPE, false);
        }
        size_t skip = owner_inst ? 1 : 0;
        size_t nparams = tmpl->decl ? array_length(tmpl->decl->as.func.params) : 0;
        if (argc + skip != nparams) {
            sema_error(s, at, "'%s' expects %zu argument%s, got %zu", tmpl->name,
                       nparams - skip, nparams - skip == 1 ? "" : "s", argc);
            return NULL;
        }
        Type **binds = (Type **)arena_alloc(s->arena, sizeof(Type *) * (want ? want : 1));
        for (size_t i = 0; i < want; i++) binds[i] = NULL;
        Array *dparams = tmpl->decl->as.func.params;
        for (size_t i = skip; i < nparams; i++) {
            ASTNode *p = *(ASTNode **)array_get(dparams, i);
            if (!unify_param(s, names, binds, p ? p->as.param.type : NULL, arg_types[i - skip],
                             decl_scope, p ? p->token : at))
                return NULL;
        }
        targs = array_new(sizeof(Type *));
        for (size_t i = 0; i < want; i++) {
            if (!binds[i]) {
                sema_error(s, at,
                           "cannot infer type parameter '%s' for '%s'; name it: "
                           "'%s<...>(...)'",
                           *(char **)array_get(names, i), tmpl->name, tmpl->name);
                return NULL;
            }
            array_push(targs, &binds[i]);
        }
    }
    return instantiate_func(s, tmpl, targs, owner_inst, at);
}

/* Type-check an array growth method (`arr.push(x)`, `.pop()`, `.insert(i, x)`,
   `.remove(i)`). These are compiler intrinsics, not real functions. Returns
   true (and fills in the call node) when the syntax applies to an array. */
static bool try_array_builtin(Sema *s, ASTNode *node, Scope *scope) {
    ASTNode *callee = node->as.call.callee;
    const char *name = NULL;
    ASTNode *recv = NULL;
    bool is_static = false;

    if (callee->kind == AST_PATH && array_length(callee->as.path.parts) >= 2) {
        size_t n = array_length(callee->as.path.parts);
        PathSegment *last = (PathSegment *)array_get(callee->as.path.parts, n - 1);
        name = last->name;
        is_static = last->is_static;
        recv = path_prefix(s, callee, n - 1);
    } else if (callee->kind == AST_MEMBER_ACCESS) {
        name = callee->as.member_access.member;
        is_static = callee->as.member_access.is_static;
        recv = callee->as.member_access.object;
    }
    if (!name || is_static) return false;

    int id = 0;
    if (strcmp(name, "push") == 0) id = BUILTIN_ARRAY_PUSH;
    else if (strcmp(name, "pop") == 0) id = BUILTIN_ARRAY_POP;
    else if (strcmp(name, "insert") == 0) id = BUILTIN_ARRAY_INSERT;
    else if (strcmp(name, "remove") == 0) id = BUILTIN_ARRAY_REMOVE;
    else return false;

    /* Only arrays have these methods; anything else is resolved normally. */
    Type *rt = recv->type ? recv->type : check_expr(s, recv, scope);
    if (!rt || rt->kind != TYPE_ARRAY) return false;
    if (rt->size != 0)
        sema_error(s, node->token, "cannot resize a fixed-size array");

    Array *args = node->as.call.args;
    size_t argc = array_length(args);
    Type *et = recv->type->base;
    Type *it = lookup_builtin(s, "int");
    const char *sig = (id == BUILTIN_ARRAY_PUSH)   ? "(value)"
                      : (id == BUILTIN_ARRAY_POP)  ? "()"
                      : (id == BUILTIN_ARRAY_INSERT) ? "(index, value)"
                                                   : "(index)";

    if (argc != (size_t)(id == BUILTIN_ARRAY_INSERT ? 2 : (id == BUILTIN_ARRAY_POP ? 0 : 1))) {
        sema_error(s, node->token, "'%s' expects %s, got %zu argument%s",
                   name, sig, argc, argc == 1 ? "" : "s");
    } else if (id == BUILTIN_ARRAY_PUSH || id == BUILTIN_ARRAY_INSERT) {
        if (id == BUILTIN_ARRAY_INSERT) {
            ASTNode *ix = *(ASTNode **)array_get(args, 0);
            Type *xt = check_expr_as(s, ix, scope, it);
            if (xt && xt->kind != TYPE_UNKNOWN && (!is_numeric(xt) || is_float_type(xt)))
                sema_error(s, ix->token, "array index must be an integer");
        }
        ASTNode *x = *(ASTNode **)array_get(args, id == BUILTIN_ARRAY_INSERT ? 1 : 0);
        Type *vt = check_expr_as(s, x, scope, et);
        if (!assignable(s, et, vt)) {
            char want[64], got[64];
            type_str(et, want, sizeof(want));
            type_str(vt, got, sizeof(got));
            sema_error(s, x->token, "cannot store '%s' in an array of '%s'", got, want);
        }
    } else if (id == BUILTIN_ARRAY_REMOVE) {
        ASTNode *ix = *(ASTNode **)array_get(args, 0);
        Type *xt = check_expr_as(s, ix, scope, it);
        if (xt && xt->kind != TYPE_UNKNOWN && (!is_numeric(xt) || is_float_type(xt)))
            sema_error(s, ix->token, "array index must be an integer");
    }

    node->as.call.callee = recv;
    node->as.call.builtin = id;
    node->type = (id == BUILTIN_ARRAY_PUSH || id == BUILTIN_ARRAY_INSERT)
                     ? s->types.void_type : et;
    return true;
}

/* Coerce a call's array/table literal arguments to the resolved parameters, so
   a literal that builds a packed value or a nominal object takes the
   parameter's type rather than its own. */
static void coerce_call_args(Sema *s, Scope *scope, Array *args, Type *ft) {
    if (!ft || ft->kind != TYPE_FUNC) return;
    size_t argc = array_length(args);
    for (size_t i = 0; i < argc && i < ft->param_count; i++) {
        ASTNode *arg = *(ASTNode **)array_get(args, i);
        if (arg->kind == AST_ARRAY_LITERAL && is_packed_agg(ft->params[i]))
            coerce_packed_literal(s, arg, ft->params[i]);
        else if (arg->kind == AST_ARRAY_LITERAL && ft->params[i]->kind == TYPE_ARRAY)
            check_expr_as(s, arg, scope, ft->params[i]);
        else if (arg->kind == AST_TABLE_LITERAL && is_nominal_obj(s, ft->params[i]))
            check_expr_as(s, arg, scope, ft->params[i]);
        else {
            bool neg;
            unsigned long long mag;
            if (literal_int_value(arg, &neg, &mag))   /* an integer literal */
                check_expr_as(s, arg, scope, ft->params[i]);
        }
    }
}

/* A method call's own explicit type arguments: `xs.map<int>(f)`. */
static Array *method_type_args(ASTNode *callee) {
    if (!callee || callee->kind != AST_PATH) return NULL;
    size_t n = array_length(callee->as.path.parts);
    if (n == 0) return NULL;
    PathSegment *last = (PathSegment *)array_get(callee->as.path.parts, n - 1);
    return last->type_args;
}

/* `obj.method(args)` — sugar for `T::method(obj, args)`, where `method` is a
   function member of `obj`'s type whose first parameter is the receiver. On
   success the receiver becomes argument 0 and the callee's symbol/type are
   set; returns NULL when the call is not a method call, so the caller falls
   back to the ordinary rules. */
static Type *try_method_call(Sema *s, ASTNode *node, Scope *scope, Type **arg_types) {
    ASTNode *callee = node->as.call.callee;
    ASTNode *recv = NULL;
    const char *name = NULL;

    if (callee->kind == AST_PATH && array_length(callee->as.path.parts) >= 2) {
        size_t n = array_length(callee->as.path.parts);
        PathSegment *last = (PathSegment *)array_get(callee->as.path.parts, n - 1);
        if (last->is_static) return NULL;   /* `T::m(...)` is an explicit call */
        name = last->name;
        recv = path_prefix(s, callee, n - 1);
    } else if (callee->kind == AST_MEMBER_ACCESS) {
        if (callee->as.member_access.is_static) return NULL;
        name = callee->as.member_access.member;
        recv = callee->as.member_access.object;
    } else {
        return NULL;
    }

    /* The receiver may already be checked: a chained call (`a.b().c()`) probes
       it through `try_array_builtin` first, and re-checking a call node would
       prepend its receiver argument a second time. */
    Type *rt = recv->type ? recv->type : check_expr(s, recv, scope);
    /* A dot on a type is the other half of the `::` rule: catch it here so the
       message says what to write, instead of "no matching method". */
    if (recv->sym && recv->sym->kind == SYM_TYPE) {
        sema_error(s, callee->token, "use '::' to call a member of type '%s'",
                   recv->sym->name);
        return s->types.unknown;   /* reported: stop the fallback from repeating it */
    }
    if (!rt || rt->kind == TYPE_UNKNOWN || !rt->members) return NULL;

    /* A method is a function member whose first parameter takes the receiver:
       a static (`T::m`, one per type) is called directly, an instance member
       is read from the instance and called with it. A function member whose
       first parameter is not the receiver is a value (a callback), so
       `g.f(x)` calls it with exactly `x`. */
    Array *all = array_new(sizeof(Symbol *));
    Array *cands = array_new(sizeof(Symbol *));
    collect_scope_symbols(rt->members, name, all);
    Symbol *tmpl = NULL;   /* a generic method: the fallback when nothing matches */
    for (size_t i = 0; i < array_length(all); i++) {
        Symbol *sym = *(Symbol **)array_get(all, i);
        if (!sym || !sym->type || sym->type->kind != TYPE_FUNC) continue;
        if (sym->is_generic) {
            if (!tmpl) tmpl = sym;   /* its real signature is per instantiation */
            continue;
        }
        if (sym->is_static) {
            array_push(cands, &sym);
        } else if (sym->type->param_count >= 1 &&
                   assignable(s, sym->type->params[0], rt)) {
            array_push(cands, &sym);
        }
    }
    if (array_length(cands) == 0 && !tmpl) return NULL;

    /* Match with the receiver as the first argument. */
    Array *args = node->as.call.args;
    size_t argc = array_length(args);
    Array *full = array_new(sizeof(ASTNode *));
    array_push(full, &recv);
    for (size_t i = 0; i < argc; i++) {
        ASTNode *a = *(ASTNode **)array_get(args, i);
        array_push(full, &a);
    }
    Type **full_types = (Type **)arena_alloc(s->arena, sizeof(Type *) * (argc + 1));
    full_types[0] = rt;
    for (size_t i = 0; i < argc; i++) full_types[i + 1] = arg_types[i];

    Symbol *m = pick_overload(s, cands, full, full_types, argc + 1,
                              callee->token, "method", name);
    if (!m && tmpl) {
        /* No concrete method matched: a generic method is the fallback. Its own
           type arguments are written (`xs.map<int>(f)`) or inferred from the
           call's arguments. */
        Symbol *isym = resolve_generic_call(s, tmpl, method_type_args(callee), arg_types,
                                            argc, scope, callee->token, rt);
        if (!isym) return s->types.unknown;   /* reported */
        node->as.call.args = full;            /* the receiver becomes argument 0 */
        callee->sym = isym;
        return isym->type;
    }
    if (!m) {
        char rn[64];
        type_str(rt, rn, sizeof(rn));
        sema_error(s, callee->token, "no matching method '%s' for '%s'", name, rn);
        m = *(Symbol **)array_get(cands, 0);
    }

    if (m->is_static) {
        callee->sym = m;   /* a static is called directly */
    } else {
        /* An instance member: resolve the member read, so codegen loads the
           function value from the instance and calls it with the receiver. */
        check_expr(s, callee, scope);
        /* Keep the overload `pick_overload` chose: the read above selects the
           last member of that name, which is a different function whenever the
           name is overloaded. */
        callee->sym = m;
        callee->type = m->type;
        if (callee->kind == AST_PATH && callee->as.path.syms &&
            array_length(callee->as.path.syms) > 0)
            *(Symbol **)array_get(callee->as.path.syms,
                                  array_length(callee->as.path.syms) - 1) = m;
    }
    node->as.call.args = full;   /* the receiver becomes argument 0 */
    return m->type;
}

static Type *check_expr(Sema *s, ASTNode *node, Scope *scope) {
    if (!node) return s->types.void_type;

    switch (node->kind) {
        case AST_LITERAL:
            if (node->token.kind == TOKEN_FLOAT) {
                node->type = s->types.double_type;
            } else if (node->token.kind == TOKEN_NUMBER) {
                /* A bare literal is the smallest of `int` / `int64` / `uint64`
                   that holds the value; anything larger is out of range. */
                bool neg;
                unsigned long long mag;
                if (!literal_int_value(node, &neg, &mag)) {
                    sema_error(s, node->token, "integer literal does not fit 'uint64'");
                    node->type = s->types.unknown;
                } else if (mag <= 2147483647ull) node->type = lookup_builtin(s, "int");
                else if (mag <= 9223372036854775807ull)
                    node->type = lookup_builtin(s, "int64");
                else if (mag <= 18446744073709551615ull)
                    node->type = lookup_builtin(s, "uint64");
                else {
                    sema_error(s, node->token, "integer literal does not fit 'uint64'");
                    node->type = s->types.unknown;
                }
            } else if (node->token.kind == TOKEN_CHAR) node->type = s->types.byte;
            else node->type = s->types.string;
            return node->type;

        case AST_INTERPOLATED_STRING: {
            Array *parts = node->as.interpolated.parts;
            for (size_t i = 0; i < array_length(parts); i++) {
                ASTNode *part = *(ASTNode **)array_get(parts, i);
                Type *pt = check_expr(s, part, scope);
                if (is_void_type(pt))
                    sema_error(s, part->token, "cannot interpolate a value of type 'void'");
                else if (is_packed_agg(pt))
                    sema_error(s, part->token,
                               "cannot interpolate packed value '%s' yet", pt->name);
            }
            node->type = s->types.string;
            return node->type;
        }

        case AST_PATH: {
            Type *t = resolve_path(s, node, scope);
            /* A lone type name is not a value: it is only meaningful in a type,
               as a constructor callee (`T()`), or followed by `::member`. */
            if (node->sym && node->sym->kind == SYM_TYPE &&
                array_length(node->as.path.parts) == 1)
                sema_error(s, node->token, "'%s' is a type, not a value", node->sym->name);
            return t;
        }

        case AST_NULL:
            node->type = s->types.null_type;
            return node->type;

        case AST_MEMBER_ACCESS: {
            /* The receiver may already be checked: resolving a method callee
               re-enters here (`try_method_call`), and re-checking a call node
               would prepend its receiver argument a second time. */
            ASTNode *obj_node = node->as.member_access.object;
            Type *obj = obj_node->type ? obj_node->type : check_expr(s, obj_node, scope);
            Type *result = s->types.unknown;
            if (obj && obj->members) {
                Symbol *m = scope_lookup_local(obj->members, node->as.member_access.member);
                if (m) { result = m->type; node->sym = m; }
                else {
                    char on[64];
                    type_str(obj, on, sizeof(on));
                    sema_error(s, node->token, "type '%s' has no member '%s'",
                               on, node->as.member_access.member);
                }
            }
            node->type = result;
            return result;
        }

        case AST_CALL: {
            if (try_array_builtin(s, node, scope)) return node->type;

            /* A bare sibling call inside a method means `receiver.name(...)`. */
            rewrite_bare_callee(s, node->as.call.callee, scope);

            Array *args = node->as.call.args;
            size_t argc = array_length(args);
            Type **arg_types = (Type **)arena_alloc(s->arena, sizeof(Type *) * (argc ? argc : 1));
            for (size_t i = 0; i < argc; i++) {
                arg_types[i] = check_expr(s, *(ASTNode **)array_get(args, i), scope);
            }

            /* `obj.method(args)`: a static member function whose first
               parameter is the receiver. On a match the receiver is inserted
               as argument 0 (see `try_method_call`). */
            Type *callee = try_method_call(s, node, scope, arg_types);
            if (callee) {
                node->as.call.callee->type = callee;
                coerce_call_args(s, scope, node->as.call.args, callee);
            } else if (node->as.call.callee->kind == AST_PATH) {
                /* A call through a value member (`obj.member(...)`) resolves via
                   `check_expr` below, which fills the path's symbols so codegen
                   can load the member; only plain and `::` paths go through the
                   overload resolver. */
                Array *cp = node->as.call.callee->as.path.parts;
                size_t cn = array_length(cp);
                bool member_value =
                    cn >= 2 && !((PathSegment *)array_get(cp, cn - 1))->is_static;
                if (!member_value) {
                    callee = resolve_call_overload(s, node->as.call.callee, args, arg_types, argc, scope);
                    if (callee) {
                        node->as.call.callee->type = callee;
                        Symbol *cs = node->as.call.callee->sym;
                        if (cs) coerce_call_args(s, scope, args, cs->type);
                    }
                }
            }
            if (!callee) {
                callee = check_expr(s, node->as.call.callee, scope);
                if (callee && callee->kind == TYPE_FUNC) {
                    if (argc != callee->param_count) {
                        sema_error(s, node->token, "expected %zu argument%s, got %zu",
                                   callee->param_count, callee->param_count == 1 ? "" : "s", argc);
                    }
                    for (size_t i = 0; i < argc; i++) {
                        if (i >= callee->param_count) break;
                        ASTNode *arg = *(ASTNode **)array_get(args, i);
                        if (arg->kind == AST_ARRAY_LITERAL && is_packed_agg(callee->params[i])) {
                            coerce_packed_literal(s, arg, callee->params[i]);
                        } else if (arg->kind == AST_ARRAY_LITERAL &&
                                   callee->params[i]->kind == TYPE_ARRAY) {
                            check_expr_as(s, arg, scope, callee->params[i]);
                        } else if (arg->kind == AST_TABLE_LITERAL &&
                                   is_nominal_obj(s, callee->params[i])) {
                            check_expr_as(s, arg, scope, callee->params[i]);
                        } else if (adapt_literal_to(s, arg, callee->params[i])) {
                            /* an integer literal adapted (or was reported) */
                            arg_types[i] = arg->type;
                        } else if (!assignable(s, callee->params[i], arg_types[i])) {
                            char want[64], got[64];
                            type_str(callee->params[i], want, sizeof(want));
                            type_str(arg_types[i], got, sizeof(got));
                            sema_error(s, node->token,
                                       "argument %zu expects '%s', got '%s'", i + 1, want, got);
                        }
                    }
                } else if (callee && callee->kind != TYPE_UNKNOWN) {
                    char got[64];
                    type_str(callee, got, sizeof(got));
                    sema_error(s, node->token, "cannot call a value of type '%s'", got);
                }
            }
            if (callee && callee->kind == TYPE_FUNC) node->type = callee->ret;
            else if (callee && callee->kind == TYPE_PACKED) node->type = callee; /* `T()` zero-init */
            else node->type = s->types.unknown;
            return node->type;
        }

        case AST_INDEX: {
            Type *target = check_expr(s, node->as.index.target, scope);
            Type *idx = check_expr(s, node->as.index.index, scope);
            if (idx && idx->kind != TYPE_UNKNOWN && (!is_numeric(idx) || is_float_type(idx))) {
                sema_error(s, node->as.index.index->token, "array index must be an integer");
            }
            if (is_packed_agg(target)) {
                /* A packed value is inline storage: the index must be a
                   compile-time constant so the byte offset is known. */
                long long ci;
                if (!int_const_value(node->as.index.index, &ci)) {
                    sema_error(s, node->as.index.index->token,
                               "index into packed type '%s' must be a constant", target->name);
                    node->type = s->types.unknown;
                    return node->type;
                }
                if (ci < 0 || (size_t)ci >= target->count) {
                    sema_error(s, node->as.index.index->token,
                               "index %lld is out of range for '%s' (%zu elements)",
                               ci, target->name, target->count);
                    node->type = s->types.unknown;
                    return node->type;
                }
                node->as.index.const_index = (long)ci;
                node->as.index.has_const_index = true;
                node->type = target->base;
                return node->type;
            }
            if (target && target->kind != TYPE_ARRAY && target->kind != TYPE_UNKNOWN &&
                !is_packed_agg(target)) {
                char got[64];
                type_str(target, got, sizeof(got));
                sema_error(s, node->token, "cannot index a value of type '%s'", got);
            }
            node->type = (target && target->kind == TYPE_ARRAY) ? target->base : s->types.unknown;
            return node->type;
        }

        case AST_BINARY_EXPR: {
            Type *l = check_expr(s, node->as.binary.left, scope);
            Type *r = check_expr(s, node->as.binary.right, scope);
            /* A literal operand takes the other side's integer type when the
               two are not already compatible (`u + 1`). */
            adapt_operand_literal(s, node->as.binary.left, &l, r);
            adapt_operand_literal(s, node->as.binary.right, &r, l);
            TokenKind op = node->as.binary.op.kind;
            if (is_comparison(op)) {
                /* `null` compares only with tagged values. */
                Type *other = (l->kind == TYPE_NULL) ? r : (r->kind == TYPE_NULL ? l : NULL);
                /* A nominal type may define its own comparison (`::==` and
                   `::<`); the other four operators follow from those two. */
                Symbol *cmp = (l == r && !other) ? comparison_member(l, op) : NULL;
                if (other && other->kind != TYPE_NULL && other->kind != TYPE_UNKNOWN &&
                    !is_tagged_type(other)) {
                    char a[64], b[64];
                    type_str(l, a, sizeof(a));
                    type_str(r, b, sizeof(b));
                    sema_error(s, node->token, "cannot compare '%s' and '%s'", a, b);
                } else if (cmp) {
                    rewrite_comparison(s, node, cmp);
                    return node->type;
                } else if (is_packed_agg(l) || is_packed_agg(r)) {
                    if (l != r) {
                        char a[64], b[64];
                        type_str(l, a, sizeof(a));
                        type_str(r, b, sizeof(b));
                        sema_error(s, node->token, "cannot compare '%s' and '%s'", a, b);
                    } else if (op != TOKEN_EQUAL_EQUAL && op != TOKEN_BANG_EQUAL) {
                        sema_error(s, node->token, "cannot order packed values ('%s')", l->name);
                    }
                } else if (op != TOKEN_EQUAL_EQUAL && op != TOKEN_BANG_EQUAL &&
                           l == r && l->kind == TYPE_OBJ) {
                    /* Equality is identity without `::==`, but there is no
                       ordering to fall back on. */
                    sema_error(s, node->token, "cannot order '%s'; define '::<' for it",
                               l->name ? l->name : "?");
                } else if (l->kind != TYPE_UNKNOWN && r->kind != TYPE_UNKNOWN &&
                           is_numeric(l) && is_numeric(r) &&
                           !is_float_type(l) && !is_float_type(r) &&
                           !int_nests(l, r) && !int_nests(r, l)) {
                    /* Comparisons follow the same rule as arithmetic: a mixed
                        signed/unsigned test must say which domain it means. */
                    char a[64], b[64];
                    type_str(l, a, sizeof(a));
                    type_str(r, b, sizeof(b));
                    sema_error(s, node->token,
                               "cannot compare '%s' and '%s'; convert one side explicitly",
                               a, b);
                }
                node->type = lookup_builtin(s, "int");
            } else if (token_is_word(node->as.binary.op, "and") ||
                       token_is_word(node->as.binary.op, "or")) {
                /* Logical operators: integer operands, int result. */
                if (l && l->kind != TYPE_UNKNOWN && !is_numeric(l)) {
                    char got[64];
                    type_str(l, got, sizeof(got));
                    sema_error(s, node->token, "'%.*s' needs integer operands, got '%s'",
                               (int)node->as.binary.op.length, node->as.binary.op.start, got);
                }
                if (r && r->kind != TYPE_UNKNOWN && !is_numeric(r)) {
                    char got[64];
                    type_str(r, got, sizeof(got));
                    sema_error(s, node->token, "'%.*s' needs integer operands, got '%s'",
                               (int)node->as.binary.op.length, node->as.binary.op.start, got);
                }
                node->type = lookup_builtin(s, "int");
            } else {
                resolve_binary(s, node, l, r, node->as.binary.op, binary_op_name(op));
            }
            return node->type;
        }

        case AST_UNARY_EXPR: {
            Type *t = check_expr(s, node->as.unary.operand, scope);
            if (node->as.unary.op.kind == TOKEN_MINUS) {
                /* `byte` is storage and widens to `int` here; an unsigned
                   arithmetic domain has to be converted on purpose. */
                if (t && t->kind != TYPE_UNKNOWN && is_numeric(t) &&
                    !is_float_type(t) && type_is_unsigned(t)) {
                    if (t->size == 1) {
                        t = lookup_builtin(s, "int");
                    } else {
                        char got[64];
                        type_str(t, got, sizeof(got));
                        sema_error(s, node->token,
                                   "cannot negate an unsigned value ('%s'); convert it to a signed type",
                                   got);
                        t = s->types.unknown;
                    }
                }
                node->type = t;
            } else if (node->as.unary.op.kind == TOKEN_TILDE) {
                /* `~`: integer operand, the operand's own type (no promotion). */
                if (t && t->kind != TYPE_UNKNOWN && (!is_numeric(t) || is_float_type(t))) {
                    char got[64];
                    type_str(t, got, sizeof(got));
                    sema_error(s, node->token, "'~' needs an integer operand, got '%s'", got);
                    t = s->types.unknown;
                }
                node->type = t;
            } else if (token_is_word(node->as.unary.op, "not")) {
                /* `not`: integer operand, int result. */
                if (t && t->kind != TYPE_UNKNOWN && !is_numeric(t)) {
                    char got[64];
                    type_str(t, got, sizeof(got));
                    sema_error(s, node->token, "'not' needs an integer operand, got '%s'", got);
                }
                node->type = lookup_builtin(s, "int");
            } else {
                /* The parser only makes `+`, `-`, `~` and `not`; never let
                   anything else quietly act like `not`. */
                sema_error(s, node->token, "unknown unary operator");
                node->type = s->types.unknown;
            }
            return node->type;
        }

        case AST_TABLE_LITERAL:
            /* A `{}` literal gets its own structural type, so its members are
               known wherever it appears (a return value, an argument, a local). */
            if (!node->type) {
                node->type = type_obj(&s->types, "obj");
                collect_table_members(s, node, node->type, scope);
            }
            check_table(s, node, node->type, scope);
            return node->type;

        case AST_ARRAY_LITERAL: {
            /* A literal with no expected type is growable (`T[]`); a fixed
               length comes from the expected type (see `coerce_array_literal`). */
            if (node->as.array.repeat) {
                /* `[expr; n]`: n copies of expr, evaluating expr once. A
                   constant count is known here; a runtime one makes a
                   growable `T[]` holding n copies (n < 0 is a runtime
                   error). */
                Type *et = check_expr(s, node->as.array.repeat, scope);
                Type *ct = check_expr(s, node->as.array.count_expr, scope);
                long long n;
                if (int_const_value(node->as.array.count_expr, &n)) {
                    if (n < 0) {
                        sema_error(s, node->token, "a repeat count cannot be negative");
                    } else {
                        node->as.array.count = (size_t)n;
                        node->as.array.count_const = true;
                    }
                } else if (!assignable(s, s->types.int_type, ct)) {
                    sema_error(s, node->token, "a repeat count must be an integer");
                }
                node->type = type_array(&s->types, et, 0);
                return node->type;
            }
            Array *elems = node->as.array.elements;
            size_t n = array_length(elems);
            Type *elem = s->types.unknown;
            for (size_t i = 0; i < n; i++) {
                Type *et = check_expr(s, *(ASTNode **)array_get(elems, i), scope);
                if (i == 0) { elem = et; continue; }
                if (elem == et) continue;
                if (assignable(s, elem, et)) continue;               /* et fits elem */
                if (assignable(s, et, elem)) { elem = et; continue; } /* widen */
                sema_error(s, node->token, "array elements have mixed types");
            }
            node->type = type_array(&s->types, elem, 0);
            return node->type;
        }

        case AST_FUNC_LITERAL:
            check_func_body(s, node, scope);
            return node->type;

        case AST_EXTERN_DEF:
            /* Signature was built in phase A; nothing to check in the body. */
            return node->type ? node->type : s->types.unknown;

        case AST_USE:
            /* A literal element slot parses statements (`parse_primary`), so a
               `use` can land in expression position; it is never loaded. */
            sema_error(s, node->token, "'use' must be at the top level of a file");
            node->type = s->types.unknown;
            return node->type;

        default:
            node->type = s->types.unknown;
            return node->type;
    }
}

/* `target.member = value` (a value target) or `Type::member = value` (a type
   target). Kept separate so a rewritten bare sibling write can reuse it. */
static void check_member_assign(Sema *s, ASTNode *node, Scope *scope) {
    ASTNode *target = node->as.member_assign.target;
    const char *member = node->as.member_assign.member;
    /* A path target names the owner directly; any other target is a value
       expression whose type is the owner. */
    Type *owner = NULL;
    if (target && target->kind == AST_PATH) {
        owner = resolve_type_node(s, target, scope);
    } else if (target) {
        owner = check_expr(s, target, scope);
    }

    /* Resolve the member symbol so codegen knows its slot. The collect phase
       already picked one for top-level statements (important for overloaded
       function members); only resolve when it did not. */
    Symbol *msym = node->sym;
    if (!msym && owner && owner->members)
        msym = scope_lookup_local(owner->members, member);
    if (msym) node->sym = msym;
    if (msym && (msym->has_const || msym->is_enum_item)) {
        sema_error(s, node->token, "cannot assign to '%s', which is an enum item", member);
        return;
    }

    Type *mt = msym ? msym->type : NULL;
    /* The value may be a member function literal: its receiver is a parameter
       of the owner type, and its body can use siblings. */
    Type *saved_owner = s->cur_owner;
    if (owner) s->cur_owner = owner;
    Type *vt = check_expr_as(s, node->as.member_assign.value, scope, mt);
    s->cur_owner = saved_owner;
    node->type = vt;

    if (is_packed_agg(owner)) {
        if (!msym) {
            sema_error(s, node->token, "type '%s' has no member '%s'", owner->name, member);
        } else if (!assignable(s, mt, vt)) {
            char want[64], got[64];
            type_str(mt, want, sizeof(want));
            type_str(vt, got, sizeof(got));
            sema_error(s, node->token, "cannot assign '%s' to '%s'", got, want);
        }
        return;
    }

    if (owner && owner->kind != TYPE_UNKNOWN && !msym) {
        sema_error(s, node->token, "type '%s' has no member '%s'", owner->name, member);
        return;
    }

    if (owner) refine_member(owner, member, vt);
}

/* A case value (or a range bound) must be assignable to the switch value. */
static void check_case_type(Sema *s, Type *st, ASTNode *v, Type *vt) {
    if (st && st->kind != TYPE_UNKNOWN && vt && vt->kind != TYPE_UNKNOWN &&
        !assignable(s, st, vt)) {
        char want[64], got[64];
        type_str(st, want, sizeof(want));
        type_str(vt, got, sizeof(got));
        sema_error(s, v->token, "case has type '%s', but the switch value is '%s'",
                   got, want);
    }
}

/* A range needs an ordered integral value: an integer kind, or an enum of
   one. Unknown is allowed through (another error explains it). */
static bool range_switch_kind(Type *t) {
    if (!t || t->kind == TYPE_UNKNOWN) return true;
    if (t->kind == TYPE_ENUM) t = t->base;
    if (t && t->kind == TYPE_UNKNOWN) return true;
    if (!t || t->kind != TYPE_PACKED || t->count > 0) return false;
    return !t->is_float;
}

/* True when every item of an enum is covered by an unguarded case — an item
   value, or a constant range containing the item's value — so no value can
   fall through. `missing` receives the first uncovered item otherwise. */
static bool enum_cases_cover(Type *et, Array *cases, Symbol **missing) {
    if (missing) *missing = NULL;
    if (!et || !et->members) return false;
    Array *syms = et->members->symbols;
    for (size_t i = 0; i < array_length(syms); i++) {
        Symbol *item = *(Symbol **)array_get(syms, i);
        if (!item || !item->is_enum_item) continue;
        bool covered = false;
        if (item->has_const) {
            for (size_t j = 0; j < array_length(cases) && !covered; j++) {
                ASTNode *c = *(ASTNode **)array_get(cases, j);
                if (c->as.case_stmt.guard) continue;   /* a guard may fail */
                long long lo;
                if (!int_const_value(c->as.case_stmt.value, &lo)) continue;
                if (c->as.case_stmt.range_hi) {
                    long long hi;
                    if (!int_const_value(c->as.case_stmt.range_hi, &hi)) continue;
                    covered = lo <= item->const_value && item->const_value < hi;
                } else {
                    covered = lo == item->const_value;
                }
            }
        }
        if (!covered) {
            if (missing) *missing = item;
            return false;
        }
    }
    return true;
}

static void check_stmt(Sema *s, ASTNode *node, Scope *scope) {
    if (!node) return;

    switch (node->kind) {
        case AST_USE:
            /* Imports are loaded by scanning a program's top-level statements
               (`collect_types`), so a `use` anywhere else — a block, an
               `if`/`while`/`for` body, the `for` header — would be dropped. */
            if (scope != s->global)
                sema_error(s, node->token, "'use' must be at the top level of a file");
            break;

        case AST_PACKED_DEF:
        case AST_ENUM_DEF:
        case AST_EXTERN_DEF:
            break;

        case AST_TYPE_DEF:
            /* A template has no members; its instances are checked on use. */
            if (!node->type || !node->type->is_template)
                check_table(s, node->as.type_def.body, node->type, scope);
            break;

        case AST_BIND: {
            /* An untyped bind (`local name = value`) takes the value's type. */
            Type *declared = node->as.bind.type
                                 ? resolve_type_node(s, node->as.bind.type, scope)
                                 : NULL;
            Type *vt = check_expr_as(s, node->as.bind.value, scope, declared);
            if (is_void_type(vt)) {
                /* `void` is not a value; recover as unknown so the name can be
                   used below without a cascade of errors. */
                Token at = node->as.bind.value ? node->as.bind.value->token : node->token;
                sema_error(s, at, "cannot bind a value of type 'void'");
                declared = s->types.unknown;
            } else if (!declared) {
                declared = vt;
                if (null_inferred(declared))
                    sema_error(s, node->token,
                               "cannot infer a type from 'null'; write 'Type name = null'");
            } else if (!assignable(s, declared, vt)) {
                char want[64], got[64];
                type_str(declared, want, sizeof(want));
                type_str(vt, got, sizeof(got));
                Token at = node->as.bind.value ? node->as.bind.value->token : node->token;
                sema_error(s, at, "cannot bind '%s' to '%s'", got, want);
            }
            node->type = declared;

            if (lookup_type_name(scope, node->as.bind.name))
                sema_error(s, node->token,
                           "cannot reuse the type name '%s' as a variable", node->as.bind.name);

            /* `local` declares in the current block scope; a plain bind at
               file scope is a global, and inside a function a local. */
            Scope *bscope;
            if (node->as.bind.is_local) {
                if (s->func_depth == 0 && s->allow_local == 0)
                    sema_error(s, node->token, "'local' outside a function");
                bscope = scope;
            } else {
                bscope = (s->func_depth == 0) ? s->global : scope;
            }
            Symbol *bsym = node->sym;   /* set by `collect_defs` for a file-scope declaration */
            if (!bsym) bsym = define_value(s, bscope, node->as.bind.name, declared,
                                           node->token);
            if (bsym && !node->as.bind.is_local && s->func_depth == 0) {
                bsym->is_global = true;
                bsym->def_func = -1;
            }
            node->sym = bsym;
            break;
        }

        case AST_ASSIGN: {
            /* A bare sibling write inside a method writes the receiver's
               member: it becomes `receiver.name = value`. */
            if (rewrite_bare_assign(s, node, scope)) {
                check_member_assign(s, node, scope);
                break;
            }

            /* A generic function's body is checked per instantiation. */
            if (node->as.assign.value && node->as.assign.value->kind == AST_FUNC_LITERAL &&
                node->as.assign.value->as.func.type_params) {
                node->type = node->sym ? node->sym->type : s->types.unknown;
                break;
            }

            ASTNode *target = node->as.assign.target;
            ASTNode *value = node->as.assign.value;

            /* `v[i] = value` on a packed value stores one element. */
            if (target && target->kind == AST_INDEX) {
                Type *tt = check_expr(s, target, scope);
                Type *vt = check_expr_as(s, value, scope, tt);
                if (tt && tt->kind != TYPE_UNKNOWN && !assignable(s, tt, vt)) {
                    char want[64], got[64];
                    type_str(tt, want, sizeof(want));
                    type_str(vt, got, sizeof(got));
                    sema_error(s, value ? value->token : node->token,
                               "cannot assign '%s' to '%s'", got, want);
                }
                node->type = tt;
                break;
            }

            const char *name = simple_path_name(target);
            if (!name) {
                /* `null = 5`, `1 = 2`, ... — not an lvalue. */
                sema_error(s, node->token, "cannot assign to this expression");
                node->type = check_expr(s, value, scope);
                break;
            }
            /* `$name` is always the global; a bare name inside a function never
               reaches one (globals are written `$name`). */
            bool global_ref = target->kind == AST_PATH && target->as.path.is_global;
            Symbol *sym = global_ref ? scope_lookup_local(s->global, name)
                                     : (node->sym ? node->sym : scope_lookup(scope, name));
            if (!global_ref && sym && s->func_depth > 0 && sym->is_global) sym = NULL;
            Type *vt = check_expr_as(s, value, scope,
                                     (sym && sym->kind == SYM_VALUE) ? sym->type : NULL);
            if (is_void_type(vt)) {
                /* `void` is not a value: there is nothing to store. Recover as
                   unknown so the target stays usable. */
                sema_error(s, value ? value->token : node->token,
                           "cannot assign a value of type 'void'");
                vt = s->types.unknown;
            }
            if (sym && sym->kind == SYM_TYPE) {
                sema_error(s, node->token, "cannot assign to type '%s'", name);
                node->type = vt;
                break;
            }
            if (sym) {
                if (sym->type && sym->type->kind == TYPE_UNKNOWN) {
                    /* The symbol had no type yet, so it is being inferred from
                       this value — which `null` alone cannot supply. */
                    sym->type = vt;
                    if (null_inferred(vt))
                        sema_error(s, node->token,
                                   "cannot infer a type from 'null'; write 'Type name = null'");
                }
                if (sym->kind == SYM_VALUE && !assignable(s, sym->type, vt)) {
                    char want[64], got[64];
                    type_str(sym->type, want, sizeof(want));
                    type_str(vt, got, sizeof(got));
                    Token at = value ? value->token : node->token;
                    sema_error(s, at, "cannot assign '%s' to '%s'", got, want);
                }
                node->as.assign.target->type = sym->type;
                node->sym = sym;
            } else if (name) {
                bool at_global = global_ref || s->func_depth == 0;
                /* A new name inside a function is a local; `local` makes that
                   explicit, so nudge toward it rather than declaring
                   silently. */
                if (!at_global)
                    sema_warning(s, target->token,
                                 "'%s' is undeclared; the assignment declares a new local "
                                 "(write 'local %s = ...')", name, name);
                Scope *def_scope = at_global ? s->global : scope;
                Symbol *ns = define_value(s, def_scope, name, vt, target->token);
                if (ns && at_global) {
                    ns->is_global = true;
                    ns->def_func = -1;
                }
                node->sym = ns;
                node->as.assign.target->type = vt;
                if (null_inferred(vt))
                    sema_error(s, node->token,
                               "cannot infer a type from 'null'; write 'Type name = null'");
            }
            if (name && is_captured(s, node->sym))
                sema_error(s, node->token,
                           "cannot assign to captured '%s' (closures capture by value)", name);
            node->type = vt;
            break;
        }

        case AST_COMPOUND_ASSIGN: {
            ASTNode *target = node->as.compound.target;
            ASTNode *value = node->as.compound.value;
            Token op = node->as.compound.op;
            const char *opname = compound_op_name(op.kind);

            Type *tt = check_expr(s, target, scope);
            Type *vt = check_expr(s, value, scope);
            /* `u += 1`: a literal value takes the target's integer type. */
            adapt_operand_literal(s, value, &vt, tt);

            const char *cname = simple_path_name(target);
            if (cname && is_captured(s, target->sym))
                sema_error(s, target->token,
                           "cannot assign to captured '%s' (closures capture by value)", cname);
            if (target->sym && (target->sym->has_const || target->sym->is_enum_item))
                sema_error(s, target->token, "cannot assign to '%s', which is an enum item",
                           target->sym->name);

            /* An operator overload `T::op=` on the target's type wins. */
            Symbol *ov = NULL;
            bool has_op = false;
            if (tt && tt->members) {
                Array *cands = array_new(sizeof(Symbol *));
                collect_scope_symbols(tt->members, opname, cands);
                if (array_length(cands) > 0) {
                    has_op = true;
                    Type *argt[2] = { tt, vt };
                    ov = pick_overload(s, cands, NULL, argt, 2, op, "operator", opname);
                    if (!ov) {
                        char got[64];
                        type_str(vt, got, sizeof(got));
                        sema_error(s, op, "no matching '%s' operator for '%s' and '%s'",
                                   opname, tt->name, got);
                    }
                }
            }

            /* Build the value: `T::op=(target, value)` or `target op value`
               (which may itself resolve to a binary operator overload). */
            ASTNode *opexpr = NULL;
            if (ov) {
                opexpr = make_op_call(s, tt, opname, ov, target, value, op);
            } else if (has_op) {
                /* The type defines the operator but nothing matched; reported. */
            } else {
                Token base = op;
                base.kind = compound_base_op(op.kind);
                base.length = 1;   /* the AST shows the base operator */
                opexpr = ast_node_new(s->arena, AST_BINARY_EXPR, base);
                opexpr->as.binary.left = target;
                opexpr->as.binary.op = base;
                opexpr->as.binary.right = value;
                resolve_binary(s, opexpr, tt, vt, op, opname);
            }

            /* Rewrite in place into the matching assignment form. */
            if (target->kind == AST_PATH && array_length(target->as.path.parts) > 1) {
                size_t n = array_length(target->as.path.parts);
                PathSegment *last = (PathSegment *)array_get(target->as.path.parts, n - 1);
                ASTNode *prefix = path_prefix(s, target, n - 1);
                Symbol *msym = target->sym;
                node->kind = AST_MEMBER_ASSIGN;
                node->as.member_assign.target = prefix;
                node->as.member_assign.member = last->name;
                node->as.member_assign.value = opexpr;
                node->as.member_assign.is_static = last->is_static;
                node->sym = msym;
            } else if (target->kind == AST_MEMBER_ACCESS) {
                node->kind = AST_MEMBER_ASSIGN;
                node->as.member_assign.target = target->as.member_access.object;
                node->as.member_assign.member = target->as.member_access.member;
                node->as.member_assign.value = opexpr;
                node->as.member_assign.is_static = target->as.member_access.is_static;
                node->sym = target->sym;
            } else {
                node->kind = AST_ASSIGN;
                node->as.assign.target = target;
                node->as.assign.value = opexpr;
                node->sym = (target->kind == AST_PATH) ? target->sym : NULL;
            }
            node->type = tt;
            break;
        }

        case AST_MEMBER_ASSIGN: {
            check_member_assign(s, node, scope);
            break;
        }

        case AST_RETURN: {
            Type *vt = node->as.ret.value
                           ? check_expr_as(s, node->as.ret.value, scope, s->current_ret)
                           : s->types.void_type;
            if (s->current_ret && !assignable(s, s->current_ret, vt)) {
                char want[64], got[64];
                type_str(s->current_ret, want, sizeof(want));
                type_str(vt, got, sizeof(got));
                sema_error(s, node->token, "cannot return '%s' from a function returning '%s'", got, want);
            }
            node->type = vt;
            break;
        }

        case AST_IF: {
            Type *ct = check_expr(s, node->as.if_stmt.cond, scope);
            if (is_void_type(ct))
                sema_error(s, node->as.if_stmt.cond->token,
                           "cannot use a value of type 'void' as a condition");
            else if (is_packed_agg(ct))
                sema_error(s, node->as.if_stmt.cond->token,
                           "cannot test packed value '%s'", ct->name);
            check_stmt(s, node->as.if_stmt.then_branch, scope);
            check_stmt(s, node->as.if_stmt.else_branch, scope);
            break;
        }

        case AST_WHILE: {
            Type *ct = check_expr(s, node->as.while_stmt.cond, scope);
            if (is_void_type(ct))
                sema_error(s, node->as.while_stmt.cond->token,
                           "cannot use a value of type 'void' as a condition");
            else if (is_packed_agg(ct))
                sema_error(s, node->as.while_stmt.cond->token,
                           "cannot test packed value '%s'", ct->name);
            s->loop_depth++;
            check_stmt(s, node->as.while_stmt.body, scope);
            s->loop_depth--;
            break;
        }

        case AST_FOR: {
            /* The header introduces a scope, so the counter declared with
               `local` lives only for the loop (and `local` is allowed here even
               at file scope, where it is otherwise rejected). */
            Scope *loop = scope_new(s->arena, scope);
            s->allow_local++;
            check_stmt(s, node->as.for_stmt.init, loop);
            s->allow_local--;
            Type *ct = check_expr(s, node->as.for_stmt.cond, loop);
            if (is_void_type(ct))
                sema_error(s, node->as.for_stmt.cond->token,
                           "cannot use a value of type 'void' as a condition");
            else if (is_packed_agg(ct))
                sema_error(s, node->as.for_stmt.cond->token,
                           "cannot test packed value '%s'", ct->name);
            s->loop_depth++;
            check_stmt(s, node->as.for_stmt.step, loop);
            check_stmt(s, node->as.for_stmt.body, loop);
            s->loop_depth--;
            break;
        }

        case AST_SWITCH: {
            Type *st = node->as.switch_stmt.expr
                           ? check_expr(s, node->as.switch_stmt.expr, scope)
                           : s->types.unknown;
            if (is_void_type(st))
                sema_error(s, node->as.switch_stmt.expr->token,
                           "cannot use a value of type 'void' as a switch value");
            Array *cases = node->as.switch_stmt.cases;
            for (size_t i = 0; i < array_length(cases); i++) {
                ASTNode *c = *(ASTNode **)array_get(cases, i);
                ASTNode *v = c->as.case_stmt.value;
                check_case_type(s, st, v, check_expr(s, v, scope));
                ASTNode *hi = c->as.case_stmt.range_hi;
                /* A type with `::==` matches through it, so `switch` agrees
                   with `if`. Codegen reads the member off the case. */
                if (!hi) c->sym = comparison_member(st, TOKEN_EQUAL_EQUAL);
                if (hi) {
                    check_case_type(s, st, hi, check_expr(s, hi, scope));
                    if (st && !range_switch_kind(st)) {
                        sema_error(s, v->token,
                                   "a range case needs an integer or enum switch value, not '%s'",
                                   st->name ? st->name : "?");
                    } else {
                        long long lo;
                        if (!int_const_value(v, &lo))
                            sema_error(s, v->token,
                                       "a range bound must be a constant integer");
                        else {
                            long long hi_v;
                            if (!int_const_value(hi, &hi_v))
                                sema_error(s, hi->token,
                                           "a range bound must be a constant integer");
                        }
                    }
                }
                ASTNode *g = c->as.case_stmt.guard;
                if (g) {
                    Type *gt = check_expr(s, g, scope);
                    if (is_void_type(gt))
                        sema_error(s, g->token,
                                   "cannot use a value of type 'void' as a condition");
                    else if (is_packed_agg(gt))
                        sema_error(s, g->token, "cannot test packed value '%s'", gt->name);
                }
                check_stmt(s, c->as.case_stmt.body, scope);
            }
            check_stmt(s, node->as.switch_stmt.default_body, scope);
            /* An enum switch that misses items and has no `default` does
               nothing for them: warn. */
            if (st && st->kind == TYPE_ENUM && !node->as.switch_stmt.default_body) {
                Symbol *missing = NULL;
                if (!enum_cases_cover(st, cases, &missing) && missing)
                    sema_warning(s, node->token,
                                 "switch on '%s' has no case for '%s' and no default",
                                 st->name, missing->name);
            }
            break;
        }

        case AST_BREAK:
            if (s->loop_depth == 0)
                sema_error(s, node->token, "'break' outside a loop");
            break;

        case AST_CONTINUE:
            if (s->loop_depth == 0)
                sema_error(s, node->token, "'continue' outside a loop");
            break;

        case AST_BLOCK:
            check_block(s, node, scope);
            break;

        /* A function-type alias is fully handled in `collect_types`
           (`plans/done/function_type_aliases.md`); nothing to check here. */
        case AST_TYPE_ALIAS:
            break;

        default:
            check_expr(s, node, scope);
            break;
    }
}

static void check_block(Sema *s, ASTNode *block, Scope *scope) {
    if (!block) return;
    Scope *inner = scope_new(s->arena, scope);
    Array *stmts = block->as.block.stmts;
    for (size_t i = 0; i < array_length(stmts); i++) {
        check_stmt(s, *(ASTNode **)array_get(stmts, i), inner);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

void sema_init(Sema *s, Arena *arena, Diagnostics *diag) {
    /* Callers pass a stack struct, so start from a defined state: every field is
       read somewhere (`cur_receiver`/`cur_owner` are saved and restored around
       each method body), and a missed assignment must not turn into
       stack-dependent behaviour. */
    memset(s, 0, sizeof *s);
    s->arena = arena;
    type_context_init(&s->types, arena);
    s->global = scope_new(arena, NULL);
    s->current = s->global;
    s->current_ret = NULL;
    s->had_error = false;
    s->diag = diag;
    s->programs = array_new(sizeof(ProgramUnit *));
    s->loaded = array_new(sizeof(char *));
    s->main_dir = ".";
    s->global_size = 0;
    s->func_depth = 0;
    s->loop_depth = 0;
    s->allow_local = 0;
    s->in_default = 0;
    s->cur_func = 0;
    s->func_seq = 0;
    s->func_stack = array_new(sizeof(ASTNode *));
    s->field_enclosing = NULL;
    s->inst_depth = 0;
    s->defer_inst = false;
    s->pending_insts = array_new(sizeof(Type *));
    /* `extern` names are resolved against the native registry, so make sure
       the built-ins are present before any declaration is checked. */
    native_register_std();
}

/* Assign byte offsets to the static members of `t`, recursing into nested
   type members. Statics that already have a slot are left alone, so a type
   reachable from several globals is only laid out once. Constant enum items
   are compile-time values and take no storage; a non-constant item
   (`A = uint(1)`) is a static and does. */
static void assign_type_statics(Sema *s, Type *t) {
    if (!t || t->is_template || !t->members) return;
    Array *ms = t->members->symbols;
    for (size_t j = 0; j < array_length(ms); j++) {
        Symbol *m = *(Symbol **)array_get(ms, j);
        if (!m) continue;
        if (m->kind == SYM_TYPE) {
            assign_type_statics(s, m->type);
        } else if (m->is_static && !m->has_const && m->slot == -1) {
            m->slot = (int)s->global_size;
            s->global_size += type_size(m->type);
        }
    }
}

/* Assign byte offsets to the instance members of every object type, and record
   each type's total field size. Runs after the check phase so every member's
   type is final. */
static void assign_field_offsets(Sema *s) {
    for (size_t i = 0; i < array_length(s->types.all); i++) {
        Type *t = *(Type **)array_get(s->types.all, i);
        if (!t || t->kind != TYPE_OBJ || t->laid_out || t->is_template) continue;
        size_t off = 0;
        Array *ms = t->members->symbols;
        for (size_t j = 0; j < array_length(ms); j++) {
            Symbol *m = *(Symbol **)array_get(ms, j);
            if (!m || m->is_static || m->kind == SYM_TYPE || m->is_generic) continue;
            m->slot = (int)off;
            off += type_size(m->type);
        }
        t->size = off;
        t->laid_out = true;
    }
}

/* Assign byte offsets to globals and static members. Runs after the check
   phase so every type is final and `type_size` is accurate. */
static void assign_global_slots(Sema *s) {
    s->global_size = 0;
    Array *syms = s->global->symbols;
    for (size_t i = 0; i < array_length(syms); i++) {
        Symbol *sym = *(Symbol **)array_get(syms, i);
        if (!sym) continue;
        if (sym->kind == SYM_TYPE) {
            assign_type_statics(s, sym->type);
        } else if (sym->is_global) {
            sym->slot = (int)s->global_size;
            s->global_size += type_size(sym->type);
            /* A value whose type carries static members (`box = {}` with
               `box::helper`) owns their storage too. */
            assign_type_statics(s, sym->type);
        }
    }
    /* Statics of types that no global names (e.g. a type declared inside a
       function) still need storage. */
    for (size_t i = 0; i < array_length(s->types.all); i++) {
        Type *t = *(Type **)array_get(s->types.all, i);
        if (t) assign_type_statics(s, t);
    }
}

/* The embedded prelude key `path` names, or NULL when it is not one of the
   prelude's own files. Matched by path suffix, so the absolute spelling the
   language server arrives with (`/mnt/c/…/src/prelude/core.oby`) works. */
static const char *prelude_key_for(const char *path) {
    size_t pl = strlen(path);
    const EmbeddedFile *files = NULL;
    size_t count = embed_static_files(&files);
    for (size_t i = 0; i < count; i++) {
        const char *key = files[i].path;
        size_t kl = strlen(key);
        if (kl == 0 || pl < kl) continue;
        if (strcmp(path + pl - kl, key) != 0) continue;
        if (pl > kl && path[pl - kl - 1] != '/') continue;   /* a path tail, not a name tail */
        return key;
    }
    return NULL;
}

/* Register the entry file in the pragma-once set. A `use` that leads back to
   it is then a no-op, and when the entry *is* one of the embedded prelude files
   — the language server checks `src/prelude/File.oby` while it is open in the
   editor — its key is registered too, so the prelude's own import of that file
   does not load a second copy whose definitions would clash with the entry's. */
static void seed_entry_loaded(Sema *s, const char *entry) {
    size_t el = strlen(entry);
    char *stored = (char *)arena_alloc(s->arena, el + 1);
    memcpy(stored, entry, el + 1);
    array_push(s->loaded, &stored);

    const char *key = prelude_key_for(entry);
    if (key) {
        size_t kl = strlen(key);
        char *kstored = (char *)arena_alloc(s->arena, kl + 1);
        memcpy(kstored, key, kl + 1);
        array_push(s->loaded, &kstored);
    }
}

void sema_check(Sema *s, ASTNode *program, const char *path) {
    /* One canonical spelling for the entry, so a `use` that leads back to it
       is recognized as already loaded. */
    char *entry = path_normalize(s->arena, path);
    s->main_dir = path_dir(entry, s->arena);
    const char *main_file = s->diag->file;
    const char *main_src = s->diag->source;
    /* Nothing is checked while definitions are still being registered: an
       instantiated body may name a global declared in a later statement or
       unit. Instances queue and are checked once all units are collected. */
    s->defer_inst = true;

    scope_define(s->arena, s->global, "byte", s->types.byte, SYM_TYPE, false);
    scope_define(s->arena, s->global, "object", s->types.object, SYM_TYPE, false);
    scope_define(s->arena, s->global, "void", s->types.void_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "String", s->types.string, SYM_TYPE, false);
    scope_define(s->arena, s->global, "float", s->types.float_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "double", s->types.double_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "int", s->types.int_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "uint", s->types.uint_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "int64", s->types.int64_type, SYM_TYPE, false);
    scope_define(s->arena, s->global, "uint64", s->types.uint64_type, SYM_TYPE, false);

    /* The entry is registered before the prelude loads: an import that leads
       back to it — or, when it is a prelude file being edited, to its embedded
       copy — becomes a pragma-once no-op rather than a second compile. */
    seed_entry_loaded(s, entry);

    /* The prelude is injected before the user script. */
    load_prelude(s);
    if (s->had_error) return;

    /* String's intrinsic fields: the C String struct's data buffer and length.
       Field indices 0 and 1 are what the VM's GETFIELD special-cases. */
    Symbol *buf = scope_define(s->arena, s->types.string->members, "buffer",
                               type_array(&s->types, s->types.byte, 0), SYM_MEMBER, false);
    assign_member_slot(s, s->types.string, buf, false);
    Symbol *len = scope_define(s->arena, s->types.string->members, "length",
                               lookup_builtin(s, "int"), SYM_MEMBER, false);
    assign_member_slot(s, s->types.string, len, false);

    /* The entry file's name/source are already set on the diagnostics. */
    ProgramUnit *main_unit = (ProgramUnit *)arena_alloc(s->arena, sizeof(ProgramUnit));
    main_unit->program = program;
    main_unit->file = main_file;
    main_unit->source = main_src;

    /* A prelude file being edited imports its siblings from the prelude's own
       directory — the embedded copy — when the path it arrived with has no
       directory on this side, which is what happens when a client elsewhere
       (Zed on Windows, say) labels the file with its own path spelling. */
    const char *import_dir = s->main_dir;
    const char *prelude_key = prelude_key_for(entry);
    if (prelude_key && !source_exists(s->main_dir))
        import_dir = path_dir(prelude_key, s->arena);

    /* Imports are pushed ahead of the file that `use`s them, so a program is
       compiled after everything it depends on. */
    collect_types(s, program, import_dir);
    array_push(s->programs, &main_unit);
    if (s->had_error) return; /* a failed import is fatal */

    for (size_t i = 0; i < array_length(s->programs); i++) {
        ProgramUnit *u = *(ProgramUnit **)array_get(s->programs, i);
        diag_set_source(s->diag, u->file, u->source);
        collect_defs(s, u->program);
    }
    s->defer_inst = false;
    check_pending_instances(s);

    for (size_t i = 0; i < array_length(s->programs); i++) {
        ProgramUnit *u = *(ProgramUnit **)array_get(s->programs, i);
        diag_set_source(s->diag, u->file, u->source);
        Array *stmts = u->program->as.program.stmts;
        for (size_t j = 0; j < array_length(stmts); j++) {
            check_stmt(s, *(ASTNode **)array_get(stmts, j), s->global);
        }
    }

    assign_field_offsets(s);
    assign_global_slots(s);
}
