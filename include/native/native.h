#ifndef OBY_NATIVE_H
#define OBY_NATIVE_H

#include <stdint.h>
#include <stdbool.h>
#include "core/value.h"

/*
 * Native (C) functions callable from OBY through `extern`.
 *
 * Values cross the boundary as tagged `Value`s, so a native can inspect a
 * value's type. The static signature is still checked at compile time; the
 * tag is what makes dynamic features (and a generic marshaller) possible.
 */
typedef Value (*NativeFn)(Value *args, int argc);

/* Registry lifecycle. The other functions call this lazily, so an explicit
   call is only needed to control when the table is first allocated. */
void native_init(void);

/* Register `fn` under `name` (the C symbol written after `extern`).
   Returns false if the name is already registered. */
bool native_add(const char *name, NativeFn fn);

/* Look up a native by name. Returns its id, or -1 if not found. */
int native_find(const char *name);

/* The name registered for `id`, or NULL if `id` is out of range. */
const char *native_name(int id);

/* Invoke the native with `id`. Returns a null value for an invalid id. */
Value native_call(int id, Value *args, int argc);

/* Register the built-in natives the prelude depends on. Idempotent. */
void native_register_std(void);

/* A hash of the registered native names in id order. A bytecode image records
   the fingerprint of the table that compiled it; a runner whose table differs
   refuses the image instead of calling the wrong C function. Registers the
   built-in natives first, so the value is the same in every process. */
uint64_t native_fingerprint(void);

#endif // OBY_NATIVE_H
