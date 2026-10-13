#ifndef OBY_OBJ_H
#define OBY_OBJ_H

/*
 * Base for every heap object. The kind tag lets a Value identify what it
 * points at. Objects are freed manually (there is no GC).
 *
 * `String` and `Array` are built-in primitives, not `Obj`s; `VAL_OBJ` is for
 * dynamic and nominal `{}` objects.
 */
typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_OBJECT,
} ObjKind;

typedef struct Obj {
    ObjKind kind;
} Obj;

#endif // OBY_OBJ_H
