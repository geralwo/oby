#include <stdint.h>
#include <string.h>

#include "lsp/query.h"

size_t lsp_line_start(const char *source, size_t source_len, int line) {
    if (line <= 0) return 0;
    size_t offset = 0;
    for (int current = 0; current < line && offset < source_len; current++) {
        while (offset < source_len && source[offset] != '\n') offset++;
        if (offset < source_len) offset++;   /* past the '\n' */
    }
    return offset;
}

/* Where a line's content ends: at its '\n' or a preceding '\r' (a CRLF
   terminator is not part of the line — positions cannot denote it). */
static size_t line_content_end(const char *source, size_t source_len, size_t start) {
    size_t end = start;
    while (end < source_len && source[end] != '\n' && source[end] != '\r') end++;
    return end;
}

/* UTF-16 code units in `source[start..stop)`. A malformed byte counts one, so
   the result stays monotone on broken text. */
static int utf16_units(const char *source, size_t start, size_t stop) {
    int units = 0;
    for (size_t i = start; i < stop; i++) {
        unsigned char c = (unsigned char)source[i];
        if ((c & 0xC0) == 0x80) continue;                   /* continuation */
        units += (c & 0xF8) == 0xF0 ? 2 : 1;                /* 4-byte lead: a pair */
    }
    return units;
}

LspPosition lsp_position_of(const char *source, size_t source_len,
                            size_t offset, const char *encoding) {
    if (offset > source_len) offset = source_len;

    int line = 0;
    size_t start = 0;
    for (size_t i = 0; i < offset; i++) {
        if (source[i] == '\n') {
            line++;
            start = i + 1;
        }
    }

    size_t content_end = line_content_end(source, source_len, start);
    size_t stop = offset < content_end ? offset : content_end;

    LspPosition p;
    p.line = line;
    p.character = strcmp(encoding, "utf-8") == 0
                      ? (int)(stop - start)
                      : utf16_units(source, start, stop);
    return p;
}

size_t lsp_offset_of(const char *source, size_t source_len,
                     LspPosition position, const char *encoding) {
    size_t start = lsp_line_start(source, source_len, position.line);
    if (position.character <= 0) return start;
    size_t content_end = line_content_end(source, source_len, start);

    if (strcmp(encoding, "utf-8") == 0) {
        size_t offset = start + (size_t)position.character;
        return offset < content_end ? offset : content_end;
    }

    /* UTF-16: walk whole characters, stopping on the last boundary that does
       not pass the requested count. */
    int units = 0;
    size_t offset = start;
    while (offset < content_end) {
        unsigned char c = (unsigned char)source[offset];
        int width = (c & 0xC0) == 0x80 ? 0 : ((c & 0xF8) == 0xF0 ? 2 : 1);
        if (units + width > position.character) break;
        units += width;
        offset++;
        while (offset < content_end && ((unsigned char)source[offset] & 0xC0) == 0x80) {
            offset++;   /* the rest of this character */
        }
    }
    return offset;
}

/* ------------------------------------------------------------------ */
/* Cursor -> AST target                                               */
/* ------------------------------------------------------------------ */

/* True when the token's text lies inside the buffer. Tokens from other files
   have pointers outside it; the comparison goes through `uintptr_t` so it
   stays defined for them. */
static bool token_in_buffer(const char *source, size_t source_len, Token tok,
                            size_t *start) {
    if (!tok.start || tok.length == 0) return false;
    uintptr_t base = (uintptr_t)source;
    uintptr_t p = (uintptr_t)tok.start;
    if (p < base) return false;
    size_t at = (size_t)(p - base);
    if (at >= source_len || at + tok.length > source_len) return false;
    if (start) *start = at;
    return true;
}

bool lsp_token_in_source(const char *source, size_t source_len, Token tok) {
    return token_in_buffer(source, source_len, tok, NULL);
}

/* Does the token cover byte `offset`? */
static bool token_hits(Token tok, const char *source, size_t source_len, size_t offset) {
    size_t start = 0;
    if (!token_in_buffer(source, source_len, tok, &start)) return false;
    return offset >= start && offset < start + tok.length;
}

/* The best match so far: the smallest covering token wins, so a child beats
   the statement around it. `segment` names the path segment that matched. */
