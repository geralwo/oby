#include <stdlib.h>
#include "core/object.h"

Object *object_new(size_t size) {
    Object *o = (Object *)malloc(sizeof(Object));
    if (!o) return NULL;
    o->base.kind = OBJ_OBJECT;
    o->size = size;
    o->data = NULL;
    if (size) {
        /* Zeroed: a tagged field reads as null, a packed field as 0. */
        o->data = (uint8_t *)calloc(1, size);
        if (!o->data) { free(o); return NULL; }
    }
    return o;
}

void object_free(Object *obj) {
    if (!obj) return;
    free(obj->data);
    free(obj);
}
