#include <string.h>

#include "compiler/ast.h"

ASTNode *ast_node_new(Arena *arena, NodeType kind, Token token) {
    ASTNode *node = (ASTNode *)arena_alloc(arena, sizeof(ASTNode));
    if (!node) return NULL;
    /* The arena does not zero, so clear the whole node: every field the
       producer does not set must start defined (notably `as`). */
    memset(node, 0, sizeof(ASTNode));
    node->kind = kind;
    node->token = token;
    return node;
}

/* ------------------------------------------------------------------ */
/* Cloning: how a generic type body is instantiated (`plans/done/generics.md`) */
/* ------------------------------------------------------------------ */

/* A deep copy. Names and string literals are shared (both live in the arena
   for the whole compile), while every child node and list is copied. Sema's
   annotations are deliberately NOT carried over — `type`/`sym`, a path's
   `syms`, a literal's `upvalues` and `func_id` start clear — so the copy can
   be checked on its own. */

static Array *clone_node_array(Arena *arena, Array *src);
static Array *clone_names(Arena *arena, Array *src);

static Array *clone_path_parts(Arena *arena, Array *src) {
    Array *dst = array_new(sizeof(PathSegment));
    if (!src || !dst) return dst;
    for (size_t i = 0; i < array_length(src); i++) {
        PathSegment *seg = (PathSegment *)array_get(src, i);
        PathSegment copy;
        copy.name = seg->name;
        copy.is_static = seg->is_static;
        copy.type_args = seg->type_args ? clone_node_array(arena, seg->type_args) : NULL;
        array_push(dst, &copy);
    }
    return dst;
}

static Array *clone_enum_items(Arena *arena, Array *src) {
    Array *dst = array_new(sizeof(EnumItem));
    if (!src || !dst) return dst;
    for (size_t i = 0; i < array_length(src); i++) {
        EnumItem *item = (EnumItem *)array_get(src, i);
        EnumItem copy = *item;
        copy.value = item->value ? ast_clone(arena, item->value) : NULL;
        array_push(dst, &copy);
    }
    return dst;
}

static Array *clone_names(Arena *arena, Array *src) {
    (void)arena;   /* the names themselves are shared, not copied */
    Array *dst = array_new(sizeof(char *));
    if (!src || !dst) return dst;
    for (size_t i = 0; i < array_length(src); i++) {
        char *name = *(char **)array_get(src, i);
        array_push(dst, &name);
    }
    return dst;
}

static Array *clone_node_array(Arena *arena, Array *src) {
    Array *dst = array_new(sizeof(ASTNode *));
    if (!src || !dst) return dst;
    for (size_t i = 0; i < array_length(src); i++) {
        ASTNode *child = *(ASTNode **)array_get(src, i);
        ASTNode *copy = ast_clone(arena, child);
        array_push(dst, &copy);
    }
    return dst;
}

