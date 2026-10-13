#ifndef OBY_AST_H
#define OBY_AST_H

#include "compiler/lexer.h"
#include "compiler/type.h"
#include "core/array.h"
#include "core/arena.h"

/* One segment of a path. `is_static` is true for `::` (type/static access)
   and false for `.` (instance access). `type_args` is a `Name<...>` argument
   list (Array of ASTNode* type references), or NULL when absent; see
   `plans/done/generics.md`. */
typedef struct {
    char *name;
    bool is_static;
    Array *type_args;
    Token tok;      /* the identifier, for tooling (hover, definition) */
} PathSegment;

/* One constant of an enum definition. */
typedef struct {
    const char *name;
    struct ASTNode *value;  /* explicit value expression, or NULL */
    long auto_value;        /* resolved value when `value` is NULL */
    Token tok;              /* the constant's name token, for tooling */
} EnumItem;

typedef enum {
    AST_PROGRAM,
    AST_PACKED_DEF,
    AST_TYPE_DEF,
    AST_TYPE_ALIAS,
    AST_ENUM_DEF,
    AST_ARRAY_TYPE,
    AST_FUNC_TYPE,
    AST_BIND,
    AST_ASSIGN,
    AST_COMPOUND_ASSIGN,
    AST_MEMBER_ASSIGN,
    AST_MEMBER_ACCESS,
    AST_CALL,
    AST_INDEX,
    AST_TABLE_LITERAL,
    AST_ARRAY_LITERAL,
    AST_INTERPOLATED_STRING,
    AST_BINARY_EXPR,
    AST_UNARY_EXPR,
    AST_LITERAL,
    AST_PATH,
    AST_RETURN,
    AST_BREAK,
    AST_CONTINUE,
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_SWITCH,
    AST_CASE,
    AST_NULL,
    AST_USE,
    AST_FUNC_LITERAL,
    AST_EXTERN_DEF,
    AST_PARAM,
    AST_BLOCK
} NodeType;

/* Compiler intrinsics recognized from method-style syntax; they are not real
   function calls. */
typedef enum {
    BUILTIN_NONE = 0,
    BUILTIN_ARRAY_PUSH,
    BUILTIN_ARRAY_POP,
    BUILTIN_ARRAY_INSERT,
    BUILTIN_ARRAY_REMOVE,
} BuiltinId;

typedef struct ASTNode {
    NodeType kind;      /* node kind (was `type`) */
    Type *type;         /* resolved type; NULL until checked */
    Symbol *sym;        /* resolved symbol; NULL until checked */
    Token token;

    union {
        struct { struct ASTNode *left; Token op; struct ASTNode *right; } binary;
        struct { Token op; struct ASTNode *operand; } unary;
        struct { struct ASTNode *callee; Array *args; int builtin; } call;
        struct {
            struct ASTNode *target;
            struct ASTNode *index;
            long const_index;    /* packed element index, when constant */
            bool has_const_index;
        } index;
        struct { struct ASTNode *target; struct ASTNode *value; } assign;
        struct { struct ASTNode *target; Token op; struct ASTNode *value; } compound;
        struct {
            struct ASTNode *target;
            struct ASTNode *body;
            Array *type_params;   /* `type<T, K>` parameter names (char*), or NULL */
        } type_def;
        struct {
            struct ASTNode *target;   /* the type name */
            struct ASTNode *element;  /* element type reference */
            size_t count;             /* element count */
            struct ASTNode *body;     /* optional member aliases, or NULL */
        } packed_def;
        struct { struct ASTNode *target; Array *items; } enum_def;
        struct { struct ASTNode *element; size_t length; bool has_length; } array_type;
        struct { Array *params; struct ASTNode *ret; } func_type;
        struct { struct ASTNode *type; const char *name; struct ASTNode *value; bool is_local; } bind;
        /* `name = <function type>` (or `name<T> = ...` for a generic one) names
           a function type (`plans/done/function_type_aliases.md`). */
        struct { const char *name; struct ASTNode *type; Array *type_params; } type_alias;
        struct { struct ASTNode *target; const char *member; Token member_tok; struct ASTNode *value; bool is_static; } member_assign;
        struct { struct ASTNode *object; const char *member; Token member_tok; bool is_static; } member_access;
        struct { Array *fields; } table;
        struct {
            Array *elements;          /* `[a, b, c]` */
            struct ASTNode *repeat;   /* `[expr; n]`: the element, or NULL */
            struct ASTNode *count_expr;
            size_t count;             /* resolved repeat count */
            bool count_const;         /* 1 when `count` came from a constant */
        } array;
        struct { const char *value; size_t len; } literal;
        struct { Array *parts; Array *syms; bool is_global; } path; // parts: PathSegment*, syms: Symbol*
        struct { Array *parts; } interpolated;
        struct { Array *stmts; } program;
        struct { struct ASTNode *value; } ret;
        struct { struct ASTNode *cond; struct ASTNode *then_branch; struct ASTNode *else_branch; } if_stmt;
        struct { struct ASTNode *cond; struct ASTNode *body; } while_stmt;
        struct {
            struct ASTNode *expr;       /* the value being matched */
            Array *cases;               /* Array of AST_CASE */
            struct ASTNode *default_body;
        } switch_stmt;
        struct {
            struct ASTNode *value;      /* the case value, or a range's lower bound */
            struct ASTNode *range_hi;   /* a range's upper bound, or NULL */
            struct ASTNode *guard;      /* `if cond`, or NULL */
            struct ASTNode *body;
        } case_stmt;
        struct {
            struct ASTNode *init;   /* run once, before the loop */
            struct ASTNode *cond;   /* tested before each iteration */
            struct ASTNode *step;   /* run after each iteration */
            struct ASTNode *body;
        } for_stmt;
        struct { const char *path; } use;
        struct {
            Array *params;
            struct ASTNode *ret_type;
            struct ASTNode *body;
            Array *upvalues;   /* Symbol* captured from enclosing functions (sema) */
            int    func_id;    /* nesting id assigned while checking (sema) */
            /* Generic functions (`plans/done/generics.md` Phase 7): the declaration
               carries the parameter names, the declaration collects a clone
               per instantiation, and each clone carries its argument types. */
            Array *type_params;  /* char* names, on the declaration */
            Array *type_args;    /* Type* arguments, on an instantiation clone */
            Array *insts;        /* ASTNode* clones, on the declaration */
        } func;
        struct { const char *name; Array *params; struct ASTNode *ret_type; int native_id; } extern_def;
        struct { struct ASTNode *type; const char *name; } param;
        struct { Array *stmts; } block;
    } as;
} ASTNode;

ASTNode *ast_node_new(Arena *arena, NodeType type, Token token);

/* A deep copy of `node` (children included) in `arena`. Sema's annotations are
   not carried over — `type`/`sym`, a path's `syms`, a literal's `upvalues` and
   `func_id` start clear — so the copy can be checked independently. This is
   how a generic type body is instantiated (`plans/done/generics.md`). */
ASTNode *ast_clone(Arena *arena, ASTNode *node);

#endif // OBY_AST_H