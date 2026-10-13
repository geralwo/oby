#include <string.h>
#include "compiler/type.h"
#include "core/value.h"

/* The stack layout assumes a tagged value is exactly this wide. */
_Static_assert(sizeof(Value) == 16, "Value size changed; revisit the stack layout");

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

Type *type_new(TypeContext *tc, TypeKind kind, const char *name) {
    Type *t = (Type *)arena_alloc(tc->arena, sizeof(Type));
    if (!t) return NULL;
    memset(t, 0, sizeof(Type));
    t->kind = kind;
    t->name = name;
    t->members = scope_new(tc->arena, NULL);
    if (tc->all) array_push(tc->all, &t);
    return t;
}

Type *type_packed(TypeContext *tc, const char *name, Type *base, size_t size) {
    Type *t = type_new(tc, TYPE_PACKED, name);
    t->base = base;
    t->size = size;
    return t;
}

Type *type_packed_aggregate(TypeContext *tc, const char *name, Type *element, size_t count) {
    Type *t = type_new(tc, TYPE_PACKED, name);
    t->base = element;
    t->count = count;
    t->size = count * type_size(element);
    return t;
}

Type *type_float(TypeContext *tc, const char *name, size_t size) {
    Type *t = type_new(tc, TYPE_PACKED, name);
    t->size = size;
    t->is_float = true;
    return t;
}

Type *type_obj(TypeContext *tc, const char *name) {
    return type_new(tc, TYPE_OBJ, name);
}

Type *type_string(TypeContext *tc, const char *name) {
    return type_new(tc, TYPE_STRING, name);
}

/* Structural types (arrays, function signatures) are interned: the same shape
   is one `Type*`. That keeps pointer equality meaningful — a generic type
   argument written twice must reach the same instance, and object types are
   compared by identity. */
static Type *find_array(TypeContext *tc, Type *element, size_t length) {
    for (size_t i = 0; i < array_length(tc->all); i++) {
        Type *t = *(Type **)array_get(tc->all, i);
        if (t && t->kind == TYPE_ARRAY && t->base == element && t->size == length)
            return t;
    }
    return NULL;
}

static Type *find_func(TypeContext *tc, Type **params, size_t count, Type *ret) {
    for (size_t i = 0; i < array_length(tc->all); i++) {
        Type *t = *(Type **)array_get(tc->all, i);
        if (!t || t->kind != TYPE_FUNC || t->param_count != count || t->ret != ret)
            continue;
        bool same = true;
        for (size_t j = 0; j < count; j++) {
            if (t->params[j] != params[j]) { same = false; break; }
        }
        if (same) return t;
    }
    return NULL;
}

Type *type_array(TypeContext *tc, Type *element, size_t length) {
    Type *existing = find_array(tc, element, length);
    if (existing) return existing;
    Type *t = type_new(tc, TYPE_ARRAY, "array");
    t->base = element;
    t->size = length;
    /* Every array exposes `length`; the VM's GETFIELD special-cases field 0.
       Lengths are `int`: they are counts, not an unsigned domain. */
    Symbol *len = scope_define(tc->arena, t->members, "length", tc->int_type,
                               SYM_MEMBER, false);
    if (len) len->slot = 0;
    return t;
}

Type *type_enum(TypeContext *tc, const char *name, Type *underlying) {
    Type *t = type_new(tc, TYPE_ENUM, name);
    t->base = underlying;
    t->size = underlying ? underlying->size : 0;
    return t;
}

Type *type_func(TypeContext *tc, Type **params, size_t count, Type *ret) {
    Type *existing = find_func(tc, params, count, ret);
    if (existing) return existing;
    Type *t = type_new(tc, TYPE_FUNC, "func");
    t->params = params;
    t->param_count = count;
    t->ret = ret;
    return t;
}

Type *type_template(TypeContext *tc, const char *name, Array *params) {
    Type *t = type_obj(tc, name);
    if (!t) return NULL;
    t->type_params = params;
    t->instances = array_new(sizeof(Type *));
    t->is_template = true;
    return t;
}

/* True when `inst` was made from `tmpl` with exactly these arguments. */
static bool instance_matches(Type *inst, Type *tmpl, Array *args) {
    if (!inst || inst->origin != tmpl || !inst->type_args) return false;
    size_t n = array_length(args);
    if (array_length(inst->type_args) != n) return false;
    for (size_t i = 0; i < n; i++) {
        Type *a = *(Type **)array_get(inst->type_args, i);
        Type *b = *(Type **)array_get(args, i);
        if (a != b) return false;
    }
    return true;
}

