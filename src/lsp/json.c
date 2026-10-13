#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "lsp/json.h"

/* ------------------------------------------------------------------ */
/* Framing                                                            */
/* ------------------------------------------------------------------ */

/* A frame beyond this is refused: the protocol is not a bulk transfer, and a
   wild Content-Length must not become a wild allocation. */
#define FRAME_MAX (16u * 1024u * 1024u)

/* ASCII case-insensitive: does `line` start with `prefix` (lowercase)? */
static bool ci_prefix(const char *line, const char *prefix) {
    for (; *prefix; line++, prefix++) {
        char c = *line;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *prefix) return false;
    }
    return true;
}

char *frame_read(FILE *in, size_t *len) {
    char header[128];
    size_t content_length = 0;
    bool have_length = false;

    for (;;) {
        if (!fgets(header, sizeof header, in)) return NULL;   /* EOF or error */
        size_t hl = strlen(header);
        while (hl > 0 && (header[hl - 1] == '\n' || header[hl - 1] == '\r')) {
            header[--hl] = '\0';
        }
        if (hl == 0) break;   /* the blank line before the body */

        if (ci_prefix(header, "content-length:")) {
            const char *value = header + strlen("content-length:");
            while (*value == ' ' || *value == '\t') value++;
            char *end = NULL;
            unsigned long long n = strtoull(value, &end, 10);
            if (end == value) return NULL;
            content_length = (size_t)n;
            have_length = true;
        }
        /* Any other header (Content-Type and friends) is ignored: utf-8 is
           the only charset the protocol allows, and it is what we assume. */
    }

    if (!have_length || content_length == 0 || content_length > FRAME_MAX) return NULL;

    char *body = (char *)malloc(content_length + 1);
    if (!body) return NULL;
    size_t got = 0;
    while (got < content_length) {
        size_t n = fread(body + got, 1, content_length - got, in);
        if (n == 0) {   /* the input ended inside the body */
            free(body);
            return NULL;
        }
        got += n;
    }
    body[content_length] = '\0';
    if (len) *len = content_length;
    return body;
}

bool frame_write(FILE *out, const char *body) {
    size_t len = strlen(body);
    if (fprintf(out, "Content-Length: %zu\r\n\r\n", len) < 0) return false;
    if (fwrite(body, 1, len, out) != len) return false;
    return fflush(out) == 0;
}

bool frame_writef(FILE *out, const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(copy);
        return false;
    }

    char *body = (char *)malloc((size_t)needed + 1);
    if (!body) {
        va_end(copy);
        return false;
    }
    vsnprintf(body, (size_t)needed + 1, fmt, copy);
    va_end(copy);

    bool ok = frame_write(out, body);
    free(body);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Response bodies                                                    */
/* ------------------------------------------------------------------ */

/* The JSON replacement text for one byte, written into `buf` (8 bytes). */
static const char *escape_rep(unsigned char c, char *buf) {
    switch (c) {
        case '"':  return "\\\"";
        case '\\': return "\\\\";
        case '\b': return "\\b";
        case '\f': return "\\f";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        default:
            if (c < 0x20) {
                snprintf(buf, 8, "\\u%04x", (unsigned)c);
                return buf;
            }
            buf[0] = (char)c;
            buf[1] = '\0';
            return buf;
    }
}

void json_escape(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char rep_buf[8];
        const char *rep = escape_rep(*p, rep_buf);
        size_t rl = strlen(rep);
        if (o + rl + 1 > cap) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}

static bool buf_reserve(JsonBuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *grown = (char *)realloc(b->data, cap);
    if (!grown) {
        b->oom = true;
        return false;
    }
    b->data = grown;
    b->cap = cap;
    return true;
}

void json_buf_init(JsonBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

void json_buf_free(JsonBuf *b) {
    free(b->data);
    json_buf_init(b);
}

void json_buf_printf(JsonBuf *b, const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(copy);
        b->oom = true;
        return;
    }
    if (!buf_reserve(b, (size_t)needed)) {
        va_end(copy);
        return;
    }
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, copy);
    va_end(copy);
    b->len += (size_t)needed;
}

void json_buf_escape(JsonBuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char rep_buf[8];
        const char *rep = escape_rep(*p, rep_buf);
        if (!buf_reserve(b, 6)) return;
        size_t rl = strlen(rep);
        memcpy(b->data + b->len, rep, rl);
        b->len += rl;
        b->data[b->len] = '\0';
    }
}