typedef struct {
    LspTarget target;
    size_t span;
    int segment;
} Hit;

static void take(Hit *hit, ASTNode *node, int segment, size_t span) {
    if (hit->target.node && span >= hit->span) return;
    hit->target.node = node;
    hit->target.sym = NULL;
    hit->target.type = NULL;
    hit->span = span;
    hit->segment = segment;
}

static void scan(ASTNode *node, const char *source, size_t source_len,
                 size_t offset, Hit *hit);

static void scan_list(Array *list, const char *source, size_t source_len,
                      size_t offset, Hit *hit) {
    if (!list) return;
    for (size_t i = 0; i < array_length(list); i++) {
        scan(*(ASTNode **)array_get(list, i), source, source_len, offset, hit);
    }
}

static void consider(ASTNode *node, const char *source, size_t source_len,
                     size_t offset, Hit *hit) {
    if (node->kind == AST_PATH) {
        Array *parts = node->as.path.parts;
        for (size_t i = 0; parts && i < array_length(parts); i++) {
            PathSegment *seg = (PathSegment *)array_get(parts, i);
            if (token_hits(seg->tok, source, source_len, offset)) {
                take(hit, node, (int)i, seg->tok.length);
                return;
            }
        }
    } else if (node->kind == AST_MEMBER_ACCESS) {
        Token tok = node->as.member_access.member_tok;
        if (token_hits(tok, source, source_len, offset)) {
            take(hit, node, -1, tok.length);
            return;
        }
    } else if (node->kind == AST_MEMBER_ASSIGN) {
        Token tok = node->as.member_assign.member_tok;
        if (token_hits(tok, source, source_len, offset)) {
            take(hit, node, -1, tok.length);
            return;
        }
    }

    if (token_hits(node->token, source, source_len, offset)) {
        take(hit, node, -1, node->token.length);
    }
}