Type *type_instance_find(TypeContext *tc, Type *tmpl, Array *args) {
    (void)tc;
    if (!tmpl || !tmpl->instances) return NULL;
    for (size_t i = 0; i < array_length(tmpl->instances); i++) {
        Type *inst = *(Type **)array_get(tmpl->instances, i);
        if (instance_matches(inst, tmpl, args)) return inst;
    }
    return NULL;
}

Type *type_instance(TypeContext *tc, Type *tmpl, Array *args, const char *name) {
    Type *existing = type_instance_find(tc, tmpl, args);
    if (existing) return existing;
    Type *t = type_obj(tc, name);
    if (!t) return NULL;
    t->origin = tmpl;
    t->type_args = args;
    if (tmpl) {
        if (!tmpl->instances) tmpl->instances = array_new(sizeof(Type *));
        array_push(tmpl->instances, &t);
    }
    return t;
}

void type_context_init(TypeContext *tc, Arena *arena) {
    tc->arena = arena;
    tc->all = array_new(sizeof(Type *));
    tc->unknown = type_new(tc, TYPE_UNKNOWN, "?");
    tc->void_type = type_new(tc, TYPE_VOID, "void");
    tc->null_type = type_new(tc, TYPE_NULL, "null");
    tc->byte = type_packed(tc, "byte", NULL, 1);
    tc->byte->base = tc->byte;
    tc->object = type_obj(tc, "object");
    tc->string = type_string(tc, "String");
    tc->float_type = type_float(tc, "float", 4);
    tc->double_type = type_float(tc, "double", 8);
    /* Native integer types: fixed-width packed values over `byte`. */
    tc->int_type = type_packed(tc, "int", tc->byte, 4);
    tc->uint_type = type_packed(tc, "uint", tc->byte, 4);
    tc->int64_type = type_packed(tc, "int64", tc->byte, 8);
    tc->uint64_type = type_packed(tc, "uint64", tc->byte, 8);
}

/* ------------------------------------------------------------------ */
/* Sizes                                                              */
/* ------------------------------------------------------------------ */

size_t type_size(const Type *t) {
    if (!t) return sizeof(Value);
    switch (t->kind) {
        case TYPE_VOID:    return 0;
        case TYPE_PACKED:  return t->size ? t->size : sizeof(Value);
        case TYPE_ENUM:    return type_size(t->base);
        default:           return sizeof(Value);
    }
}

bool type_is_unsigned(const Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_ENUM) return type_is_unsigned(t->base);
    if (t->kind == TYPE_PACKED && t->is_float) return false;
    if (!t->name) return false;
    if (strcmp(t->name, "byte") == 0) return true;
    return t->name[0] == 'u';
}

/* ------------------------------------------------------------------ */
/* Scopes                                                             */
/* ------------------------------------------------------------------ */

Scope *scope_new(Arena *arena, Scope *parent) {
    Scope *s = (Scope *)arena_alloc(arena, sizeof(Scope));
    if (!s) return NULL;
    s->parent = parent;
    s->symbols = array_new(sizeof(Symbol *));
    return s;
}

Symbol *scope_define(Arena *arena, Scope *scope, const char *name, Type *type,
                     SymbolKind kind, bool is_static) {
    Symbol *sym = (Symbol *)arena_alloc(arena, sizeof(Symbol));
    if (!sym) return NULL;
    /* The arena hands out raw memory: zero it first, so a stray `is_generic`
       or `is_alias` bit cannot change how the symbol is treated later. */
    memset(sym, 0, sizeof(Symbol));
    sym->name = name;
    sym->type = type;
    sym->kind = kind;
    sym->is_static = is_static;
    sym->native_id = -1;
    sym->slot = -1;
    sym->func_id = -1;
    sym->def_func = -1;
    sym->next = scope_lookup_local(scope, name);
    Symbol *sp = sym;
    array_push(scope->symbols, &sp);
    return sym;
}

/* Most recently defined symbol with this name in this scope. */
Symbol *scope_lookup_local(Scope *scope, const char *name) {
    if (!scope || !scope->symbols) return NULL;
    for (size_t i = array_length(scope->symbols); i > 0; i--) {
        Symbol *sym = *(Symbol **)array_get(scope->symbols, i - 1);
        if (sym && sym->name && strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

Symbol *scope_lookup(Scope *scope, const char *name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *sym = scope_lookup_local(s, name);
        if (sym) return sym;
    }
    return NULL;
}
