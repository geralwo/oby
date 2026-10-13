#include <stdlib.h>
#include <string.h>
#include "compiler/codegen.h"
#include "vm/opcode.h"
#include "core/string.h"
#include "native/native.h"
#include "util.h"

/* A tagged value occupies one `Value` on the stack. */
#define VSLOT ((size_t)sizeof(Value))
#define NO_REG ((size_t)-1)

typedef struct {
    Symbol *sym;
    size_t  off;   /* byte offset */
} Local;

/* An enclosing loop, for `break`. */
typedef struct {
    int start;       /* the continue target when it is already emitted (while) */
    bool forward;    /* `for`: the continue target (the step) is not emitted yet */
    Array *breaks;   /* positions of `break` jumps, patched at the loop end */
    Array *continues;/* positions of `continue` jumps, patched to the step (for) */
} LoopEntry;

typedef struct {
    Sema        *sema;
    Arena       *arena;
    Diagnostics *diag;
    Function    *fn;         /* function currently being compiled */
    Type        *ret_type;   /* declared return type of `fn` */
    size_t       next_off;   /* next free byte offset */
    size_t       frame_size; /* high-water mark, bytes */
    Array       *functions;  /* Array of Function* */
    Array       *locals;     /* Array of Local */
    size_t       locals_base;/* first index of the current function's locals */
    Array       *loops;      /* Array of LoopEntry (innermost last) */
    Array       *caps;       /* Array of Symbol*: the current function's upvalues */
} Codegen;

/* Bytes a value of `node`'s type occupies. */
static size_t node_size(ASTNode *node) {
    return type_size(node ? node->type : NULL);
}

/* True when a `{}` literal writes `m` itself (so its default must not run). */
static bool table_writes_member(ASTNode *table, Symbol *m) {
    Array *fields = table->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *f = *(ASTNode **)array_get(fields, i);
        if (f && f->sym == m) return true;
    }
    return false;
}

/* True if `t` is the contextual keyword `kw`. */
static bool token_is_word(Token t, const char *kw) {
    size_t len = strlen(kw);
    return t.kind == TOKEN_IDENTIFIER && t.length == len && memcmp(t.start, kw, len) == 0;
}

/* The storage kind of a type: tagged (a 16-byte Value) or a packed scalar.
   Packed aggregates (user packed types) are byte-copied, never scalars. */
static int kind_of(Type *t) {
    if (!t) return KIND_TAGGED;
    if (t->kind == TYPE_ENUM) return kind_of(t->base);
    if (t->kind != TYPE_PACKED) return KIND_TAGGED;
    if (t->count > 0) return KIND_TAGGED;
    if (t->is_float) return t->size == 8 ? KIND_FLOAT64 : KIND_FLOAT32;
    switch (t->size) {
        case 1:  return KIND_BYTE;
        case 4:  return type_is_unsigned(t) ? KIND_UINT32 : KIND_INT32;
        case 8:  return type_is_unsigned(t) ? KIND_UINT64 : KIND_INT64;
        default: return KIND_TAGGED;   /* user packed types are not scalars */
    }
}

/* The type two operands are evaluated as: the wider of the two. */
static Type *common_type(Type *a, Type *b) {
    if (!a) return b;
    if (!b) return a;
    if (a == b) return a;
    if (kind_of(a) == KIND_TAGGED || kind_of(b) == KIND_TAGGED) return a;
    return type_size(a) >= type_size(b) ? a : b;
}

/* ------------------------------------------------------------------ */
/* Registers (byte offsets)                                           */
/* ------------------------------------------------------------------ */

static size_t reg_alloc(Codegen *cg, size_t size) {
    size_t off = cg->next_off;
    cg->next_off += size;
    if (cg->next_off > cg->frame_size) cg->frame_size = cg->next_off;
    return off;
}

static void reg_free(Codegen *cg, size_t size) { cg->next_off -= size; }

static void emit(Codegen *cg, Instruction ins) { function_emit(cg->fn, ins); }

/* ------------------------------------------------------------------ */
/* Locals                                                             */
/* ------------------------------------------------------------------ */

static void local_set(Codegen *cg, Symbol *sym, size_t off) {
    Local l = { sym, off };
    array_push(cg->locals, &l);
}

static size_t local_get(Codegen *cg, Symbol *sym) {
    /* Only the current function's locals are visible: an enclosing function's
       locals are below `locals_base`, so a nested body resolves them as
       upvalues instead of reading a same-offset slot. */
    for (size_t i = array_length(cg->locals); i > cg->locals_base; i--) {
        Local *l = (Local *)array_get(cg->locals, i - 1);
        if (l->sym == sym) return l->off;
    }
    return NO_REG;
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void gen_stmt(Codegen *cg, ASTNode *node);
static void gen_expr_to(Codegen *cg, ASTNode *node, size_t dest);
static Function *gen_function(Codegen *cg, ASTNode *func, const char *name);
static void gen_assign(Codegen *cg, Symbol *sym, ASTNode *value);

/* Evaluate `node` into `dest` as a value of type `want`, converting between
   packed kinds when the expression's own type has a different width. */
static void gen_expr_as(Codegen *cg, ASTNode *node, size_t dest, Type *want) {
    int wk = kind_of(want);
    int nk = kind_of(node ? node->type : NULL);
    if (nk == wk || nk == KIND_TAGGED || wk == KIND_TAGGED) {
        gen_expr_to(cg, node, dest);
        return;
    }
    size_t tmp = reg_alloc(cg, node_size(node));
    gen_expr_to(cg, node, tmp);
    emit(cg, INSTR_ABCN(OP_CONV, dest, tmp, nk, wk));
    reg_free(cg, node_size(node));
}

/* ------------------------------------------------------------------ */
/* Loads                                                              */
/* ------------------------------------------------------------------ */

/* The upvalue slot of `sym` in the function being compiled, or -1. */
static int cap_index(Codegen *cg, Symbol *sym) {
    if (!cg->caps || !sym) return -1;
    for (size_t i = 0; i < array_length(cg->caps); i++) {
        if (*(Symbol **)array_get(cg->caps, i) == sym) return (int)i;
    }
    return -1;
}

static void gen_symbol_to(Codegen *cg, Symbol *sym, size_t dest) {
    if (!sym) { emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0)); return; }
    if (sym->has_const) {
        int k = function_add_const(cg->fn, val_int((int64_t)sym->const_value));
        emit(cg, INSTR_ABXN(OP_LOADK, dest, k, kind_of(sym->type)));
        return;
    }
    if (sym->is_global) {
        emit(cg, INSTR_ABXN(OP_GETGLOBAL, dest, sym->slot, (int)type_size(sym->type)));
    } else {
        size_t r = local_get(cg, sym);
        if (r != NO_REG) {
            if (r != dest)
                emit(cg, INSTR_ABCN(OP_MOVEN, dest, r, 0, (int)type_size(sym->type)));
        } else {
            /* A local of an enclosing function: read it from the closure. */
            int idx = cap_index(cg, sym);
            if (idx >= 0)
                emit(cg, INSTR_ABC(OP_GETUPVAL, dest, idx, (int)type_size(sym->type)));
        }
    }
}

/* Evaluate the first `count` segments of a path and return the byte offset of
   the resulting value. `*is_temp` is set when that offset is a scratch copy
   (a global, or a tagged member) rather than the value's own storage; the
   caller must restore `cg->next_off` afterwards. Packed member access is pure
   offset arithmetic on the parent's storage. */