static void scan(ASTNode *node, const char *source, size_t source_len,
                 size_t offset, Hit *hit) {
    if (!node) return;

    /* Children first: the span comparison makes the innermost token win. */
    switch (node->kind) {
        case AST_PROGRAM: scan_list(node->as.program.stmts, source, source_len, offset, hit); break;
        case AST_BLOCK:   scan_list(node->as.block.stmts, source, source_len, offset, hit); break;
        case AST_BIND:
            scan(node->as.bind.type, source, source_len, offset, hit);
            scan(node->as.bind.value, source, source_len, offset, hit);
            break;
        case AST_ASSIGN:
            scan(node->as.assign.target, source, source_len, offset, hit);
            scan(node->as.assign.value, source, source_len, offset, hit);
            break;
        case AST_COMPOUND_ASSIGN:
            scan(node->as.compound.target, source, source_len, offset, hit);
            scan(node->as.compound.value, source, source_len, offset, hit);
            break;
        case AST_MEMBER_ASSIGN:
            scan(node->as.member_assign.target, source, source_len, offset, hit);
            scan(node->as.member_assign.value, source, source_len, offset, hit);
            break;
        case AST_MEMBER_ACCESS:
            scan(node->as.member_access.object, source, source_len, offset, hit);
            break;
        case AST_CALL:
            scan(node->as.call.callee, source, source_len, offset, hit);
            scan_list(node->as.call.args, source, source_len, offset, hit);
            break;
        case AST_INDEX:
            scan(node->as.index.target, source, source_len, offset, hit);
            scan(node->as.index.index, source, source_len, offset, hit);
            break;
        case AST_TABLE_LITERAL: scan_list(node->as.table.fields, source, source_len, offset, hit); break;
        case AST_ARRAY_LITERAL:
            scan_list(node->as.array.elements, source, source_len, offset, hit);
            scan(node->as.array.repeat, source, source_len, offset, hit);
            scan(node->as.array.count_expr, source, source_len, offset, hit);
            break;
        case AST_INTERPOLATED_STRING:
            scan_list(node->as.interpolated.parts, source, source_len, offset, hit);
            break;
        case AST_BINARY_EXPR:
            scan(node->as.binary.left, source, source_len, offset, hit);
            scan(node->as.binary.right, source, source_len, offset, hit);
            break;
        case AST_UNARY_EXPR: scan(node->as.unary.operand, source, source_len, offset, hit); break;
        case AST_RETURN:     scan(node->as.ret.value, source, source_len, offset, hit); break;
        case AST_IF:
            scan(node->as.if_stmt.cond, source, source_len, offset, hit);
            scan(node->as.if_stmt.then_branch, source, source_len, offset, hit);
            scan(node->as.if_stmt.else_branch, source, source_len, offset, hit);
            break;
        case AST_WHILE:
            scan(node->as.while_stmt.cond, source, source_len, offset, hit);
            scan(node->as.while_stmt.body, source, source_len, offset, hit);
            break;
        case AST_FOR:
            scan(node->as.for_stmt.init, source, source_len, offset, hit);
            scan(node->as.for_stmt.cond, source, source_len, offset, hit);
            scan(node->as.for_stmt.step, source, source_len, offset, hit);
            scan(node->as.for_stmt.body, source, source_len, offset, hit);
            break;
        case AST_SWITCH:
            scan(node->as.switch_stmt.expr, source, source_len, offset, hit);
            scan_list(node->as.switch_stmt.cases, source, source_len, offset, hit);
            scan(node->as.switch_stmt.default_body, source, source_len, offset, hit);
            break;
        case AST_CASE:
            scan(node->as.case_stmt.value, source, source_len, offset, hit);
            scan(node->as.case_stmt.range_hi, source, source_len, offset, hit);
            scan(node->as.case_stmt.guard, source, source_len, offset, hit);
            scan(node->as.case_stmt.body, source, source_len, offset, hit);
            break;
        case AST_FUNC_LITERAL:
            scan_list(node->as.func.params, source, source_len, offset, hit);
            scan(node->as.func.ret_type, source, source_len, offset, hit);
            scan(node->as.func.body, source, source_len, offset, hit);
            break;
        case AST_EXTERN_DEF:
            scan_list(node->as.extern_def.params, source, source_len, offset, hit);
            scan(node->as.extern_def.ret_type, source, source_len, offset, hit);
            break;
        case AST_PARAM: scan(node->as.param.type, source, source_len, offset, hit); break;
        case AST_PACKED_DEF:
            scan(node->as.packed_def.target, source, source_len, offset, hit);
            scan(node->as.packed_def.element, source, source_len, offset, hit);
            scan(node->as.packed_def.body, source, source_len, offset, hit);
            break;
        case AST_TYPE_DEF:
            scan(node->as.type_def.target, source, source_len, offset, hit);
            scan(node->as.type_def.body, source, source_len, offset, hit);
            break;
        case AST_TYPE_ALIAS:
            scan(node->as.type_alias.type, source, source_len, offset, hit);
            break;
        case AST_ENUM_DEF:
            scan(node->as.enum_def.target, source, source_len, offset, hit);
            if (node->as.enum_def.items) {
                for (size_t i = 0; i < array_length(node->as.enum_def.items); i++) {
                    EnumItem *item = (EnumItem *)array_get(node->as.enum_def.items, i);
                    scan(item->value, source, source_len, offset, hit);
                }
            }
            break;
        case AST_ARRAY_TYPE: scan(node->as.array_type.element, source, source_len, offset, hit); break;
        case AST_FUNC_TYPE:
            scan_list(node->as.func_type.params, source, source_len, offset, hit);
            scan(node->as.func_type.ret, source, source_len, offset, hit);
            break;
        case AST_LITERAL:
        case AST_PATH:
        case AST_NULL:
        case AST_USE:
        case AST_BREAK:
        case AST_CONTINUE:
            break;
    }

    consider(node, source, source_len, offset, hit);
}

