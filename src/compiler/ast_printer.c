#include <stdio.h>
#include <string.h>
#include "compiler/ast.h"

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void type_ref_to_buf(ASTNode *node, char *buf, size_t cap);

/* Append a segment's type-argument list (`Name<int, String>`) to a buffer. */
static void type_args_to_buf(Array *type_args, char *buf, size_t cap) {
    if (!type_args) return;
    strncat(buf, "<", cap - strlen(buf) - 1);
    for (size_t i = 0; i < array_length(type_args); i++) {
        char arg[256];
        type_ref_to_buf(*(ASTNode **)array_get(type_args, i), arg, sizeof(arg));
        if (i) strncat(buf, ", ", cap - strlen(buf) - 1);
        strncat(buf, arg, cap - strlen(buf) - 1);
    }
    strncat(buf, ">", cap - strlen(buf) - 1);
}

static void print_path(ASTNode *node) {
    ArrayIterator it;
    array_iter_init(&it, node->as.path.parts);
    PathSegment *seg;
    bool first = true;
    while ((seg = (PathSegment *)array_next(&it)) != NULL) {
        if (!first) printf(seg->is_static ? "::" : ".");
        printf("%s", seg->name);
        if (seg->type_args) {
            char args[256];
            args[0] = '\0';
            type_args_to_buf(seg->type_args, args, sizeof(args));
            printf("%s", args);
        }
        first = false;
    }
}

static void path_to_buf(ASTNode *node, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!node || node->kind != AST_PATH) return;
    if (node->as.path.is_global) strncat(buf, "$", cap - 1);
    ArrayIterator it;
    array_iter_init(&it, node->as.path.parts);
    PathSegment *seg;
    bool first = true;
    while ((seg = (PathSegment *)array_next(&it)) != NULL) {
        if (!first) {
            strncat(buf, seg->is_static ? "::" : ".", cap - strlen(buf) - 1);
        }
        strncat(buf, seg->name, cap - strlen(buf) - 1);
        type_args_to_buf(seg->type_args, buf, cap);
        first = false;
    }
}

/* The written form of a type reference, for `-printast` and member names. */
static void type_ref_to_buf(ASTNode *node, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!node) { strncat(buf, "?", cap - 1); return; }
    switch (node->kind) {
        case AST_PATH:
            path_to_buf(node, buf, cap);
            break;
        case AST_ARRAY_TYPE: {
            char inner[128];
            char tmp[192];
            type_ref_to_buf(node->as.array_type.element, inner, sizeof(inner));
            if (node->as.array_type.has_length)
                snprintf(tmp, sizeof(tmp), "%s[%zu]", inner, node->as.array_type.length);
            else
                snprintf(tmp, sizeof(tmp), "%s[]", inner);
            strncat(buf, tmp, cap - strlen(buf) - 1);
            break;
        }
        case AST_FUNC_TYPE: {
            char tmp[256];
            tmp[0] = '\0';
            strncat(tmp, "(", sizeof(tmp) - 1);
            for (size_t i = 0; i < array_length(node->as.func_type.params); i++) {
                char p[96];
                type_ref_to_buf(*(ASTNode **)array_get(node->as.func_type.params, i), p, sizeof(p));
                if (i) strncat(tmp, ", ", sizeof(tmp) - strlen(tmp) - 1);
                strncat(tmp, p, sizeof(tmp) - strlen(tmp) - 1);
            }
            strncat(tmp, ") -> ", sizeof(tmp) - strlen(tmp) - 1);
            char ret[96];
            type_ref_to_buf(node->as.func_type.ret, ret, sizeof(ret));
            strncat(tmp, ret, sizeof(tmp) - strlen(tmp) - 1);
            strncat(buf, tmp, cap - strlen(buf) - 1);
            break;
        }
        default:
            strncat(buf, "?", cap - 1);
            break;
    }
}