static size_t gen_chain_loc(Codegen *cg, Array *syms, size_t count,
                            Type **out_type, bool *is_temp) {
    Symbol *first = *(Symbol **)array_get(syms, 0);
    Type *cur_type = first->type;

    if (count == 1) {
        if (first->kind != SYM_TYPE && !first->is_global && !first->has_const) {
            size_t off = local_get(cg, first);
            if (off != NO_REG) {
                if (out_type) *out_type = cur_type;
                if (is_temp) *is_temp = false;
                return off;
            }
        }
        size_t off = reg_alloc(cg, type_size(cur_type));
        gen_symbol_to(cg, first, off);
        if (out_type) *out_type = cur_type;
        if (is_temp) *is_temp = true;
        return off;
    }

    bool parent_temp = false;
    Type *parent_type = NULL;
    size_t off = gen_chain_loc(cg, syms, count - 1, &parent_type, &parent_temp);
    Symbol *m = *(Symbol **)array_get(syms, count - 1);

    /* A static member (`value::name`, where `value` is an object rather than a
       type) lives in global storage, not inside the parent value. */
    if (m->is_global) {
        size_t next = reg_alloc(cg, type_size(m->type));
        gen_symbol_to(cg, m, next);
        if (out_type) *out_type = m->type;
        if (is_temp) *is_temp = true;
        return next;
    }

    if (parent_type && parent_type->kind == TYPE_PACKED && parent_type->count > 0) {
        off += (size_t)m->slot * type_size(parent_type->base);
        if (out_type) *out_type = m->type;
        if (is_temp) *is_temp = parent_temp;
        return off;
    }

    size_t next = reg_alloc(cg, type_size(m->type));
    emit(cg, INSTR_ABCN(OP_GETFIELD, next, off, m->slot, (int)type_size(m->type)));
    if (out_type) *out_type = m->type;
    if (is_temp) *is_temp = true;
    return next;
}

/* Store `size` bytes from `src` into the lvalue denoted by the first `count`
   segments of a path. Locals are written in place; globals and object members
   are read-modify-written, recursing when a packed parent is itself a copy. */
static void gen_chain_store(Codegen *cg, Array *syms, size_t count,
                            size_t src, size_t size) {
    Symbol *first = *(Symbol **)array_get(syms, 0);

    if (count == 1) {
        if (first->is_global) {
            emit(cg, INSTR_ABXN(OP_SETGLOBAL, src, first->slot, (int)size));
        } else {
            size_t off = local_get(cg, first);
            if (off != NO_REG && off != src)
                emit(cg, INSTR_ABCN(OP_MOVEN, off, src, 0, (int)size));
        }
        return;
    }

    size_t saved = cg->next_off;
    Type *pt = NULL;
    bool ptemp = false;
    size_t parent = gen_chain_loc(cg, syms, count - 1, &pt, &ptemp);
    Symbol *m = *(Symbol **)array_get(syms, count - 1);

    if (pt && pt->kind == TYPE_PACKED && pt->count > 0) {
        size_t moff = (size_t)m->slot * type_size(pt->base);
        emit(cg, INSTR_ABCN(OP_MOVEN, parent + moff, src, 0, (int)size));
        if (ptemp) gen_chain_store(cg, syms, count - 1, parent, type_size(pt));
    } else {
        emit(cg, INSTR_ABCN(OP_SETFIELD, parent, m->slot, src, (int)type_size(m->type)));
    }
    cg->next_off = saved;
}

/* Evaluate `node` into a stack location and return its byte offset. The caller
   must restore `cg->next_off` afterwards. */
static size_t gen_expr_loc(Codegen *cg, ASTNode *node, Type **out_type, bool *is_temp) {
    if (node && node->kind == AST_PATH && node->as.path.syms &&
        array_length(node->as.path.syms) > 0) {
        Symbol *first = *(Symbol **)array_get(node->as.path.syms, 0);
        if (first->kind != SYM_TYPE)
            return gen_chain_loc(cg, node->as.path.syms,
                                 array_length(node->as.path.syms), out_type, is_temp);
    }
    size_t off = reg_alloc(cg, node_size(node));
    gen_expr_to(cg, node, off);
    if (out_type) *out_type = node ? node->type : NULL;
    if (is_temp) *is_temp = true;
    return off;
}

/* Store `size` bytes from `src` at `off` bytes into the packed value denoted
   by `base`, writing the modified value back when `base` is a copy. */
static void gen_packed_sub_store(Codegen *cg, ASTNode *base, size_t off,
                                 size_t src, size_t size) {
    size_t saved = cg->next_off;
    Type *bt = NULL;
    bool temp = false;
    size_t loc = gen_expr_loc(cg, base, &bt, &temp);
    emit(cg, INSTR_ABCN(OP_MOVEN, loc + off, src, 0, (int)size));
    if (temp) {
        if (base->kind == AST_PATH && base->as.path.syms) {
            gen_chain_store(cg, base->as.path.syms, array_length(base->as.path.syms),
                            loc, type_size(bt));
        } else if (base->kind == AST_INDEX && bt && bt->kind == TYPE_PACKED &&
                   bt->count > 0) {
            /* A packed array element: write the modified copy back. */
            ASTNode *arr_node = base->as.index.target;
            Type *at = arr_node ? arr_node->type : NULL;
            if (at && at->kind == TYPE_ARRAY) {
                size_t arr = reg_alloc(cg, VSLOT);
                gen_expr_to(cg, arr_node, arr);
                size_t idx = reg_alloc(cg, type_size(cg->sema->types.int_type));
                gen_expr_as(cg, base->as.index.index, idx, cg->sema->types.int_type);
                emit(cg, INSTR_ABC(OP_SETINDEX, arr, idx, loc));
            }
        }
    }
    cg->next_off = saved;
}

/* Load the value denoted by a path. A path whose first segment is a type is a
   static reference (a global slot); otherwise it is an instance chain. */
static void gen_path_to(Codegen *cg, ASTNode *node, size_t dest) {
    Array *syms = node->as.path.syms;
    size_t n = syms ? array_length(syms) : 0;
    if (n == 0) { emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0)); return; }

    Symbol *first = *(Symbol **)array_get(syms, 0);

    if (n == 1 || first->kind == SYM_TYPE) {
        Symbol *last = *(Symbol **)array_get(syms, n - 1);
        gen_symbol_to(cg, last, dest);
        return;
    }

    size_t saved = cg->next_off;
    Type *t = NULL;
    bool temp = false;
    size_t off = gen_chain_loc(cg, syms, n, &t, &temp);
    if (off != dest) emit(cg, INSTR_ABCN(OP_MOVEN, dest, off, 0, (int)type_size(t)));
    cg->next_off = saved;
}

/* ------------------------------------------------------------------ */
/* Jumps                                                              */
/* ------------------------------------------------------------------ */

static int emit_jump(Codegen *cg, OpCode op, int a) {
    emit(cg, INSTR_ABX(op, a, 0));
    return (int)cg->fn->code_len - 1;
}

static void patch_jump(Codegen *cg, int at) {
    int offset = (int)cg->fn->code_len - at - 1;
    cg->fn->code[at] = INSTR_ABX(OP_JMP, 0, (uint32_t)offset);
}

/* Jump past the body unless `cond` holds: TEST skips the jump when the
   condition is true, so the jump is taken when the case does not match.
   `jumps` collects its index for a later patch. */
static void jump_unless(Codegen *cg, size_t cond, int kind, Array *jumps) {
    emit(cg, INSTR_ABCN(OP_TEST, cond, 0, 0, kind));
    int j = emit_jump(cg, OP_JMP, 0);
    array_push(jumps, &j);
}

/* ------------------------------------------------------------------ */
/* Calls                                                              */
/* ------------------------------------------------------------------ */

/* Array growth intrinsics (`arr.push(x)`, `.pop()`, `.insert(i, x)`,
   `.remove(i)`). Sema rewrote the callee to the receiver and set `builtin`. */
