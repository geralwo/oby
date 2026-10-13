#ifndef OBY_OBJECT_H
#define OBY_OBJECT_H

#include <stddef.h>
#include <stdint.h>
#include "core/value.h"

/*
 * A `{}` object (dynamic or nominal): an Obj header plus a byte buffer holding
 * its fields. The compiler lays each field out at a byte offset, sized from its
 * type, so packed values (`vec2i`, `int`, ...) are stored inline and tagged
 * values (`String`, arrays, nested objects) take a 16-byte `Value` slot.
 *
 * Objects are freed manually (there is no GC).
 */
typedef struct Object {
    Obj      base;
    size_t   size;   /* bytes of field storage */
    uint8_t *data;
} Object;

Object *object_new(size_t size);
void    object_free(Object *obj);

#endif // OBY_OBJECT_H
