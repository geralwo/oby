#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "native/strings.h"
#include "native/native.h"
#include "core/string.h"
#include "core/array.h"
#include "util.h"

/* to_str(any) -> String : format any value as a string. */
static Value nat_to_str(Value *args, int argc) {
    (void)argc;
    Value v = args[0];
    char buf[48];
    switch (v.tag) {
        case VAL_STRING:
            return v;
        case VAL_INT: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i);
            if (n < 0) break;
            String *s = string_new_len(buf, (size_t)n);
            return s ? val_string(s) : val_null();
        }
        case VAL_FLOAT: {
            int n = snprintf(buf, sizeof(buf), "%g", v.as.f);
            if (n < 0) break;
            String *s = string_new_len(buf, (size_t)n);
            return s ? val_string(s) : val_null();
        }
        case VAL_BOOL: {
            String *s = string_new(v.as.b ? "true" : "false");
            return s ? val_string(s) : val_null();
        }
        case VAL_CLOSURE: {
            String *s = string_new("<closure>");
            return s ? val_string(s) : val_null();
        }
        default: {
            String *s = string_new("null");
            return s ? val_string(s) : val_null();
        }
    }
    return val_null();
}

/* str_concat(String a, String b) -> String */
static Value nat_str_concat(Value *args, int argc) {
    (void)argc;
    if (args[0].tag != VAL_STRING || args[1].tag != VAL_STRING) return val_null();
    String *s = string_concat(args[0].as.str, args[1].as.str);
    return s ? val_string(s) : val_null();
}

/* str_slice(String s, int start, int end) -> String : a copy of `s[start, end)`.
   Out-of-range bounds are clamped, so a bad slice is empty, not an error. */
static Value nat_str_slice(Value *args, int argc) {
    (void)argc;
    if (args[0].tag != VAL_STRING) return val_null();
    const String *s = args[0].as.str;
    int64_t start = args[1].as.i;
    int64_t end = args[2].as.i;
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (end > (int64_t)s->length) end = (int64_t)s->length;
    String *out = string_new_len(s->data + start, (size_t)(end - start));
    return out ? val_string(out) : val_null();
}

/* str_from_bytes(byte[] bytes, int start, int count) -> String : copies `count`
   bytes of the array's buffer into a fresh String. */
static Value nat_str_from_bytes(Value *args, int argc) {
    (void)argc;
    if (args[0].tag != VAL_ARRAY) return val_null();
    const Array *a = args[0].as.arr;
    int64_t start = args[1].as.i;
    int64_t count = args[2].as.i;
    if (start < 0) start = 0;
    if (count < 0) count = 0;
    if (start > (int64_t)a->length) start = (int64_t)a->length;
    if (count > (int64_t)a->length - start) count = (int64_t)a->length - start;
    String *out = string_new_len((const char *)a->buffer + start, (size_t)count);
    return out ? val_string(out) : val_null();
}

/* Copy a String's bytes into a NUL-terminated buffer for strtoll/strtod. */
static bool number_text(const Value *v, char *buf, size_t cap) {
    if (v->tag != VAL_STRING) return false;
    const String *s = v->as.str;
    if (s->length >= cap) return false;
    memcpy(buf, s->data, s->length);
    buf[s->length] = '\0';
    return true;
}

static bool only_space(const char *p) {
    for (; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
    return true;
}

/* Decimal, `0x` hex, or `0b` binary, with an optional sign; `*end` points past
   the digits. There is no octal: `010` is ten. `strtoll` knows base 16 (and the
   `0x` prefix) but not `0b`, so binary is scanned by hand. Digit separators
   (`1_000`) are dropped; a misplaced one is not a number. */
static long long parse_int_text(char *buf, char **end) {
    /* The base is visible in the prefix, which sits after any sign. */
    const char *s = buf;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '+' || *s == '-') s++;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16;
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) base = 2;
    if (!strip_digit_separators(buf, base)) { *end = buf; return 0; }

    char *p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return strtoll(buf, end, 16);
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        char *digits = p + 2;
        char *q = digits;
        unsigned long long v = 0;
        bool overflow = false;
        for (; *q == '0' || *q == '1'; q++) {
            if (v > (ULLONG_MAX >> 1)) overflow = true;
            else v = (v << 1) | (unsigned long long)(*q - '0');
        }
        if (q == digits) { *end = buf; return 0; }   /* `0b` with no digits */
        *end = q;
        if (overflow || v > (unsigned long long)LLONG_MAX)
            return neg ? LLONG_MIN : LLONG_MAX;      /* clamp, like strtoll */
        return neg ? -(long long)v : (long long)v;
    }
    return strtoll(buf, end, 10);
}

/* str_to_int(String s) -> int : parses the whole text (surrounding spaces are
   ignored) as decimal, `0x` hex, or `0b` binary. Not a number means 0 — the
   language has no exceptions. */
static Value nat_str_to_int(Value *args, int argc) {
    (void)argc;
    char buf[64];
    if (!number_text(&args[0], buf, sizeof(buf))) return val_int(0);
    char *end = NULL;
    long long v = parse_int_text(buf, &end);
    if (end == buf || !only_space(end)) return val_int(0);
    return val_int((int64_t)v);
}

/* str_to_float(String s) -> float : the same, via strtod. */
static Value nat_str_to_float(Value *args, int argc) {
    (void)argc;
    char buf[64];
    if (!number_text(&args[0], buf, sizeof(buf))) return val_float(0.0);
    if (!strip_digit_separators(buf, 10)) return val_float(0.0);
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || !only_space(end)) return val_float(0.0);
    return val_float(v);
}

void strings_register_natives(void) {
    native_add("to_str", nat_to_str);
    native_add("str_concat", nat_str_concat);
    native_add("str_slice", nat_str_slice);
    native_add("str_from_bytes", nat_str_from_bytes);
    native_add("str_to_int", nat_str_to_int);
    native_add("str_to_float", nat_str_to_float);
}