static void gen_array_builtin(Codegen *cg, ASTNode *node, size_t dest) {
    ASTNode *recv = node->as.call.callee;
    Array *args = node->as.call.args;
    size_t argc = array_length(args);
    Type *et = (recv->type && recv->type->kind == TYPE_ARRAY) ? recv->type->base : NULL;
    size_t esize = type_size(et);
    Type *it = cg->sema->types.int_type;

    size_t saved = cg->next_off;
    size_t arr = reg_alloc(cg, VSLOT);
    gen_expr_to(cg, recv, arr);   /* the array value */

    switch (node->as.call.builtin) {
        case BUILTIN_ARRAY_PUSH:
            if (argc >= 1) {
                size_t v = reg_alloc(cg, esize);
                gen_expr_as(cg, *(ASTNode **)array_get(args, 0), v, et);
                emit(cg, INSTR_ABCN(OP_APPEND, arr, v, 0, (int)esize));
                reg_free(cg, esize);
            }
            break;
        case BUILTIN_ARRAY_POP:
            emit(cg, INSTR_ABC(OP_ARRAY_POP, arr, dest, 0));
            break;
        case BUILTIN_ARRAY_INSERT:
            if (argc >= 2) {
                size_t idx = reg_alloc(cg, type_size(it));
                gen_expr_as(cg, *(ASTNode **)array_get(args, 0), idx, it);
                size_t v = reg_alloc(cg, esize);
                gen_expr_as(cg, *(ASTNode **)array_get(args, 1), v, et);
                emit(cg, INSTR_ABC(OP_ARRAY_INSERT, arr, idx, v));
                reg_free(cg, type_size(it) + esize);
            }
            break;
        case BUILTIN_ARRAY_REMOVE:
            if (argc >= 1) {
                size_t idx = reg_alloc(cg, type_size(it));
                gen_expr_as(cg, *(ASTNode **)array_get(args, 0), idx, it);
                emit(cg, INSTR_ABC(OP_ARRAY_REMOVE, arr, idx, dest));
                reg_free(cg, type_size(it));
            }
            break;
        default:
            break;
    }
    cg->next_off = saved;
}