static void print_type(Type *t) {
    if (!t) { printf("?"); return; }
    switch (t->kind) {
        case TYPE_UNKNOWN: printf("?"); break;
        case TYPE_VOID:    printf("void"); break;
        case TYPE_NULL:    printf("null"); break;
        case TYPE_PACKED:  printf("%s", t->name); break;
        case TYPE_OBJ:     printf("%s", t->name); break;
        case TYPE_STRING:  printf("%s", t->name); break;
        case TYPE_ENUM:    printf("%s", t->name); break;
        case TYPE_ARRAY:
            print_type(t->base);
            if (t->size) printf("[%zu]", t->size);
            else printf("[]");
            break;
        case TYPE_FUNC:
            printf("(");
            for (size_t i = 0; i < t->param_count; i++) {
                if (i) printf(", ");
                print_type(t->params[i]);
            }
            printf(") -> ");
            print_type(t->ret);
            break;
    }
}

static void print_type_line(ASTNode *node, int depth) {
    print_indent(depth);
    printf("type = ");
    print_type(node->type);
    printf("\n");
}

/* `prefix` is the enclosing type path (e.g. "test.") used to render member
   names as full paths. It is reset inside function bodies and blocks. */
static void print_ast_p(ASTNode *node, int depth, const char *prefix) {
    if (!node) return;

    print_indent(depth);
    switch (node->kind) {
        case AST_PROGRAM:
            printf("Program\n");
            if (node->as.program.stmts) {
                ArrayIterator it;
                array_iter_init(&it, node->as.program.stmts);
                ASTNode **stmt;
                while ((stmt = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*stmt, depth + 1, prefix);
                }
            }
            return; /* no type line for the program root */

        case AST_PACKED_DEF: {
            char buf[256];
            path_to_buf(node->as.packed_def.target, buf, sizeof(buf));
            const char *base = (node->type && node->type->base) ? node->type->base->name : "?";
            printf("PackedDef '%s' = packed %s[%zu]\n", buf, base, node->as.packed_def.count);
            print_type_line(node, depth + 1);
            ASTNode *body = node->as.packed_def.body;
            if (body && body->kind == AST_TABLE_LITERAL) {
                Array *fields = body->as.table.fields;
                for (size_t i = 0; i < array_length(fields); i++) {
                    ASTNode *f = *(ASTNode **)array_get(fields, i);
                    if (!f || f->kind != AST_ASSIGN) continue;
                    char mname[256];
                    path_to_buf(f->as.assign.target, mname, sizeof(mname));
                    ASTNode *v = f->as.assign.value;
                    print_indent(depth + 1);
                    if (v && v->kind == AST_INDEX && v->as.index.has_const_index)
                        printf("%s = %s[%ld]\n", mname, buf, v->as.index.const_index);
                    else
                        printf("%s = ?\n", mname);
                }
            }
            return;
        }

        case AST_TYPE_DEF: {
            char buf[256];
            path_to_buf(node->as.type_def.target, buf, sizeof(buf));
            char label[384];
            Array *tps = node->as.type_def.type_params;
            if (tps) {
                char ps[64];
                ps[0] = '\0';
                for (size_t i = 0; i < array_length(tps); i++) {
                    if (i) strncat(ps, ", ", sizeof(ps) - strlen(ps) - 1);
                    strncat(ps, *(char **)array_get(tps, i), sizeof(ps) - strlen(ps) - 1);
                }
                snprintf(label, sizeof(label), "%s<%s>", buf, ps);
            } else {
                snprintf(label, sizeof(label), "%s", buf);
            }
            printf("TypeDef '%s' =\n", label);
            print_type_line(node, depth + 1);
            char child[300];
            snprintf(child, sizeof(child), "%s.", buf);
            print_ast_p(node->as.type_def.body, depth + 1, child);
            return;
        }

        case AST_ENUM_DEF: {
            char buf[256];
            path_to_buf(node->as.enum_def.target, buf, sizeof(buf));
            printf("EnumDef '%s' =\n", buf);
            print_type_line(node, depth + 1);
            Array *items = node->as.enum_def.items;
            for (size_t i = 0; i < array_length(items); i++) {
                EnumItem *item = (EnumItem *)array_get(items, i);
                print_indent(depth + 1);
                if (item->value) {
                    printf("%s =\n", item->name);
                    print_ast_p(item->value, depth + 2, "");
                } else {
                    printf("%s = %ld\n", item->name, item->auto_value);
                }
            }
            return;
        }

        case AST_ARRAY_TYPE:
            printf("ArrayType\n");
            print_type_line(node, depth + 1);
            print_ast_p(node->as.array_type.element, depth + 1, "");
            return;

        case AST_NULL:
            printf("Null\n");
            print_type_line(node, depth + 1);
            return;

        case AST_FUNC_TYPE: {
            printf("FuncType\n");
            print_type_line(node, depth + 1);
            Array *fparams = node->as.func_type.params;
            for (size_t i = 0; i < array_length(fparams); i++) {
                print_indent(depth + 1);
                printf("Param:\n");
                print_ast_p(*(ASTNode **)array_get(fparams, i), depth + 2, "");
            }
            if (node->as.func_type.ret) {
                print_indent(depth + 1);
                printf("Ret:\n");
                print_ast_p(node->as.func_type.ret, depth + 2, "");
            }
            return;
        }

        case AST_BIND:
            printf("Bind '%s%s'\n", prefix, node->as.bind.name);
            print_type_line(node, depth + 1);
            print_ast_p(node->as.bind.type, depth + 1, prefix);
            print_ast_p(node->as.bind.value, depth + 1, prefix);
            return;

        case AST_TYPE_ALIAS: {
            char label[256];
            Array *tps = node->as.type_alias.type_params;
            if (tps && array_length(tps) > 0) {
                char ps[64];
                ps[0] = '\0';
                for (size_t i = 0; i < array_length(tps); i++) {
                    if (i) strncat(ps, ", ", sizeof(ps) - strlen(ps) - 1);
                    strncat(ps, *(char **)array_get(tps, i), sizeof(ps) - strlen(ps) - 1);
                }
                snprintf(label, sizeof(label), "%s<%s>", node->as.type_alias.name, ps);
            } else {
                snprintf(label, sizeof(label), "%s", node->as.type_alias.name);
            }
            printf("TypeAlias '%s%s'\n", prefix, label);
            print_type_line(node, depth + 1);
            print_ast_p(node->as.type_alias.type, depth + 1, "");
            return;
        }

        case AST_ASSIGN: {
            char buf[256];
            path_to_buf(node->as.assign.target, buf, sizeof(buf));
            if (prefix[0]) printf("Assign %s%s =\n", prefix, buf);
            else printf("Define '%s' =\n", buf);
            print_type_line(node, depth + 1);
            ASTNode *value = node->as.assign.value;
            if (value && value->kind == AST_TABLE_LITERAL) {
                char child[300];
                snprintf(child, sizeof(child), "%s%s.", prefix, buf);
                print_ast_p(value, depth + 1, child);
            } else {
                print_ast_p(value, depth + 1, prefix);
            }
            return;
        }

        case AST_MEMBER_ASSIGN: {
            char buf[256];
            path_to_buf(node->as.member_assign.target, buf, sizeof(buf));
            const char *sep = node->as.member_assign.is_static ? "::" : ".";
            const char *member = node->as.member_assign.member;
            if (buf[0]) {
                printf("Assign %s%s%s%s =\n", prefix, buf, sep, member);
            } else {
                /* Static member of the enclosing type: `test.` -> `test::new`. */
                size_t plen = strlen(prefix);
                if (plen > 0 && prefix[plen - 1] == '.') {
                    printf("Assign %.*s%s%s =\n", (int)(plen - 1), prefix, sep, member);
                } else {
                    printf("Assign %s%s%s =\n", prefix, sep, member);
                }
            }
            print_type_line(node, depth + 1);
            print_ast_p(node->as.member_assign.value, depth + 1, prefix);
            return;
        }

        case AST_MEMBER_ACCESS:
            printf("MemberAccess %s%s\n",
                   node->as.member_access.is_static ? "::" : ".",
                   node->as.member_access.member);
            print_type_line(node, depth + 1);
            print_ast_p(node->as.member_access.object, depth + 1, prefix);
            return;

        case AST_CALL:
            printf("Call\n");
            print_type_line(node, depth + 1);
            print_ast_p(node->as.call.callee, depth + 1, prefix);
            if (node->as.call.args) {
                ArrayIterator it;
                array_iter_init(&it, node->as.call.args);
                ASTNode **arg;
                while ((arg = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*arg, depth + 1, prefix);
                }
            }
            return;

        case AST_INDEX:
            printf("Index\n");
            print_type_line(node, depth + 1);
            print_ast_p(node->as.index.target, depth + 1, prefix);
            print_ast_p(node->as.index.index, depth + 1, prefix);
            return;

        case AST_TABLE_LITERAL:
            printf("TableLiteral\n");
            print_type_line(node, depth + 1);
            if (node->as.table.fields) {
                ArrayIterator it;
                array_iter_init(&it, node->as.table.fields);
                ASTNode **field;
                while ((field = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*field, depth + 1, prefix);
                }
            }
            return;

        case AST_ARRAY_LITERAL:
            printf("ArrayLiteral\n");
            print_type_line(node, depth + 1);
            if (node->as.array.elements) {
                ArrayIterator it;
                array_iter_init(&it, node->as.array.elements);
                ASTNode **elem;
                while ((elem = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*elem, depth + 1, prefix);
                }
            }
            return;

        case AST_BINARY_EXPR:
            printf("BinaryExpr: %.*s\n", (int)node->as.binary.op.length, node->as.binary.op.start);
            print_type_line(node, depth + 1);
            print_ast_p(node->as.binary.left, depth + 1, prefix);
            print_ast_p(node->as.binary.right, depth + 1, prefix);
            return;

        case AST_UNARY_EXPR:
            printf("UnaryExpr: %.*s\n", (int)node->as.unary.op.length, node->as.unary.op.start);
            print_type_line(node, depth + 1);
            print_ast_p(node->as.unary.operand, depth + 1, prefix);
            return;

        case AST_LITERAL:
            if (node->token.kind == TOKEN_STRING)
                printf("Literal: \"%.*s\"\n", (int)node->as.literal.len, node->as.literal.value);
            else
                printf("Literal: %.*s\n", (int)node->as.literal.len, node->as.literal.value);
            break;

        case AST_INTERPOLATED_STRING:
            printf("InterpolatedString\n");
            break;

        case AST_PATH:
            printf("Path: ");
            print_path(node);
            printf("\n");
            break;

        case AST_RETURN:
            printf("Return\n");
            print_type_line(node, depth + 1);
            print_ast_p(node->as.ret.value, depth + 1, prefix);
            return;

        case AST_USE:
            printf("Use: %s\n", node->as.use.path);
            return;

        case AST_IF:
            printf("If\n");
            print_type_line(node, depth + 1);
            print_indent(depth + 1);
            printf("Cond:\n");
            print_ast_p(node->as.if_stmt.cond, depth + 2, prefix);
            print_indent(depth + 1);
            printf("Then:\n");
            print_ast_p(node->as.if_stmt.then_branch, depth + 2, prefix);
            if (node->as.if_stmt.else_branch) {
                print_indent(depth + 1);
                printf("Else:\n");
                print_ast_p(node->as.if_stmt.else_branch, depth + 2, prefix);
            }
            return;

        case AST_WHILE:
            printf("While\n");
            print_type_line(node, depth + 1);
            print_indent(depth + 1);
            printf("Cond:\n");
            print_ast_p(node->as.while_stmt.cond, depth + 2, prefix);
            print_indent(depth + 1);
            printf("Body:\n");
            print_ast_p(node->as.while_stmt.body, depth + 2, prefix);
            return;

        case AST_FOR:
            printf("For\n");
            print_type_line(node, depth + 1);
            print_indent(depth + 1);
            printf("Init:\n");
            print_ast_p(node->as.for_stmt.init, depth + 2, prefix);
            print_indent(depth + 1);
            printf("Cond:\n");
            print_ast_p(node->as.for_stmt.cond, depth + 2, prefix);
            print_indent(depth + 1);
            printf("Step:\n");
            print_ast_p(node->as.for_stmt.step, depth + 2, prefix);
            print_indent(depth + 1);
            printf("Body:\n");
            print_ast_p(node->as.for_stmt.body, depth + 2, prefix);
            return;

        case AST_SWITCH: {
            printf("Switch\n");
            print_type_line(node, depth + 1);
            print_indent(depth + 1);
            printf("Value:\n");
            print_ast_p(node->as.switch_stmt.expr, depth + 2, prefix);
            Array *scases = node->as.switch_stmt.cases;
            for (size_t i = 0; i < array_length(scases); i++) {
                print_indent(depth + 1);
                printf("Case:\n");
                print_ast_p(*(ASTNode **)array_get(scases, i), depth + 2, prefix);
            }
            if (node->as.switch_stmt.default_body) {
                print_indent(depth + 1);
                printf("Default:\n");
                print_ast_p(node->as.switch_stmt.default_body, depth + 2, prefix);
            }
            return;
        }

        case AST_CASE:
            printf("Case\n");
            print_type_line(node, depth + 1);
            print_indent(depth + 1);
            printf("Value:\n");
            print_ast_p(node->as.case_stmt.value, depth + 2, prefix);
            if (node->as.case_stmt.range_hi) {
                print_indent(depth + 1);
                printf("RangeHi:\n");
                print_ast_p(node->as.case_stmt.range_hi, depth + 2, prefix);
            }
            if (node->as.case_stmt.guard) {
                print_indent(depth + 1);
                printf("Guard:\n");
                print_ast_p(node->as.case_stmt.guard, depth + 2, prefix);
            }
            print_indent(depth + 1);
            printf("Body:\n");
            print_ast_p(node->as.case_stmt.body, depth + 2, prefix);
            return;

        case AST_FUNC_LITERAL:
            printf("FuncLiteral\n");
            print_type_line(node, depth + 1);
            if (node->as.func.type_params) {
                print_indent(depth + 1);
                printf("TypeParams:");
                for (size_t i = 0; i < array_length(node->as.func.type_params); i++)
                    printf(" %s", *(char **)array_get(node->as.func.type_params, i));
                printf("\n");
            }
            if (node->as.func.params) {
                ArrayIterator it;
                array_iter_init(&it, node->as.func.params);
                ASTNode **param;
                while ((param = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*param, depth + 1, "");
                }
            }
            if (node->as.func.ret_type) {
                print_indent(depth + 1);
                printf("ReturnType:\n");
                print_ast_p(node->as.func.ret_type, depth + 2, "");
            }
            print_ast_p(node->as.func.body, depth + 1, "");
            return;

        case AST_EXTERN_DEF:
            printf("ExternDef: %s\n", node->as.extern_def.name);
            print_type_line(node, depth + 1);
            if (node->as.extern_def.params) {
                ArrayIterator it;
                array_iter_init(&it, node->as.extern_def.params);
                ASTNode **param;
                while ((param = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*param, depth + 1, "");
                }
            }
            if (node->as.extern_def.ret_type) {
                print_indent(depth + 1);
                printf("ReturnType:\n");
                print_ast_p(node->as.extern_def.ret_type, depth + 2, "");
            }
            return;

        case AST_PARAM:
            printf("Param");
            if (node->as.param.name) printf(" name=%s", node->as.param.name);
            printf("\n");
            print_type_line(node, depth + 1);
            if (node->as.param.type) print_ast_p(node->as.param.type, depth + 1, "");
            return;

        case AST_BLOCK:
            printf("Block\n");
            if (node->as.block.stmts) {
                ArrayIterator it;
                array_iter_init(&it, node->as.block.stmts);
                ASTNode **stmt;
                while ((stmt = (ASTNode **)array_next(&it)) != NULL) {
                    print_ast_p(*stmt, depth + 1, "");
                }
            }
            return;

        default:
            printf("Node kind: %d\n", node->kind);
            return;
    }

    /* Leaf nodes that fall through print their type line here. */
    print_type_line(node, depth + 1);
}

void print_ast(ASTNode *node, int depth) {
    print_ast_p(node, depth, "");
}
