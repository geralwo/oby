#include <stdio.h>
#include <inttypes.h>
#include "core/numbers.h"
#include "core/string.h"
#include "native/native.h"

/*
 * int_to_str(int i) -> String
 * int64_to_str(int64 i) -> String
 *
 * Formats `i` as a decimal string. The result is a heap `String`, returned as
 * an object value. One formatter serves both: a boxed integer is 64-bit either
 * way, so `int` and `int64` print identically.
 */
static Value nat_int_to_str(Value *args, int argc) {
    (void)argc;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, args[0].as.i);
    if (n < 0 || (size_t)n >= sizeof(buf)) return val_null();
    String *s = string_new_len(buf, (size_t)n);
    if (!s) return val_null();
    return val_string(s);
}

/*
 * byte_from_int(int i) -> byte
 *
 * Truncates `i` to its low byte. This is the explicit narrowing conversion
 * behind `byte(x)`: the type system never narrows implicitly, so a constructor
 * call is how you ask for one.
 */
static Value nat_byte_from_int(Value *args, int argc) {
    (void)argc;
    return val_int((int64_t)(uint8_t)args[0].as.i);
}

/*
 * The explicit signed <-> unsigned conversions. Crossing signedness is never
 * implicit; these natives are what `uint(x)`, `int(u)`, ... bottom out in, so
 * the sign flip happens once, in C, and the prelude constructors stay thin.
 * They wrap like `byte(300)`: the low bits, read in the target's domain.
 */
static Value nat_uint_from_int(Value *args, int argc) {
    (void)argc;
    return val_int((int64_t)(uint32_t)args[0].as.i);
}

static Value nat_uint_from_int64(Value *args, int argc) {
    (void)argc;
    return val_int((int64_t)(uint32_t)args[0].as.i);
}

static Value nat_uint_from_uint64(Value *args, int argc) {
    (void)argc;
    return val_int((int64_t)(uint32_t)(uint64_t)args[0].as.i);
}

static Value nat_int_from_uint(Value *args, int argc) {
    (void)argc;
    return val_int((int32_t)(uint32_t)args[0].as.i);
}

static Value nat_int64_from_uint64(Value *args, int argc) {
    (void)argc;
    return val_int(args[0].as.i);   /* 64-bit sign flips are bit-identical */
}

static Value nat_uint64_from_int64(Value *args, int argc) {
    (void)argc;
    return val_int(args[0].as.i);
}

/*
 * uint_to_str(uint u) -> String
 * uint64_to_str(uint64 u) -> String
 *
 * Unsigned decimal formatting. The generic `to_str` sees only a boxed integer
 * and would print a high-bit value as negative, so interpolation picks these
 * by the part's static type.
 */
static Value nat_uint_to_str(Value *args, int argc) {
    (void)argc;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRIu32, (uint32_t)args[0].as.i);
    if (n < 0 || (size_t)n >= sizeof(buf)) return val_null();
    String *s = string_new_len(buf, (size_t)n);
    if (!s) return val_null();
    return val_string(s);
}

static Value nat_uint64_to_str(Value *args, int argc) {
    (void)argc;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%" PRIu64, (uint64_t)args[0].as.i);
    if (n < 0 || (size_t)n >= sizeof(buf)) return val_null();
    String *s = string_new_len(buf, (size_t)n);
    if (!s) return val_null();
    return val_string(s);
}

void numbers_register_natives(void) {
    native_add("int_to_str", nat_int_to_str);
    native_add("int64_to_str", nat_int_to_str);   /* the same formatter */
    native_add("byte_from_int", nat_byte_from_int);
    native_add("uint_from_int", nat_uint_from_int);
    native_add("uint_from_int64", nat_uint_from_int64);
    native_add("uint_from_uint64", nat_uint_from_uint64);
    native_add("int_from_uint", nat_int_from_uint);
    native_add("int64_from_uint64", nat_int64_from_uint64);
    native_add("uint64_from_int64", nat_uint64_from_int64);
    native_add("uint_to_str", nat_uint_to_str);
    native_add("uint64_to_str", nat_uint64_to_str);
}