static void gen_call_to(Codegen *cg, ASTNode *node, size_t dest) {
    if (node->as.call.builtin != BUILTIN_NONE) {
        gen_array_builtin(cg, node, dest);
        return;
    }
    ASTNode *callee = node->as.call.callee;
    Array *args = node->as.call.args;
    size_t argc = array_length(args);
    Symbol *target = callee->sym;
    Type *ft = callee->type;

    /* A native call needs no callee value: args at R[base..], result at R[base].
       Packed arguments are boxed into their 16-byte slots. */
    if (target && target->native_id >= 0) {
        size_t base = reg_alloc(cg, VSLOT);
        for (size_t i = 1; i < argc; i++) reg_alloc(cg, VSLOT);
        for (size_t i = 0; i < argc; i++) {
            ASTNode *arg = *(ASTNode **)array_get(args, i);
            Type *pt = (ft && ft->kind == TYPE_FUNC && i < ft->param_count) ? ft->params[i] : NULL;
            int k = kind_of(pt);
            if (k == KIND_TAGGED) {
                gen_expr_as(cg, arg, base + i * VSLOT, pt);
            } else {
                size_t tmp = reg_alloc(cg, type_size(pt));
                gen_expr_as(cg, arg, tmp, pt);
                emit(cg, INSTR_ABCN(OP_BOX, base + i * VSLOT, tmp, 0, k));
                reg_free(cg, type_size(pt));
            }
        }
        emit(cg, INSTR_ABC(OP_CALLNATIVE, base, target->native_id, (int)argc));
        int rk = kind_of(node->type);
        if (node->type && node->type->kind == TYPE_VOID) {
            /* no result */
        } else if (rk == KIND_TAGGED) {
            if (base != dest) emit(cg, INSTR_ABC(OP_MOVE, dest, base, 0));
        } else {
            emit(cg, INSTR_ABCN(OP_UNBOX, dest, base, 0, rk));
        }
        reg_free(cg, argc * VSLOT);
        return;
    }

    /* `T()` on a packed type with no matching `::new` zero-initializes. */
    if (!target && ft && ft->kind == TYPE_PACKED) {
        emit(cg, INSTR_ABC(OP_ZERO, dest, (int)node_size(node), 0));
        return;
    }

    /* Arguments are laid out by size, matching the callee's own layout. */
    size_t base = reg_alloc(cg, VSLOT);   /* callee */
    size_t *offs = (size_t *)arena_alloc(cg->arena, sizeof(size_t) * (argc ? argc : 1));
    size_t off = VSLOT;
    for (size_t i = 0; i < argc; i++) {
        offs[i] = off;
        size_t sz = (ft && ft->kind == TYPE_FUNC && i < ft->param_count)
                        ? type_size(ft->params[i]) : VSLOT;
        reg_alloc(cg, sz);
        off += sz;
    }
    /* A method called on an expression (`f().m()`): the receiver is both read
       (to load the method's value) and passed as argument 0, so evaluate it
       once and read the member from that slot. */
    bool recv_once = target && target->kind == SYM_MEMBER && !target->is_static &&
                     argc > 0 && callee->kind == AST_MEMBER_ACCESS &&
                     !callee->as.member_access.is_static &&
                     *(ASTNode **)array_get(args, 0) == callee->as.member_access.object;

    /* A non-static member has no local/global slot of its own, so load it from
       the object it belongs to (e.g. a function stored in a `{}` member). */
    if (recv_once) {
        ASTNode *recv = *(ASTNode **)array_get(args, 0);
        Type *pt0 = (ft && ft->kind == TYPE_FUNC && ft->param_count > 0) ? ft->params[0]
                                                                        : NULL;
        size_t rl = base + offs[0];
        gen_expr_as(cg, recv, rl, pt0);
        Type *ot = recv->type;
        if (ot && ot->kind == TYPE_PACKED && ot->count > 0) {
            size_t off = rl + (size_t)target->slot * type_size(ot->base);
            emit(cg, INSTR_ABCN(OP_MOVEN, base, off, 0, (int)type_size(target->type)));
        } else {
            emit(cg, INSTR_ABCN(OP_GETFIELD, base, rl, target->slot,
                                (int)type_size(target->type)));
        }
    } else if (target && !(target->kind == SYM_MEMBER && !target->is_static)) {
        gen_symbol_to(cg, target, base);
    } else {
        gen_expr_to(cg, callee, base);
    }
    for (size_t i = 0; i < argc; i++) {
        if (recv_once && i == 0) continue;   /* already evaluated into its slot */
        ASTNode *arg = *(ASTNode **)array_get(args, i);
        Type *pt = (ft && ft->kind == TYPE_FUNC && i < ft->param_count) ? ft->params[i] : NULL;
        gen_expr_as(cg, arg, base + offs[i], pt);
    }
    emit(cg, INSTR_ABC(OP_CALL, base, (int)argc, 1));
    if (base != dest)
        emit(cg, INSTR_ABCN(OP_MOVEN, dest, base, 0, (int)node_size(node)));
    reg_free(cg, off);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

static void gen_expr_to(Codegen *cg, ASTNode *node, size_t dest) {
    if (!node) { emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0)); return; }

    switch (node->kind) {
        case AST_LITERAL: {
            Value v;
            if (node->token.kind == TOKEN_FLOAT)
                v = val_float(atof(node->as.literal.value));
            else if (node->token.kind == TOKEN_NUMBER || node->token.kind == TOKEN_CHAR) {
                /* 64-bit wide: a literal above int64 range is uint64, and the
                   bits must survive. Sema has already rejected anything out of
                   range, so a parse failure here can only mean a `0` value. */
                unsigned long long u = 0;
                parse_int_literal(node->as.literal.value, &u);
                v = val_int((int64_t)u);
            } else
                v = val_string(string_new_len(node->as.literal.value, node->as.literal.len));
            int k = function_add_const(cg->fn, v);
            emit(cg, INSTR_ABXN(OP_LOADK, dest, k, kind_of(node->type)));
            break;
        }

        case AST_PATH:
            gen_path_to(cg, node, dest);
            break;

        case AST_NULL:
            emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0));
            break;

        case AST_MEMBER_ACCESS: {
            ASTNode *obj_node = node->as.member_access.object;
            size_t saved = cg->next_off;
            Type *ot = NULL;
            bool temp = false;
            size_t obj = gen_expr_loc(cg, obj_node, &ot, &temp);
            if (ot && ot->kind == TYPE_PACKED && ot->count > 0) {
                size_t off = obj + (size_t)(node->sym ? node->sym->slot : 0) *
                                       type_size(ot->base);
                emit(cg, INSTR_ABCN(OP_MOVEN, dest, off, 0, (int)node_size(node)));
            } else {
                emit(cg, INSTR_ABCN(OP_GETFIELD, dest, obj,
                                    node->sym ? node->sym->slot : 0, (int)node_size(node)));
            }
            cg->next_off = saved;
            break;
        }

        case AST_INDEX: {
            ASTNode *target = node->as.index.target;
            Type *tt = target ? target->type : NULL;
            if (tt && tt->kind == TYPE_PACKED && tt->count > 0) {
                size_t saved = cg->next_off;
                Type *bt = NULL;
                bool temp = false;
                size_t loc = gen_expr_loc(cg, target, &bt, &temp);
                size_t off = loc + (size_t)node->as.index.const_index * type_size(tt->base);
                emit(cg, INSTR_ABCN(OP_MOVEN, dest, off, 0, (int)node_size(node)));
                cg->next_off = saved;
                break;
            }
            if (tt && tt->kind == TYPE_ARRAY) {
                size_t saved = cg->next_off;
                size_t arr = reg_alloc(cg, VSLOT);
                gen_expr_to(cg, target, arr);
                size_t idx = reg_alloc(cg, type_size(cg->sema->types.int_type));
                gen_expr_as(cg, node->as.index.index, idx, cg->sema->types.int_type);
                emit(cg, INSTR_ABC(OP_GETINDEX, dest, arr, idx));
                cg->next_off = saved;
                break;
            }
            emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0));
            break;
        }

        case AST_ARRAY_LITERAL: {
            Type *t = node->type;
            if (t && t->kind == TYPE_PACKED && t->count > 0) {
                /* A packed aggregate literal: lay the elements out inline. */
                size_t esize = type_size(t->base);
                Array *elems = node->as.array.elements;
                for (size_t i = 0; i < array_length(elems) && i < t->count; i++) {
                    ASTNode *e = *(ASTNode **)array_get(elems, i);
                    gen_expr_as(cg, e, dest + i * esize, t->base);
                }
                break;
            }
            if (t && t->kind == TYPE_ARRAY) {
                /* A heap array: allocate, then append each element. */
                Array *elems = node->as.array.elements;
                size_t esize = type_size(t->base);
                if (node->as.array.repeat && !node->as.array.count_const) {
                    /* `[expr; n]` with a runtime count: start empty and let
                       the VM append n copies of the value (it is evaluated
                       once, like the constant form). */
                    emit(cg, INSTR_ABC(OP_NEWARRAY, dest, (int)esize, 0));
                    size_t isz = type_size(cg->sema->types.int_type);
                    size_t cnt = reg_alloc(cg, isz);
                    gen_expr_as(cg, node->as.array.count_expr, cnt,
                                cg->sema->types.int_type);
                    size_t v = reg_alloc(cg, esize);
                    gen_expr_as(cg, node->as.array.repeat, v, t->base);
                    emit(cg, INSTR_ABC(OP_ARRAY_REPEAT, dest, cnt, v));
                    reg_free(cg, esize);
                    reg_free(cg, isz);
                    break;
                }
                size_t n = node->as.array.repeat ? node->as.array.count
                                                 : array_length(elems);
                emit(cg, INSTR_ABC(OP_NEWARRAY, dest, (int)esize, (int)n));
                if (node->as.array.repeat) {
                    /* `[expr; n]`: evaluate once, append n times. */
                    size_t v = reg_alloc(cg, esize);
                    gen_expr_as(cg, node->as.array.repeat, v, t->base);
                    for (size_t i = 0; i < n; i++)
                        emit(cg, INSTR_ABCN(OP_APPEND, dest, v, 0, (int)esize));
                    reg_free(cg, esize);
                } else {
                    for (size_t i = 0; i < n; i++) {
                        ASTNode *e = *(ASTNode **)array_get(elems, i);
                        size_t v = reg_alloc(cg, esize);
                        gen_expr_as(cg, e, v, t->base);
                        emit(cg, INSTR_ABCN(OP_APPEND, dest, v, 0, (int)esize));
                        reg_free(cg, esize);
                    }
                }
                /* A fixed `T[n]` cannot be resized, even through a `T[]` alias. */
                if (t->size != 0) emit(cg, INSTR_ABC(OP_ARRAY_FIXED, dest, 0, 0));
                break;
            }
            emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0));
            break;
        }

        case AST_CALL:
            gen_call_to(cg, node, dest);
            break;

        case AST_BINARY_EXPR: {
            ASTNode *ln = node->as.binary.left;
            ASTNode *rn = node->as.binary.right;
            TokenKind tk = node->as.binary.op.kind;

            /* `and` / `or` short-circuit: the right operand is only evaluated
               when the left one does not decide the result. TEST skips the next
               instruction when the left is truthy, so the truthy path comes
               first and the false path is jumped to. */
            if (token_is_word(node->as.binary.op, "and") ||
                token_is_word(node->as.binary.op, "or")) {
                bool is_and = token_is_word(node->as.binary.op, "and");
                Type *it = cg->sema->types.int_type;
                size_t r = reg_alloc(cg, type_size(it));
                gen_expr_as(cg, ln, r, it);
                emit(cg, INSTR_ABCN(OP_TEST, r, 0, 0, KIND_INT32));
                int j_false = emit_jump(cg, OP_JMP, 0);

                /* Left truthy: `and` is (right != 0), `or` is 1. */
                if (is_and) {
                    size_t r2 = reg_alloc(cg, type_size(it));
                    gen_expr_as(cg, rn, r2, it);
                    size_t z = reg_alloc(cg, type_size(it));
                    emit(cg, INSTR_ABXN(OP_LOADK, z,
                                        function_add_const(cg->fn, val_int(0)), KIND_INT32));
                    emit(cg, INSTR_ABCN(OP_NE, r, r2, z, KIND_INT32));
                    reg_free(cg, 2 * type_size(it));
                } else {
                    emit(cg, INSTR_ABXN(OP_LOADK, r,
                                        function_add_const(cg->fn, val_int(1)), KIND_INT32));
                }
                int j_end = emit_jump(cg, OP_JMP, 0);

                /* Left false: `and` is 0, `or` is (right != 0). */
                patch_jump(cg, j_false);
                if (is_and) {
                    emit(cg, INSTR_ABXN(OP_LOADK, r,
                                        function_add_const(cg->fn, val_int(0)), KIND_INT32));
                } else {
                    size_t r2 = reg_alloc(cg, type_size(it));
                    gen_expr_as(cg, rn, r2, it);
                    size_t z = reg_alloc(cg, type_size(it));
                    emit(cg, INSTR_ABXN(OP_LOADK, z,
                                        function_add_const(cg->fn, val_int(0)), KIND_INT32));
                    emit(cg, INSTR_ABCN(OP_NE, r, r2, z, KIND_INT32));
                    reg_free(cg, 2 * type_size(it));
                }
                patch_jump(cg, j_end);

                if (r != dest) emit(cg, INSTR_ABCN(OP_MOVEN, dest, r, 0, (int)type_size(it)));
                reg_free(cg, type_size(it));
                break;
            }

            bool cmp = (tk == TOKEN_EQUAL_EQUAL || tk == TOKEN_BANG_EQUAL ||
                        tk == TOKEN_LT || tk == TOKEN_GT ||
                        tk == TOKEN_LESS_EQUAL || tk == TOKEN_GREATER_EQUAL);

            /* A packed aggregate compares by its bytes; only == and != are
               allowed (sema enforces that). */
            if (cmp && ln->type && ln->type->kind == TYPE_PACKED && ln->type->count > 0) {
                size_t size = type_size(ln->type);
                size_t l = reg_alloc(cg, size);
                gen_expr_to(cg, ln, l);
                size_t r = reg_alloc(cg, size);
                gen_expr_to(cg, rn, r);
                OpCode op = (tk == TOKEN_EQUAL_EQUAL) ? OP_EQPACKED : OP_NEPACKED;
                emit(cg, INSTR_ABCN(op, dest, l, r, (int)size));
                reg_free(cg, 2 * size);
                break;
            }

            /* Shifts: the result is the left operand's type, and the count is
               converted to a 64-bit value so an out-of-range count is caught
               at run time. `>>` is logical for unsigned values, arithmetic for
               signed ones. */
            if (tk == TOKEN_SHIFT_LEFT || tk == TOKEN_SHIFT_RIGHT) {
                Type *lt = ln->type;
                int kind = kind_of(lt);
                size_t l = reg_alloc(cg, type_size(lt));
                gen_expr_as(cg, ln, l, lt);
                size_t r = reg_alloc(cg, 8);
                gen_expr_as(cg, rn, r, cg->sema->types.int64_type);
                OpCode op = (tk == TOKEN_SHIFT_LEFT) ? OP_SHL
                          : (type_is_unsigned(lt) ? OP_SHRU : OP_SAR);
                emit(cg, INSTR_ABCN(op, dest, l, r, kind));
                reg_free(cg, type_size(lt) + 8);
                break;
            }

            /* Comparisons keep their operands' common type; arithmetic uses the
               promoted result type. */
            Type *opnd = cmp ? common_type(ln->type, rn->type) : node->type;
            int kind = kind_of(opnd);
            size_t l = reg_alloc(cg, type_size(opnd));
            gen_expr_as(cg, ln, l, opnd);
            size_t r = reg_alloc(cg, type_size(opnd));
            gen_expr_as(cg, rn, r, opnd);
            bool flt = opnd && opnd->is_float;
            OpCode op;
            switch (tk) {
                case TOKEN_PLUS:          op = flt ? OP_ADD_FLOAT : OP_ADD_INT; break;
                case TOKEN_MINUS:         op = flt ? OP_SUB_FLOAT : OP_SUB_INT; break;
                case TOKEN_STAR:          op = flt ? OP_MUL_FLOAT : OP_MUL_INT; break;
                case TOKEN_SLASH:         op = flt ? OP_DIV_FLOAT : OP_DIV_INT; break;
                case TOKEN_PERCENT:       op = OP_MOD_INT; break;
                case TOKEN_AMP:           op = OP_BAND; break;
                case TOKEN_PIPE:          op = OP_BOR; break;
                case TOKEN_CARET:         op = OP_BXOR; break;
                case TOKEN_EQUAL_EQUAL:   op = OP_EQ; break;
                case TOKEN_BANG_EQUAL:    op = OP_NE; break;
                case TOKEN_LT:            op = OP_LT; break;
                case TOKEN_GT:            op = OP_GT; break;
                case TOKEN_LESS_EQUAL:    op = OP_LE; break;
                case TOKEN_GREATER_EQUAL: op = OP_GE; break;
                default:                  op = OP_ADD_INT; break;
            }
            emit(cg, INSTR_ABCN(op, dest, l, r, kind));
            reg_free(cg, 2 * type_size(opnd));
            break;
        }

        case AST_UNARY_EXPR: {
            ASTNode *on = node->as.unary.operand;
            Type *ot = node->type;
            TokenKind tk = node->as.unary.op.kind;
            if (tk != TOKEN_MINUS && tk != TOKEN_TILDE &&
                !token_is_word(node->as.unary.op, "not"))
                break;   /* unreachable: the parser rejects other unary operators */
            int kind = kind_of(ot);
            size_t r = reg_alloc(cg, type_size(ot));
            gen_expr_as(cg, on, r, ot);
            OpCode op = (tk == TOKEN_MINUS)
                            ? ((ot && ot->is_float) ? OP_NEG_FLOAT : OP_NEG_INT)
                        : (tk == TOKEN_TILDE) ? OP_BNOT
                                              : OP_NOT;
            emit(cg, INSTR_ABCN(op, dest, r, 0, kind));
            reg_free(cg, type_size(ot));
            break;
        }

        case AST_TABLE_LITERAL: {
            Type *t = node->type;
            size_t fields_n = t ? t->size : 0;
            emit(cg, INSTR_ABX(OP_NEWOBJ, dest, (int)fields_n));
            Array *fields = node->as.table.fields;
            for (size_t i = 0; i < array_length(fields); i++) {
                ASTNode *field = *(ASTNode **)array_get(fields, i);
                if (!field || (field->kind != AST_ASSIGN && field->kind != AST_BIND)) continue;
                Symbol *m = field->sym;
                if (!m) continue;
                ASTNode *value = (field->kind == AST_BIND) ? field->as.bind.value
                                                          : field->as.assign.value;
                Type *ft = m->type;
                size_t v = reg_alloc(cg, type_size(ft));
                gen_expr_as(cg, value, v, ft);
                emit(cg, INSTR_ABCN(OP_SETFIELD, dest, m->slot, v, (int)type_size(ft)));
                reg_free(cg, type_size(ft));
            }
            /* Members the literal omits start at their declared default. A
               tagged default (`s = ""`, `xs = [0; 16]`) allocates a fresh value
               per construction, so no two instances share one. */
            if (t && t->members) {
                Array *ms = t->members->symbols;
                for (size_t i = 0; i < array_length(ms); i++) {
                    Symbol *m = *(Symbol **)array_get(ms, i);
                    if (!m || m->is_static || m->kind == SYM_TYPE || !m->init) continue;
                    if (table_writes_member(node, m)) continue;
                    size_t sz = type_size(m->type);
                    size_t v = reg_alloc(cg, sz);
                    gen_expr_as(cg, m->init, v, m->type);
                    emit(cg, INSTR_ABCN(OP_SETFIELD, dest, m->slot, v, (int)sz));
                    reg_free(cg, sz);
                }
            }
            /* Static members declared in the literal (`::name = value`). */
            for (size_t i = 0; i < array_length(fields); i++) {
                ASTNode *field = *(ASTNode **)array_get(fields, i);
                if (field && field->kind == AST_MEMBER_ASSIGN &&
                    field->as.member_assign.is_static && field->sym)
                    gen_assign(cg, field->sym, field->as.member_assign.value);
            }
            break;
        }

        case AST_FUNC_LITERAL: {
            Function *fn = gen_function(cg, node, "anon");
            Array *ups = node->as.func.upvalues;
            size_t nup = ups ? array_length(ups) : 0;
            if (nup == 0 || fn->up_count == 0) {
                int k = function_add_const(cg->fn, val_func(fn));
                emit(cg, INSTR_ABX(OP_LOADK, dest, k));
                break;
            }
            /* Evaluate the captured values into a contiguous block laid out the
               same way as the closure's storage, then build the closure. */
            size_t saved = cg->next_off;
            size_t src = reg_alloc(cg, fn->up_total);
            for (size_t i = 0; i < nup; i++) {
                Symbol *u = *(Symbol **)array_get(ups, i);
                gen_symbol_to(cg, u, src + fn->up_off[i]);
            }
            int k = function_add_const(cg->fn, val_func(fn));
            emit(cg, INSTR_ABC(OP_CLOSURE, dest, k, (int)src));
            cg->next_off = saved;
            break;
        }

        case AST_INTERPOLATED_STRING: {
            /* Concatenate the parts, converting each to a String first. An
               unsigned part formats with its own native: the generic `to_str`
               sees a boxed value and would print a high-bit uint64 signed. */
            int to_str_id = native_find("to_str");
            int uint_str_id = native_find("uint_to_str");
            int uint64_str_id = native_find("uint64_to_str");
            int concat_id = native_find("str_concat");
            Array *parts = node->as.interpolated.parts;
            size_t n = array_length(parts);

            if (n == 0) {
                int k = function_add_const(cg->fn, val_string(string_new_len("", 0)));
                emit(cg, INSTR_ABX(OP_LOADK, dest, k));
                break;
            }

            size_t base = reg_alloc(cg, VSLOT);   /* accumulator */
            reg_alloc(cg, VSLOT);                 /* reserved 2nd concat argument */
            for (size_t i = 0; i < n; i++) {
                ASTNode *part = *(ASTNode **)array_get(parts, i);
                size_t slot = (i == 0) ? base : base + VSLOT;
                int k = kind_of(part->type);
                if (k == KIND_TAGGED) {
                    gen_expr_to(cg, part, slot);
                } else {
                    size_t tmp = reg_alloc(cg, node_size(part));
                    gen_expr_to(cg, part, tmp);
                    emit(cg, INSTR_ABCN(OP_BOX, slot, tmp, 0, k));
                    reg_free(cg, node_size(part));
                }
                if (!part->type || part->type->kind != TYPE_STRING) {
                    int fmt = to_str_id;
                    if (k == KIND_UINT32 && uint_str_id >= 0) fmt = uint_str_id;
                    else if (k == KIND_UINT64 && uint64_str_id >= 0) fmt = uint64_str_id;
                    emit(cg, INSTR_ABC(OP_CALLNATIVE, slot, fmt, 1));
                }
                if (i > 0) emit(cg, INSTR_ABC(OP_CALLNATIVE, base, concat_id, 2));
            }
            if (base != dest) emit(cg, INSTR_ABC(OP_MOVE, dest, base, 0));
            reg_free(cg, 2 * VSLOT);
            break;
        }

        default:
            emit(cg, INSTR_ABC(OP_LOADNIL, dest, 0, 0));
            break;
    }
}