ASTNode *ast_clone(Arena *arena, ASTNode *node) {
    if (!node) return NULL;
    ASTNode *c = ast_node_new(arena, node->kind, node->token);
    if (!c) return NULL;

    switch (node->kind) {
        case AST_PROGRAM:
            c->as.program.stmts = clone_node_array(arena, node->as.program.stmts);
            break;

        case AST_BLOCK:
            c->as.block.stmts = clone_node_array(arena, node->as.block.stmts);
            break;

        case AST_PACKED_DEF:
            c->as.packed_def.target = ast_clone(arena, node->as.packed_def.target);
            c->as.packed_def.element = ast_clone(arena, node->as.packed_def.element);
            c->as.packed_def.count = node->as.packed_def.count;
            c->as.packed_def.body = ast_clone(arena, node->as.packed_def.body);
            break;

        case AST_TYPE_DEF:
            c->as.type_def.target = ast_clone(arena, node->as.type_def.target);
            c->as.type_def.body = ast_clone(arena, node->as.type_def.body);
            c->as.type_def.type_params =
                node->as.type_def.type_params
                    ? clone_names(arena, node->as.type_def.type_params)
                    : NULL;
            break;

        case AST_ENUM_DEF:
            c->as.enum_def.target = ast_clone(arena, node->as.enum_def.target);
            c->as.enum_def.items = clone_enum_items(arena, node->as.enum_def.items);
            break;

        case AST_ARRAY_TYPE:
            c->as.array_type.element = ast_clone(arena, node->as.array_type.element);
            c->as.array_type.length = node->as.array_type.length;
            c->as.array_type.has_length = node->as.array_type.has_length;
            break;

        case AST_FUNC_TYPE:
            c->as.func_type.params = clone_node_array(arena, node->as.func_type.params);
            c->as.func_type.ret = ast_clone(arena, node->as.func_type.ret);
            break;

        case AST_BIND:
            c->as.bind.type = ast_clone(arena, node->as.bind.type);
            c->as.bind.name = node->as.bind.name;
            c->as.bind.value = ast_clone(arena, node->as.bind.value);
            c->as.bind.is_local = node->as.bind.is_local;
            break;

        case AST_TYPE_ALIAS:
            c->as.type_alias.name = node->as.type_alias.name;
            c->as.type_alias.type = ast_clone(arena, node->as.type_alias.type);
            c->as.type_alias.type_params =
                node->as.type_alias.type_params
                    ? clone_names(arena, node->as.type_alias.type_params)
                    : NULL;
            break;

        case AST_ASSIGN:
            c->as.assign.target = ast_clone(arena, node->as.assign.target);
            c->as.assign.value = ast_clone(arena, node->as.assign.value);
            break;

        case AST_COMPOUND_ASSIGN:
            c->as.compound.target = ast_clone(arena, node->as.compound.target);
            c->as.compound.op = node->as.compound.op;
            c->as.compound.value = ast_clone(arena, node->as.compound.value);
            break;

        case AST_MEMBER_ASSIGN:
            c->as.member_assign.target = ast_clone(arena, node->as.member_assign.target);
            c->as.member_assign.member = node->as.member_assign.member;
            c->as.member_assign.value = ast_clone(arena, node->as.member_assign.value);
            c->as.member_assign.is_static = node->as.member_assign.is_static;
            break;

        case AST_MEMBER_ACCESS:
            c->as.member_access.object = ast_clone(arena, node->as.member_access.object);
            c->as.member_access.member = node->as.member_access.member;
            c->as.member_access.is_static = node->as.member_access.is_static;
            break;

        case AST_CALL:
            c->as.call.callee = ast_clone(arena, node->as.call.callee);
            c->as.call.args = clone_node_array(arena, node->as.call.args);
            c->as.call.builtin = node->as.call.builtin;
            break;

        case AST_INDEX:
            c->as.index.target = ast_clone(arena, node->as.index.target);
            c->as.index.index = ast_clone(arena, node->as.index.index);
            c->as.index.const_index = node->as.index.const_index;
            c->as.index.has_const_index = node->as.index.has_const_index;
            break;

        case AST_TABLE_LITERAL:
            c->as.table.fields = clone_node_array(arena, node->as.table.fields);
            break;

        case AST_ARRAY_LITERAL:
            c->as.array.elements = clone_node_array(arena, node->as.array.elements);
            c->as.array.repeat = ast_clone(arena, node->as.array.repeat);
            c->as.array.count_expr = ast_clone(arena, node->as.array.count_expr);
            c->as.array.count = node->as.array.count;
            c->as.array.count_const = node->as.array.count_const;
            break;

        case AST_INTERPOLATED_STRING:
            c->as.interpolated.parts = clone_node_array(arena, node->as.interpolated.parts);
            break;

        case AST_BINARY_EXPR:
            c->as.binary.left = ast_clone(arena, node->as.binary.left);
            c->as.binary.op = node->as.binary.op;
            c->as.binary.right = ast_clone(arena, node->as.binary.right);
            break;

        case AST_UNARY_EXPR:
            c->as.unary.op = node->as.unary.op;
            c->as.unary.operand = ast_clone(arena, node->as.unary.operand);
            break;

        case AST_LITERAL:
            c->as.literal.value = node->as.literal.value;
            c->as.literal.len = node->as.literal.len;
            break;

        case AST_PATH:
            c->as.path.parts = clone_path_parts(arena, node->as.path.parts);
            c->as.path.syms = NULL;
            c->as.path.is_global = node->as.path.is_global;
            break;

        case AST_RETURN:
            c->as.ret.value = ast_clone(arena, node->as.ret.value);
            break;

        case AST_IF:
            c->as.if_stmt.cond = ast_clone(arena, node->as.if_stmt.cond);
            c->as.if_stmt.then_branch = ast_clone(arena, node->as.if_stmt.then_branch);
            c->as.if_stmt.else_branch = ast_clone(arena, node->as.if_stmt.else_branch);
            break;

        case AST_WHILE:
            c->as.while_stmt.cond = ast_clone(arena, node->as.while_stmt.cond);
            c->as.while_stmt.body = ast_clone(arena, node->as.while_stmt.body);
            break;

        case AST_FOR:
            c->as.for_stmt.init = ast_clone(arena, node->as.for_stmt.init);
            c->as.for_stmt.cond = ast_clone(arena, node->as.for_stmt.cond);
            c->as.for_stmt.step = ast_clone(arena, node->as.for_stmt.step);
            c->as.for_stmt.body = ast_clone(arena, node->as.for_stmt.body);
            break;

        case AST_SWITCH:
            c->as.switch_stmt.expr = ast_clone(arena, node->as.switch_stmt.expr);
            c->as.switch_stmt.cases = clone_node_array(arena, node->as.switch_stmt.cases);
            c->as.switch_stmt.default_body =
                ast_clone(arena, node->as.switch_stmt.default_body);
            break;

        case AST_CASE:
            c->as.case_stmt.value = ast_clone(arena, node->as.case_stmt.value);
            c->as.case_stmt.range_hi = ast_clone(arena, node->as.case_stmt.range_hi);
            c->as.case_stmt.guard = ast_clone(arena, node->as.case_stmt.guard);
            c->as.case_stmt.body = ast_clone(arena, node->as.case_stmt.body);
            break;

        case AST_USE:
            c->as.use.path = node->as.use.path;
            break;

        case AST_FUNC_LITERAL:
            c->as.func.params = clone_node_array(arena, node->as.func.params);
            c->as.func.ret_type = ast_clone(arena, node->as.func.ret_type);
            c->as.func.body = ast_clone(arena, node->as.func.body);
            c->as.func.upvalues = NULL;
            c->as.func.func_id = 0;
            c->as.func.type_params =
                node->as.func.type_params
                    ? clone_names(arena, node->as.func.type_params)
                    : NULL;
            break;

        case AST_EXTERN_DEF:
            c->as.extern_def.name = node->as.extern_def.name;
            c->as.extern_def.params = clone_node_array(arena, node->as.extern_def.params);
            c->as.extern_def.ret_type = ast_clone(arena, node->as.extern_def.ret_type);
            c->as.extern_def.native_id = node->as.extern_def.native_id;
            break;

        case AST_PARAM:
            c->as.param.type = ast_clone(arena, node->as.param.type);
            c->as.param.name = node->as.param.name;
            break;

        /* Leaf statements carry nothing but their token. */
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_NULL:
            break;
    }
    return c;
}