/* Fill in the symbol and type for the winning node. */
static void resolve_hit(Hit *hit) {
    ASTNode *node = hit->target.node;
    if (!node) return;

    if (node->kind == AST_PATH && hit->segment >= 0) {
        Array *syms = node->as.path.syms;
        if (syms && (size_t)hit->segment < array_length(syms))
            hit->target.sym = *(Symbol **)array_get(syms, hit->segment);
        /* Sema sometimes records the symbol on the path itself instead —
           a call's callee, for one — and only the last segment can carry it. */
        if (!hit->target.sym && (size_t)hit->segment + 1 == array_length(node->as.path.parts))
            hit->target.sym = node->sym;
    } else if (node->kind == AST_MEMBER_ACCESS || node->kind == AST_MEMBER_ASSIGN) {
        /* Member syntax keeps only the name (unlike paths); the symbol lives
           on the owner type's members scope. */
        const char *name = node->kind == AST_MEMBER_ACCESS
                               ? node->as.member_access.member
                               : node->as.member_assign.member;
        ASTNode *owner_node = node->kind == AST_MEMBER_ACCESS
                                  ? node->as.member_access.object
                                  : node->as.member_assign.target;
        Type *owner = owner_node ? owner_node->type : NULL;
        if (owner && owner->members && name)
            hit->target.sym = scope_lookup_local(owner->members, name);
    } else {
        hit->target.sym = node->sym;
    }

    hit->target.type = hit->target.sym ? hit->target.sym->type : node->type;
    if (!hit->target.type) hit->target.type = node->type;
}

LspTarget lsp_target_at(ASTNode *program, const char *source, size_t source_len,
                        size_t offset) {
    Hit hit;
    memset(&hit, 0, sizeof hit);
    hit.segment = -1;
    scan(program, source, source_len, offset, &hit);
    resolve_hit(&hit);
    return hit.target;
}

