#ifndef OBY_VALUE_H
#define OBY_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "core/obj.h"
#include "core/string.h"
#include "core/array.h"

typedef struct Function Function;  /* vm/function.h */

/* A closure: a compiled function plus its captured state. The captured bytes
   are laid out using the function's `up_off`/`up_size` tables, so a function's
   body can find each captured value at a fixed offset. */
typedef struct Closure {
    Function *fn;
    size_t    up_total;   /* bytes of captured state in `up` */
    uint8_t  *up;
} Closure;

Closure *closure_new(Function *fn, size_t up_total);
void     closure_free(Closure *c);

/*
 * A value. Every value carries a tag, so its type is known without external
 * type information. This is the unit that crosses the native (extern)
 * boundary and sits on the VM stack.
 *
 * `String` and `Array` are built-in primitives with their own tags, not heap
 * `Obj`s. `VAL_FUNC` holds a compiled function; `VAL_OBJ` is for dynamic and
 * nominal `{}` objects.
 */
typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,     /* int64_t */
    VAL_FLOAT,   /* double */
    VAL_STRING,  /* String * (built-in primitive) */
    VAL_ARRAY,   /* Array * (built-in primitive) */
    VAL_FUNC,    /* Function * (a compiled function) */
    VAL_CLOSURE, /* Closure * (a compiled function + captured state) */
    VAL_OBJ,     /* Obj * (dynamic/nominal objects) */
} ValueTag;

typedef struct {
    ValueTag tag;
    union {
        bool    b;
        int64_t i;
        double  f;
        String   *str;
        Array    *arr;
        Function *fn;
        Closure  *closure;
        Obj      *obj;
    } as;
} Value;

static inline Value val_null(void)        { Value v; v.tag = VAL_NULL;   v.as.i = 0; return v; }
static inline Value val_bool(bool b)      { Value v; v.tag = VAL_BOOL;   v.as.b = b; return v; }
static inline Value val_int(int64_t i)    { Value v; v.tag = VAL_INT;    v.as.i = i; return v; }
static inline Value val_float(double f)   { Value v; v.tag = VAL_FLOAT;  v.as.f = f; return v; }
static inline Value val_string(String *s) { Value v; v.tag = VAL_STRING; v.as.str = s; return v; }
static inline Value val_array(Array *a)   { Value v; v.tag = VAL_ARRAY;  v.as.arr = a; return v; }
static inline Value val_func(Function *f) { Value v; v.tag = VAL_FUNC;   v.as.fn = f; return v; }
static inline Value val_closure(Closure *c) { Value v; v.tag = VAL_CLOSURE; v.as.closure = c; return v; }
static inline Value val_obj(Obj *o)       { Value v; v.tag = VAL_OBJ;    v.as.obj = o; return v; }

#endif // OBY_VALUE_H
