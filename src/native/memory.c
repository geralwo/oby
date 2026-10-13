#include "native/memory.h"
#include "native/native.h"
#include "core/string.h"
#include "core/array.h"
#include "core/object.h"

/*
 * ob_free(object o) -> void : free a heap value.
 *
 * Manual memory management: the caller must not use the value afterwards, and
 * must not free it twice or free an alias. Values that are not heap-allocated
 * (numbers, bools, null, functions) are ignored.
 */
static Value nat_free(Value *args, int argc) {
    (void)argc;
    Value v = args[0];
    switch (v.tag) {
        case VAL_STRING: string_free(v.as.str); break;
        case VAL_ARRAY:
            /* A view (e.g. `s.buffer`) is owned by its parent, not the caller. */
            if (v.as.arr->flags & ARRAY_F_OWNED) array_free(v.as.arr);
            break;
        case VAL_OBJ: object_free((Object *)v.as.obj); break;
        case VAL_CLOSURE: closure_free(v.as.closure); break;
        default: break;
    }
    return val_null();
}

void memory_register_natives(void) {
    native_add("ob_free", nat_free);
}
