#ifndef OBY_LSP_JSON_H
#define OBY_LSP_JSON_H

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

/* The LSP's non-JSON-party helpers. Reading requests is the vendored
   json.h's job (see `vendor/README.md`); this layer is the framing and the
   small builders for the response bodies. */

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
/* Response bodies                                                    */
/* ------------------------------------------------------------------ */

/* A growable buffer for building response bodies. It stays NUL-terminated as
   it grows; `oom` is set when an allocation failed, and the buffer then holds
   only a prefix and must not be sent. */
typedef struct {
    char  *data;   /* NULL until the first append */
    size_t len;
    size_t cap;
    bool   oom;
} JsonBuf;

void json_buf_init(JsonBuf *b);
void json_buf_free(JsonBuf *b);
void json_buf_printf(JsonBuf *b, const char *fmt, ...);
void json_buf_escape(JsonBuf *b, const char *s);

/* Write `src` JSON-escaped — quotes, backslashes and control characters —
   into `dst`, NUL-terminated, truncated to `cap`. */
void json_escape(char *dst, size_t cap, const char *src);

#endif // OBY_LSP_JSON_H