static size_t gen_expr(Codegen *cg, ASTNode *node) {
    size_t r = reg_alloc(cg, node_size(node));
    gen_expr_to(cg, node, r);
    return r;
}

/* ------------------------------------------------------------------ */
/* Functions                                                          */
/* ------------------------------------------------------------------ */

static Function *gen_function(Codegen *cg, ASTNode *func, const char *name) {
    Function *fn = (Function *)malloc(sizeof(Function));
    function_init(fn, name);
    array_push(cg->functions, &fn);

    /* Captured state: give each upvalue a fixed byte range so the body (and the
       creation site) can agree on where it lives. */
    Array *ups = func->as.func.upvalues;
    size_t nup = ups ? array_length(ups) : 0;
    if (nup) {
        fn->up_off = (size_t *)malloc(sizeof(size_t) * nup);
        fn->up_size = (size_t *)malloc(sizeof(size_t) * nup);
        if (fn->up_off && fn->up_size) {
            size_t o = 0;
            for (size_t i = 0; i < nup; i++) {
                Symbol *u = *(Symbol **)array_get(ups, i);
                fn->up_off[i] = o;
                fn->up_size[i] = type_size(u->type);
                o += fn->up_size[i];
            }
            fn->up_count = nup;
            fn->up_total = o;
        }
    }

    Function *saved_fn = cg->fn;
    Type *saved_ret = cg->ret_type;
    size_t saved_next = cg->next_off;
    size_t saved_frame = cg->frame_size;
    size_t saved_locals = array_length(cg->locals);
    size_t saved_base = cg->locals_base;
    Array *saved_caps = cg->caps;

    cg->fn = fn;
    cg->caps = ups;
    cg->locals_base = saved_locals;
    cg->ret_type = (func->type && func->type->kind == TYPE_FUNC) ? func->type->ret : NULL;
    cg->next_off = 0;
    cg->frame_size = 0;

    Array *params = func->as.func.params;
    int nparams = (int)array_length(params);
    fn->param_count = nparams;
    for (int i = 0; i < nparams; i++) {
        ASTNode *p = *(ASTNode **)array_get(params, i);
        size_t r = reg_alloc(cg, node_size(p));
        if (p->sym) local_set(cg, p->sym, r);
    }

    gen_stmt(cg, func->as.func.body);
    emit(cg, INSTR_ABC(OP_RETURN, 0, 0, 0));
    fn->frame_size = cg->frame_size;

    cg->fn = saved_fn;
    cg->ret_type = saved_ret;
    cg->caps = saved_caps;
    cg->locals_base = saved_base;
    cg->next_off = saved_next;
    cg->frame_size = saved_frame;
    while (array_length(cg->locals) > saved_locals) array_pop(cg->locals, NULL);

    return fn;
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static void gen_assign(Codegen *cg, Symbol *sym, ASTNode *value) {
    Type *st = sym ? sym->type : NULL;
    size_t size = type_size(st);
    if (sym && sym->is_global) {
        size_t r = reg_alloc(cg, size);
        gen_expr_as(cg, value, r, st);
        emit(cg, INSTR_ABXN(OP_SETGLOBAL, r, sym->slot, (int)size));
        reg_free(cg, size);
    } else if (sym) {
        size_t r = local_get(cg, sym);
        if (r == NO_REG) { r = reg_alloc(cg, size); local_set(cg, sym, r); }
        gen_expr_as(cg, value, r, st);
    } else {
        gen_expr(cg, value);
        reg_free(cg, node_size(value));
    }
}

/* The `::name = value` statics of a checked type body (`{}` literals emit
   theirs inline; see `AST_TABLE_LITERAL`). */
static void gen_type_statics(Codegen *cg, ASTNode *body) {
    if (!body || body->kind != AST_TABLE_LITERAL) return;
    Array *fields = body->as.table.fields;
    for (size_t i = 0; i < array_length(fields); i++) {
        ASTNode *field = *(ASTNode **)array_get(fields, i);
        if (field && field->kind == AST_MEMBER_ASSIGN && field->as.member_assign.is_static &&
            !(field->sym && field->sym->is_generic))
            gen_assign(cg, field->sym, field->as.member_assign.value);
        /* A generic method's instantiations own the globals their calls load. */
        ASTNode *fv = (field && field->kind == AST_ASSIGN) ? field->as.assign.value
                       : (field && field->kind == AST_BIND) ? field->as.bind.value
                       : (field && field->kind == AST_MEMBER_ASSIGN)
                             ? field->as.member_assign.value
                             : NULL;
        if (fv && fv->kind == AST_FUNC_LITERAL && fv->as.func.insts) {
            for (size_t k = 0; k < array_length(fv->as.func.insts); k++) {
                ASTNode *clone = *(ASTNode **)array_get(fv->as.func.insts, k);
                if (clone && clone->sym && clone->sym->is_global)
                    gen_assign(cg, clone->sym, clone);
            }
        }
    }
}

/* A generic function's declaration: one function value per instantiation, each
   stored in the instantiation's own global. */
static void gen_func_insts(Codegen *cg, ASTNode *decl) {
    if (!decl || !decl->as.func.insts) return;
    for (size_t i = 0; i < array_length(decl->as.func.insts); i++) {
        ASTNode *clone = *(ASTNode **)array_get(decl->as.func.insts, i);
        if (clone && clone->sym && clone->sym->is_global)
            gen_assign(cg, clone->sym, clone);
    }
}

static void gen_type_def(Codegen *cg, ASTNode *node) {
    Type *t = node->type;
    if (t && t->is_template) {
        /* A template's own body is unchecked (it has no members); each of its
           instances has a checked clone, whose statics — the constructor among
           them — are emitted here, once. */
        if (!t->instances) return;
        for (size_t i = 0; i < array_length(t->instances); i++) {
            Type *inst = *(Type **)array_get(t->instances, i);
            if (inst && inst->inst_body) gen_type_statics(cg, inst->inst_body);
        }
        return;
    }
    gen_type_statics(cg, node->as.type_def.body);
}

/* A non-constant enum item (`A = uint(1)`) is a static: its initializer is
   emitted in declaration order, like a `type { ... }` body's statics.
   Constant items are compile-time values and have no storage. */
static void gen_enum_def(Codegen *cg, ASTNode *node) {
    Type *et = node->type;
    if (!et || !et->members) return;
    Array *items = node->as.enum_def.items;
    for (size_t i = 0; i < array_length(items); i++) {
        EnumItem *item = (EnumItem *)array_get(items, i);
        if (!item || !item->name) continue;
        Symbol *sym = scope_lookup_local(et->members, item->name);
        if (sym && sym->init && !sym->has_const) gen_assign(cg, sym, sym->init);
    }
}

static void gen_stmt(Codegen *cg, ASTNode *node) {
    if (!node) return;

    switch (node->kind) {
        case AST_ASSIGN: {
            ASTNode *target = node->as.assign.target;
            if (target && target->kind == AST_INDEX) {
                Type *tt = target->as.index.target ? target->as.index.target->type : NULL;
                if (tt && tt->kind == TYPE_PACKED && tt->count > 0) {
                    Type *et = tt->base;
                    size_t size = type_size(et);
                    size_t val = reg_alloc(cg, size);
                    gen_expr_as(cg, node->as.assign.value, val, et);
                    size_t off = (size_t)target->as.index.const_index * size;
                    gen_packed_sub_store(cg, target->as.index.target, off, val, size);
                    reg_free(cg, size);
                    break;
                }
                if (tt && tt->kind == TYPE_ARRAY) {
                    Type *et = tt->base;
                    size_t size = type_size(et);
                    size_t saved = cg->next_off;
                    size_t arr = reg_alloc(cg, VSLOT);
                    gen_expr_to(cg, target->as.index.target, arr);
                    size_t idx = reg_alloc(cg, type_size(cg->sema->types.int_type));
                    gen_expr_as(cg, target->as.index.index, idx, cg->sema->types.int_type);
                    size_t val = reg_alloc(cg, size);
                    gen_expr_as(cg, node->as.assign.value, val, et);
                    emit(cg, INSTR_ABC(OP_SETINDEX, arr, idx, val));
                    cg->next_off = saved;
                    break;
                }
                /* Not an indexable target: evaluate and discard. */
                gen_expr(cg, node->as.assign.value);
                reg_free(cg, node_size(node->as.assign.value));
                break;
            }
            if (node->as.assign.value && node->as.assign.value->kind == AST_FUNC_LITERAL &&
                node->as.assign.value->as.func.type_params) {
                /* A generic function: one function value per instantiation. */
                gen_func_insts(cg, node->as.assign.value);
                break;
            }
            gen_assign(cg, node->sym, node->as.assign.value);
            break;
        }

        case AST_BIND:
            gen_assign(cg, node->sym, node->as.bind.value);
            break;

        case AST_MEMBER_ASSIGN: {
            Symbol *sym = node->sym;
            if (sym && sym->is_global) {
                gen_assign(cg, sym, node->as.member_assign.value);
                break;
            }
            ASTNode *target = node->as.member_assign.target;
            Type *owner = target ? target->type : NULL;
            if (owner && owner->kind == TYPE_PACKED && owner->count > 0 && sym) {
                Type *mt = sym->type;
                size_t size = type_size(mt);
                size_t val = reg_alloc(cg, size);
                gen_expr_as(cg, node->as.member_assign.value, val, mt);
                size_t off = (size_t)sym->slot * type_size(owner->base);
                gen_packed_sub_store(cg, target, off, val, size);
                reg_free(cg, size);
                break;
            }
            Type *ft = sym ? sym->type : NULL;
            size_t obj = reg_alloc(cg, VSLOT);
            gen_expr_to(cg, target, obj);
            size_t val = reg_alloc(cg, type_size(ft));
            gen_expr_as(cg, node->as.member_assign.value, val, ft);
            emit(cg, INSTR_ABCN(OP_SETFIELD, obj, sym ? sym->slot : 0, val,
                                (int)type_size(ft)));
            reg_free(cg, VSLOT + type_size(ft));
            break;
        }

        case AST_RETURN: {
            ASTNode *value = node->as.ret.value;
            if (value) {
                Type *rt = cg->ret_type;
                size_t size = type_size(rt);
                size_t r = reg_alloc(cg, size);
                gen_expr_as(cg, value, r, rt);
                emit(cg, INSTR_ABC(OP_RETURN, r, (int)size, 0));
                reg_free(cg, size);
            } else {
                emit(cg, INSTR_ABC(OP_RETURN, 0, 0, 0));
            }
            break;
        }

        case AST_IF: {
            ASTNode *c = node->as.if_stmt.cond;
            size_t cond = reg_alloc(cg, node_size(c));
            gen_expr_to(cg, c, cond);
            emit(cg, INSTR_ABCN(OP_TEST, cond, 0, 0, kind_of(c->type)));
            int j_else = emit_jump(cg, OP_JMP, 0);
            reg_free(cg, node_size(c));
            gen_stmt(cg, node->as.if_stmt.then_branch);
            if (node->as.if_stmt.else_branch) {
                int j_end = emit_jump(cg, OP_JMP, 0);
                patch_jump(cg, j_else);
                gen_stmt(cg, node->as.if_stmt.else_branch);
                patch_jump(cg, j_end);
            } else {
                patch_jump(cg, j_else);
            }
            break;
        }

        case AST_WHILE: {
            int start = (int)cg->fn->code_len;
            ASTNode *c = node->as.while_stmt.cond;
            size_t cond = reg_alloc(cg, node_size(c));
            gen_expr_to(cg, c, cond);
            emit(cg, INSTR_ABCN(OP_TEST, cond, 0, 0, kind_of(c->type)));
            int j_end = emit_jump(cg, OP_JMP, 0);
            reg_free(cg, node_size(c));

            LoopEntry entry;
            entry.start = start;
            entry.forward = false;
            entry.breaks = array_new(sizeof(int));
            entry.continues = NULL;
            array_push(cg->loops, &entry);

            gen_stmt(cg, node->as.while_stmt.body);

            int j_back = emit_jump(cg, OP_JMP, 0);
            cg->fn->code[j_back] = INSTR_ABX(OP_JMP, 0, (uint32_t)(start - j_back - 1));

            /* `break` leaves the loop: patch those jumps past the back-edge. */
            LoopEntry *cur = (LoopEntry *)array_back(cg->loops);
            for (size_t i = 0; i < array_length(cur->breaks); i++)
                patch_jump(cg, *(int *)array_get(cur->breaks, i));
            array_pop(cg->loops, NULL);

            patch_jump(cg, j_end);
            break;
        }

        case AST_FOR: {
            gen_stmt(cg, node->as.for_stmt.init);

            int cond_pos = (int)cg->fn->code_len;
            ASTNode *c = node->as.for_stmt.cond;
            size_t cond = reg_alloc(cg, node_size(c));
            gen_expr_to(cg, c, cond);
            emit(cg, INSTR_ABCN(OP_TEST, cond, 0, 0, kind_of(c->type)));
            int j_end = emit_jump(cg, OP_JMP, 0);
            reg_free(cg, node_size(c));

            LoopEntry entry;
            entry.start = cond_pos;
            entry.forward = true;
            entry.breaks = array_new(sizeof(int));
            entry.continues = array_new(sizeof(int));
            array_push(cg->loops, &entry);

            gen_stmt(cg, node->as.for_stmt.body);

            /* `continue` targets the step, which starts here. */
            int step_pos = (int)cg->fn->code_len;
            gen_stmt(cg, node->as.for_stmt.step);

            int j_back = emit_jump(cg, OP_JMP, 0);
            cg->fn->code[j_back] = INSTR_ABX(OP_JMP, 0, (uint32_t)(cond_pos - j_back - 1));

            LoopEntry *cur = (LoopEntry *)array_back(cg->loops);
            for (size_t i = 0; i < array_length(cur->continues); i++) {
                int at = *(int *)array_get(cur->continues, i);
                cg->fn->code[at] = INSTR_ABX(OP_JMP, 0, (uint32_t)(step_pos - at - 1));
            }
            for (size_t i = 0; i < array_length(cur->breaks); i++)
                patch_jump(cg, *(int *)array_get(cur->breaks, i));
            array_pop(cg->loops, NULL);

            patch_jump(cg, j_end);
            break;
        }

        case AST_BREAK: {
            LoopEntry *cur = array_length(cg->loops)
                                 ? (LoopEntry *)array_back(cg->loops) : NULL;
            if (!cur) break;   /* sema reported it */
            int at = emit_jump(cg, OP_JMP, 0);
            array_push(cur->breaks, &at);
            break;
        }

        case AST_SWITCH: {
            ASTNode *expr = node->as.switch_stmt.expr;
            Array *cases = node->as.switch_stmt.cases;
            Array *ends = array_new(sizeof(int));
            Type *it = cg->sema->types.int_type;

            /* One type for the whole switch: the scrutinee's own type for
               tagged values, the widest for packed ones. The scrutinee is
               evaluated once. */
            Type *ct = expr ? expr->type : NULL;
            for (size_t i = 0; i < array_length(cases); i++) {
                ASTNode *c = *(ASTNode **)array_get(cases, i);
                ASTNode *v = c->as.case_stmt.value;
                ct = common_type(ct, v ? v->type : NULL);
                if (c->as.case_stmt.range_hi)
                    ct = common_type(ct, c->as.case_stmt.range_hi->type);
            }
            size_t csize = type_size(ct);
            size_t sv = reg_alloc(cg, csize);
            gen_expr_as(cg, expr, sv, ct);
            int kind = kind_of(ct);
            bool packed = ct && ct->kind == TYPE_PACKED && ct->count > 0;

            for (size_t i = 0; i < array_length(cases); i++) {
                ASTNode *c = *(ASTNode **)array_get(cases, i);
                ASTNode *v = c->as.case_stmt.value;
                /* Every failing check jumps past the body, to the next case. */
                Array *misses = array_new(sizeof(int));
                ASTNode *hi = c->as.case_stmt.range_hi;
                if (hi) {
                    /* Half-open `[lo, hi)`: match on `v >= lo` and `v < hi`. */
                    size_t lo = reg_alloc(cg, csize);
                    gen_expr_as(cg, v, lo, ct);
                    size_t lower_ok = reg_alloc(cg, type_size(it));
                    emit(cg, INSTR_ABCN(OP_GE, lower_ok, sv, lo, kind));
                    jump_unless(cg, lower_ok, KIND_INT32, misses);
                    reg_free(cg, csize + type_size(it));

                    size_t ub = reg_alloc(cg, csize);
                    gen_expr_as(cg, hi, ub, ct);
                    size_t upper_ok = reg_alloc(cg, type_size(it));
                    emit(cg, INSTR_ABCN(OP_LT, upper_ok, sv, ub, kind));
                    jump_unless(cg, upper_ok, KIND_INT32, misses);
                    reg_free(cg, csize + type_size(it));
                } else {
                    size_t cv = reg_alloc(cg, csize);
                    gen_expr_as(cg, v, cv, ct);
                    size_t eq = reg_alloc(cg, type_size(it));
                    if (packed)
                        emit(cg, INSTR_ABCN(OP_EQPACKED, eq, sv, cv, (int)csize));
                    else
                        emit(cg, INSTR_ABCN(OP_EQ, eq, sv, cv, kind));
                    jump_unless(cg, eq, KIND_INT32, misses);
                    /* Free only what this case allocated: the scrutinee's slot
                       is still in use. */
                    reg_free(cg, csize + type_size(it));
                }

                /* A guard is one more condition the case must satisfy. */
                ASTNode *g = c->as.case_stmt.guard;
                if (g) {
                    size_t cond = reg_alloc(cg, node_size(g));
                    gen_expr_to(cg, g, cond);
                    jump_unless(cg, cond, kind_of(g->type), misses);
                    reg_free(cg, node_size(g));
                }

                gen_stmt(cg, c->as.case_stmt.body);
                int j_end = emit_jump(cg, OP_JMP, 0);
                array_push(ends, &j_end);
                for (size_t k = 0; k < array_length(misses); k++)
                    patch_jump(cg, *(int *)array_get(misses, k));
            }
            gen_stmt(cg, node->as.switch_stmt.default_body);
            for (size_t i = 0; i < array_length(ends); i++)
                patch_jump(cg, *(int *)array_get(ends, i));
            reg_free(cg, csize);
            break;
        }

        case AST_CONTINUE: {
            LoopEntry *cur = array_length(cg->loops)
                                 ? (LoopEntry *)array_back(cg->loops) : NULL;
            if (!cur) break;   /* sema reported it */
            if (cur->forward) {
                /* `for`: the step has not been emitted yet, so patch at the end. */
                int at = emit_jump(cg, OP_JMP, 0);
                array_push(cur->continues, &at);
            } else {
                /* `while`: jump back to the condition; the target is behind us,
                   so the offset is known now. */
                int j = emit_jump(cg, OP_JMP, 0);
                cg->fn->code[j] = INSTR_ABX(OP_JMP, 0, (uint32_t)(cur->start - j - 1));
            }
            break;
        }

        case AST_TYPE_DEF:
            gen_type_def(cg, node);
            break;

        case AST_BLOCK: {
            Array *stmts = node->as.block.stmts;
            for (size_t i = 0; i < array_length(stmts); i++)
                gen_stmt(cg, *(ASTNode **)array_get(stmts, i));
            break;
        }

        case AST_USE:
        case AST_EXTERN_DEF:
        case AST_PACKED_DEF:
        case AST_TYPE_ALIAS:
            break;

        case AST_ENUM_DEF:
            gen_enum_def(cg, node);
            break;

        default:
            gen_expr(cg, node);
            reg_free(cg, node_size(node));
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                              */
/* ------------------------------------------------------------------ */

CompiledProgram *codegen_run(Sema *sema, Arena *arena) {
    /* Same reason as `sema_init`: the struct is on the stack, so define every
       field before use rather than relying on what happens to be there. */
    Codegen cg;
    memset(&cg, 0, sizeof cg);
    cg.sema = sema;
    cg.arena = arena;
    cg.diag = sema->diag;
    cg.functions = array_new(sizeof(Function *));
    cg.locals = array_new(sizeof(Local));
    cg.locals_base = 0;
    cg.loops = array_new(sizeof(LoopEntry));
    cg.caps = NULL;

    Function *main = (Function *)malloc(sizeof(Function));
    function_init(main, "main");
    array_push(cg.functions, &main);
    cg.fn = main;
    cg.ret_type = NULL;
    cg.next_off = 0;
    cg.frame_size = 0;

    for (size_t i = 0; i < array_length(sema->programs); i++) {
        ProgramUnit *u = *(ProgramUnit **)array_get(sema->programs, i);
        Array *stmts = u->program->as.program.stmts;
        for (size_t j = 0; j < array_length(stmts); j++)
            gen_stmt(&cg, *(ASTNode **)array_get(stmts, j));
    }

    emit(&cg, INSTR_ABC(OP_HALT, 0, 0, 0));
    main->frame_size = cg.frame_size;

    CompiledProgram *p = (CompiledProgram *)malloc(sizeof(CompiledProgram));
    p->main = main;
    p->global_size = sema->global_size;
    p->functions = cg.functions;
    return p;
}

void compiled_program_free(CompiledProgram *p) {
    if (!p) return;
    for (size_t i = 0; i < array_length(p->functions); i++) {
        Function *fn = *(Function **)array_get(p->functions, i);
        function_free(fn);
        free(fn);
    }
    array_destroy(p->functions);
    free(p);
}
