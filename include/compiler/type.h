#ifndef OBY_TYPE_H
#define OBY_TYPE_H

#include <stddef.h>
#include <stdbool.h>
#include "core/arena.h"
#include "core/array.h"

typedef struct Type Type;
typedef struct Scope Scope;
typedef struct Symbol Symbol;
typedef struct ASTNode ASTNode;   /* compiler/ast.h */

/* The kind of a resolved type. */
typedef enum {
    TYPE_UNKNOWN,   /* not resolved yet (error if it survives checking) */
    TYPE_VOID,      /* no value, e.g. `() { return }` */
    TYPE_NULL,      /* the type of the `null` literal */
    TYPE_PACKED,    /* fixed-width stack value: byte, int, uint, ... */
    TYPE_OBJ,       /* heap object: object, `{}` types */
    TYPE_STRING,    /* built-in String primitive */
    TYPE_ARRAY,     /* T[n] fixed, T[] growable */
    TYPE_ENUM,      /* a named set of integer constants */
    TYPE_FUNC,      /* (params) -> ret */
} TypeKind;

/* What a symbol names. */
typedef enum {
    SYM_TYPE,       /* a type name */
    SYM_VALUE,      /* a value (slot / global) */
    SYM_FUNC,       /* a function */
    SYM_MEMBER,     /* a member of a type (static or instance) */
} SymbolKind;

struct Symbol {
    const char *name;
    Type *type;
    ASTNode *init;      /* instance member: the initializer from the type body,
                           applied when a `{}` literal omits the member */
    SymbolKind kind;
    bool is_static;     /* member accessed with `::` */
    int native_id;      /* for `extern` symbols: registry id, else -1 */
    int slot;           /* global slot / field index; -1 for locals */
    int func_id;        /* compiled-function index, else -1 */
    int def_func;       /* function nesting id that declared this local, -1 otherwise */
    bool is_global;     /* defined at global scope or a static member */
    bool is_generic;    /* a generic function declaration: a template with no storage */
    bool is_alias;      /* a function-type alias declaration (`plans/done/function_type_aliases.md`) */
    ASTNode *decl;      /* that declaration: a generic function literal, or the alias statement */
    bool has_const;     /* integer compile-time constant (enum item) */
    bool is_enum_item;  /* an item of an enum type (with or without a constant) */
    long long const_value;
    Symbol *next;       /* next overload with the same name */
};

/* A lexical scope: a name -> symbol mapping with a parent link. */
struct Scope {
    Scope *parent;
    Array *symbols;     /* Array of Symbol* */
};

struct Type {
    TypeKind kind;
    const char *name;
    size_t size;        /* packed: byte width; array: element count (0 = unknown);
                           obj: bytes of field storage (set by the layout pass) */
    size_t count;       /* packed aggregate: element count (0 = scalar) */
    bool is_float;      /* packed: floating-point (float, double) vs integer */
    bool laid_out;      /* obj: field offsets assigned */
    Type *base;         /* packed: underlying type; array: element type */
    Scope *members;     /* obj: static + instance members */
    size_t field_count; /* obj: number of instance member slots assigned */
    /* function */
    Type **params;
    size_t param_count;
    Type *ret;
    /* Generic types (`plans/done/generics.md`). A *template* is the `type<T>`
       recipe: it is never a value type and owns no members. An *instance* is a
       concrete nominal type created per argument list and interned, so one
       argument list is one type. */
    Array *type_params;   /* template: parameter names (char*) */
    Array *type_args;     /* instance: the argument types (Type*) */
    Array *instances;     /* template: instances made from it (Type*) */
    Type *origin;         /* instance: the template */
    ASTNode *inst_body;   /* template: the parsed body; instance: its checked clone */
    /* template: the source an instantiated body's diagnostics belong to (it
       may be another file — the prelude's generics, say). */
    const char *src_file;
    const char *src_source;
    bool is_template;
};

/* Owns the built-in types. User types are interned through the same arena. */
typedef struct {
    Arena *arena;
    Array *all;         /* every Type created, for the layout passes */
    Type *unknown;
    Type *void_type;
    Type *null_type;
    Type *byte;
    Type *object;
    Type *string;
    Type *float_type;
    Type *double_type;
    Type *int_type;
    Type *uint_type;
    Type *int64_type;
    Type *uint64_type;
} TypeContext;

void type_context_init(TypeContext *tc, Arena *arena);
Type *type_new(TypeContext *tc, TypeKind kind, const char *name);
Type *type_packed(TypeContext *tc, const char *name, Type *base, size_t size);
/* A packed aggregate: `count` elements of `element`, byte-tight. */
Type *type_packed_aggregate(TypeContext *tc, const char *name, Type *element, size_t count);
/* A packed floating-point type (`float`, `double`). */
Type *type_float(TypeContext *tc, const char *name, size_t size);
Type *type_obj(TypeContext *tc, const char *name);
/* The built-in String primitive. */
Type *type_string(TypeContext *tc, const char *name);
/* `length` 0 means an unknown-length (growable) array. Array types are
   interned: one shape is one `Type*`. */
Type *type_array(TypeContext *tc, Type *element, size_t length);
/* `underlying` is the integer type the constants are stored as. */
Type *type_enum(TypeContext *tc, const char *name, Type *underlying);
/* Function types are interned too (same parameter and return types). */
Type *type_func(TypeContext *tc, Type **params, size_t count, Type *ret);

/* A generic template: `type<T, ...>`, with no members until instantiated. */
Type *type_template(TypeContext *tc, const char *name, Array *params);
/* The instance of `template` for `args` (Array of Type*), interned: the same
   template and argument list always give the same Type. `name` is the display
   name used by diagnostics (`Box<int>`). */
Type *type_instance(TypeContext *tc, Type *template, Array *args, const char *name);
/* The interned instance for `template`/`args`, or NULL. */
Type *type_instance_find(TypeContext *tc, Type *template, Array *args);

/* The number of bytes a value of `t` occupies on the VM stack. Tagged values
   take a full `Value`; packed values are byte-tight. */
size_t type_size(const Type *t);

/* True for the unsigned integer types (`byte`, `uint`, `uint64`). Used by the
   numeric conversion rules and to pick the VM kind. */
bool type_is_unsigned(const Type *t);

/* Scopes. */
Scope *scope_new(Arena *arena, Scope *parent);
Symbol *scope_define(Arena *arena, Scope *scope, const char *name, Type *type,
                     SymbolKind kind, bool is_static);
Symbol *scope_lookup_local(Scope *scope, const char *name);
Symbol *scope_lookup(Scope *scope, const char *name);

#endif // OBY_TYPE_H