bool lsp_target_decl_token(const LspTarget *target, Token *out) {
    if (target->sym) {
        if (target->sym->decl) {
            *out = target->sym->decl->token;
            return true;
        }
        if (target->sym->decl_tok.start) {
            *out = target->sym->decl_tok;
            return true;
        }
    }
    if (target->type && target->type->decl_tok.start) {
        *out = target->type->decl_tok;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Walking the tree for completion                                    */
/* ------------------------------------------------------------------ */

/* Call `visit` for every direct child node of `node` — the same shape `scan`
   walks in its own switch; keep the two in step. */
static void for_each_child(ASTNode *node, void *ctx, void (*visit)(ASTNode *, void *));

static void visit_list(Array *list, void *ctx, void (*visit)(ASTNode *, void *)) {
    if (!list) return;
    for (size_t i = 0; i < array_length(list); i++) {
        visit(*(ASTNode **)array_get(list, i), ctx);
    }
}

static void for_each_child(ASTNode *node, void *ctx, void (*visit)(ASTNode *, void *)) {
    switch (node->kind) {
        case AST_PROGRAM: visit_list(node->as.program.stmts, ctx, visit); return;
        case AST_BLOCK:   visit_list(node->as.block.stmts, ctx, visit); return;
        case AST_BIND:
            visit(node->as.bind.type, ctx);
            visit(node->as.bind.value, ctx);
            return;
        case AST_ASSIGN:
            visit(node->as.assign.target, ctx);
            visit(node->as.assign.value, ctx);
            return;
        case AST_COMPOUND_ASSIGN:
            visit(node->as.compound.target, ctx);
            visit(node->as.compound.value, ctx);
            return;
        case AST_MEMBER_ASSIGN:
            visit(node->as.member_assign.target, ctx);
            visit(node->as.member_assign.value, ctx);
            return;
        case AST_MEMBER_ACCESS:
            visit(node->as.member_access.object, ctx);
            return;
        case AST_CALL:
            visit(node->as.call.callee, ctx);
            visit_list(node->as.call.args, ctx, visit);
            return;
        case AST_INDEX:
            visit(node->as.index.target, ctx);
            visit(node->as.index.index, ctx);
            return;
        case AST_TABLE_LITERAL:
            visit_list(node->as.table.fields, ctx, visit);
            return;
        case AST_ARRAY_LITERAL:
            visit_list(node->as.array.elements, ctx, visit);
            visit(node->as.array.repeat, ctx);
            visit(node->as.array.count_expr, ctx);
            return;
        case AST_INTERPOLATED_STRING:
            visit_list(node->as.interpolated.parts, ctx, visit);
            return;
        case AST_BINARY_EXPR:
            visit(node->as.binary.left, ctx);
            visit(node->as.binary.right, ctx);
            return;
        case AST_UNARY_EXPR:
            visit(node->as.unary.operand, ctx);
            return;
        case AST_RETURN:
            visit(node->as.ret.value, ctx);
            return;
        case AST_IF:
            visit(node->as.if_stmt.cond, ctx);
            visit(node->as.if_stmt.then_branch, ctx);
            visit(node->as.if_stmt.else_branch, ctx);
            return;
        case AST_WHILE:
            visit(node->as.while_stmt.cond, ctx);
            visit(node->as.while_stmt.body, ctx);
            return;
        case AST_FOR:
            visit(node->as.for_stmt.init, ctx);
            visit(node->as.for_stmt.cond, ctx);
            visit(node->as.for_stmt.step, ctx);
            visit(node->as.for_stmt.body, ctx);
            return;
        case AST_SWITCH:
            visit(node->as.switch_stmt.expr, ctx);
            visit_list(node->as.switch_stmt.cases, ctx, visit);
            visit(node->as.switch_stmt.default_body, ctx);
            return;
        case AST_CASE:
            visit(node->as.case_stmt.value, ctx);
            visit(node->as.case_stmt.range_hi, ctx);
            visit(node->as.case_stmt.guard, ctx);
            visit(node->as.case_stmt.body, ctx);
            return;
        case AST_FUNC_LITERAL:
            visit_list(node->as.func.params, ctx, visit);
            visit(node->as.func.ret_type, ctx);
            visit(node->as.func.body, ctx);
            return;
        case AST_EXTERN_DEF:
            visit_list(node->as.extern_def.params, ctx, visit);
            visit(node->as.extern_def.ret_type, ctx);
            return;
        case AST_PARAM:
            visit(node->as.param.type, ctx);
            return;
        case AST_PACKED_DEF:
            visit(node->as.packed_def.target, ctx);
            visit(node->as.packed_def.element, ctx);
            visit(node->as.packed_def.body, ctx);
            return;
        case AST_TYPE_DEF:
            visit(node->as.type_def.target, ctx);
            visit(node->as.type_def.body, ctx);
            return;
        case AST_TYPE_ALIAS:
            visit(node->as.type_alias.type, ctx);
            return;
        case AST_ENUM_DEF:
            visit(node->as.enum_def.target, ctx);
            if (node->as.enum_def.items) {
                for (size_t i = 0; i < array_length(node->as.enum_def.items); i++) {
                    EnumItem *item = (EnumItem *)array_get(node->as.enum_def.items, i);
                    visit(item->value, ctx);
                }
            }
            return;
        case AST_ARRAY_TYPE:
            visit(node->as.array_type.element, ctx);
            return;
        case AST_FUNC_TYPE:
            visit_list(node->as.func_type.params, ctx, visit);
            visit(node->as.func_type.ret, ctx);
            return;
        case AST_LITERAL:
        case AST_PATH:
        case AST_NULL:
        case AST_USE:
        case AST_BREAK:
        case AST_CONTINUE:
            return;
    }
}

/* Does any token in this subtree cover `offset`? */
typedef struct {
    const char *source;
    size_t source_len;
    size_t offset;
    bool found;
} CoverWalk;

static bool subtree_covers(ASTNode *node, const char *source, size_t source_len,
                           size_t offset);

static void cover_visit(ASTNode *child, void *raw) {
    CoverWalk *w = (CoverWalk *)raw;
    if (!w->found && subtree_covers(child, w->source, w->source_len, w->offset))
        w->found = true;
}

static bool subtree_covers(ASTNode *node, const char *source, size_t source_len,
                           size_t offset) {
    if (!node) return false;
    if (token_hits(node->token, source, source_len, offset)) return true;
    if (node->kind == AST_PATH) {
        Array *parts = node->as.path.parts;
        for (size_t i = 0; parts && i < array_length(parts); i++) {
            PathSegment *seg = (PathSegment *)array_get(parts, i);
            if (token_hits(seg->tok, source, source_len, offset)) return true;
        }
    } else if (node->kind == AST_MEMBER_ACCESS) {
        if (token_hits(node->as.member_access.member_tok, source, source_len, offset))
            return true;
    } else if (node->kind == AST_MEMBER_ASSIGN) {
        if (token_hits(node->as.member_assign.member_tok, source, source_len, offset))
            return true;
    }
    CoverWalk w = { source, source_len, offset, false };
    for_each_child(node, &w, cover_visit);
    return w.found;
}

/* The innermost function literal containing `offset`, or NULL at the top
   level. */
typedef struct {
    const char *source;
    size_t source_len;
    size_t offset;
    ASTNode *best;
} FunctionWalk;

static void find_function(ASTNode *node, void *raw);

static void function_child(ASTNode *child, void *raw) {
    find_function(child, raw);
}

static void find_function(ASTNode *node, void *raw) {
    FunctionWalk *w = (FunctionWalk *)raw;
    if (!node || !subtree_covers(node, w->source, w->source_len, w->offset)) return;
    if (node->kind == AST_FUNC_LITERAL) w->best = node;
    for_each_child(node, w, function_child);
}

/* The binds written before the cursor — without descending into nested
   functions or type bodies (their names are not locals here). */
typedef struct {
    const char *source;
    size_t source_len;
    size_t offset;
    Array *out;   /* Symbol* */
} BindWalk;

static void collect_binds(ASTNode *node, void *raw);

static void bind_child(ASTNode *child, void *raw) {
    collect_binds(child, raw);
}

static void collect_binds(ASTNode *node, void *raw) {
    BindWalk *w = (BindWalk *)raw;
    if (!node) return;
    if (node->kind == AST_FUNC_LITERAL || node->kind == AST_TYPE_DEF ||
        node->kind == AST_PACKED_DEF || node->kind == AST_ENUM_DEF)
        return;
    if (node->kind == AST_BIND && node->sym) {
        size_t start = 0;
        if (token_in_buffer(w->source, w->source_len, node->token, &start) &&
            start < w->offset)
            array_push(w->out, &node->sym);
    }
    for_each_child(node, w, bind_child);
}

void lsp_visible_bindings(ASTNode *program, const char *source, size_t source_len,
                          size_t offset, Array *out) {
    FunctionWalk fw = { source, source_len, offset, NULL };
    find_function(program, &fw);

    if (fw.best) {
        Array *params = fw.best->as.func.params;
        for (size_t i = 0; params && i < array_length(params); i++) {
            ASTNode *param = *(ASTNode **)array_get(params, i);
            if (param && param->sym) array_push(out, &param->sym);
        }
        BindWalk bw = { source, source_len, offset, out };
        collect_binds(fw.best->as.func.body, &bw);
    } else {
        BindWalk bw = { source, source_len, offset, out };
        collect_binds(program, &bw);
    }
}

/* An identifier byte: the characters a completion prefix may hold. */
static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

LspCompletion lsp_completion_at(const char *source, size_t source_len, size_t offset) {
    LspCompletion c;
    memset(&c, 0, sizeof c);
    c.kind = LSP_COMPLETE_NAME;
    if (offset > source_len) offset = source_len;

    /* The partial identifier right before the cursor. */
    size_t start = offset;
    while (start > 0 && is_ident_char(source[start - 1])) start--;
    size_t plen = offset - start;
    if (plen > sizeof c.prefix - 1) plen = sizeof c.prefix - 1;
    memcpy(c.prefix, source + start, plen);
    c.prefix[plen] = '\0';
    c.prefix_len = plen;

    /* What precedes it: `.`, `::`, or nothing. */
    size_t at = start;   /* where the receiver's text ends */
    if (start >= 1 && source[start - 1] == '.') {
        c.kind = LSP_COMPLETE_MEMBER;
        c.receiver_end = start - 1;
        at = start - 1;
    } else if (start >= 2 && source[start - 1] == ':' && source[start - 2] == ':') {
        c.kind = LSP_COMPLETE_STATIC;
        c.receiver_end = start - 2;
        at = start - 2;
    } else {
        return c;
    }

    size_t rstart = at;
    while (rstart > 0 && is_ident_char(source[rstart - 1])) rstart--;
    size_t rlen = at - rstart;
    if (rlen > sizeof c.receiver - 1) rlen = sizeof c.receiver - 1;
    memcpy(c.receiver, source + rstart, rlen);
    c.receiver[rlen] = '\0';
    c.receiver_len = rlen;
    return c;
}
