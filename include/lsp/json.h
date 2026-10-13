#ifndef OBY_LSP_JSON_H
#define OBY_LSP_JSON_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/array.h"

/* ------------------------------------------------------------------ */
/* Framing (base protocol, "Header Part")                             */
/* ------------------------------------------------------------------ */

/* Read one `Content-Length`-framed message from `in`. The body comes back
   malloc'd and NUL-terminated (the caller frees it), with its byte length in
   `*len` — the length excludes the NUL. NULL at end of input or on a
   malformed header/frame. */
char *frame_read(FILE *in, size_t *len);

/* Write one frame and flush it. False on a write error. */
bool frame_write(FILE *out, const char *body);

/* Write one frame whose body is built from a printf-style format. */
bool frame_writef(FILE *out, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* JSON                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonKind;

/* A parsed JSON value. Everything but `raw` lives in the parse arena; `raw`
   points into the source text it was parsed from (kept for echoing request
   ids back verbatim). */
typedef struct JsonValue {
    JsonKind kind;
    bool    boolean;      /* JSON_BOOL */
    double  number;       /* JSON_NUMBER */
    char   *string;       /* JSON_STRING: decoded UTF-8, NUL-terminated */
    size_t  string_len;   /* its length in bytes */
    Array  *items;        /* JSON_ARRAY: JsonValue* */
    Array  *keys;         /* JSON_OBJECT: char*, parallel to `values` */
    Array  *values;       /* JSON_OBJECT: JsonValue* */
    const char *raw;      /* this value's source text */
    size_t  raw_len;
} JsonValue;

/* Parse one complete JSON value into `arena`. NULL when `text` is not exactly
   one value, with `*err` (when not NULL) set to a short reason. */
JsonValue *json_parse(Arena *arena, const char *text, const char **err);

/* The member `key` of an object value, or NULL. */
JsonValue *json_get(const JsonValue *value, const char *key);

/* The bytes of a string value, or NULL. */
const char *json_string(const JsonValue *value);

/* The value of a number that is an exact integer, or false. */
bool json_int(const JsonValue *value, long long *out);

/* Write `src` JSON-escaped — quotes, backslashes and control characters —
   into `dst`, NUL-terminated, truncated to `cap`. */
void json_escape(char *dst, size_t cap, const char *src);

#endif // OBY_LSP_JSON_H
