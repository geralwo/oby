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
/* JSON                                                               */
/* ------------------------------------------------------------------ */

#define JSON_MAX_DEPTH 64

typedef struct {
    Arena *arena;
    const char *p;
    const char *end;
    const char *err;   /* first failure, a static string */
} JParse;

static void fail(JParse *j, const char *err) {
    if (!j->err) j->err = err;
}

static void skip_ws(JParse *j) {
    while (j->p < j->end &&
           (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static JsonValue *new_value(JParse *j, JsonKind kind) {
    JsonValue *v = (JsonValue *)arena_alloc(j->arena, sizeof(JsonValue));
    if (!v) {
        fail(j, "out of memory");
        return NULL;
    }
    memset(v, 0, sizeof *v);
    v->kind = kind;
    return v;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A `\uXXXX` body: four hex digits into `*out`. */
static bool parse_hex4(JParse *j, unsigned long *out) {
    if (j->end - j->p < 4) {
        fail(j, "truncated \\u escape");
        return false;
    }
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(j->p[i]);
        if (h < 0) {
            fail(j, "bad \\u escape");
            return false;
        }
        v = (v << 4) | (unsigned long)h;
    }
    j->p += 4;
    *out = v;
    return true;
}

/* Encode the code point `cp` as UTF-8 into `out`; returns the byte count. */
static size_t utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A string starting at its opening quote; advances past the closing one.
   The decoded bytes live in the arena. */
static char *parse_string(JParse *j, size_t *len) {
    j->p++;   /* the opening quote */
    const char *start = j->p;
    /* Decoding never grows the text, so the source span is a safe bound. */
    char *buf = (char *)arena_alloc(j->arena, (size_t)(j->end - start) + 1);
    if (!buf) {
        fail(j, "out of memory");
        return NULL;
    }
    size_t o = 0;

    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->end) {
                fail(j, "truncated escape");
                return NULL;
            }
            char e = *j->p++;
            switch (e) {
                case '"':  buf[o++] = '"'; break;
                case '\\': buf[o++] = '\\'; break;
                case '/':  buf[o++] = '/'; break;
                case 'b':  buf[o++] = '\b'; break;
                case 'f':  buf[o++] = '\f'; break;
                case 'n':  buf[o++] = '\n'; break;
                case 'r':  buf[o++] = '\r'; break;
                case 't':  buf[o++] = '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!parse_hex4(j, &cp)) return NULL;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        /* A high surrogate: the low half must follow. */
                        unsigned long lo = 0;
                        if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') {
                            fail(j, "unpaired surrogate");
                            return NULL;
                        }
                        j->p += 2;
                        if (!parse_hex4(j, &lo)) return NULL;
                        if (lo < 0xDC00 || lo > 0xDFFF) {
                            fail(j, "unpaired surrogate");
                            return NULL;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail(j, "unpaired surrogate");
                        return NULL;
                    }
                    o += utf8_encode(cp, buf + o);
                    break;
                }
                default:
                    fail(j, "unknown escape");
                    return NULL;
            }
        } else if (c < 0x20) {
            fail(j, "control character in string");
            return NULL;
        } else {
            buf[o++] = (char)c;
        }
    }

    if (j->p >= j->end) {
        fail(j, "unterminated string");
        return NULL;
    }
    j->p++;   /* the closing quote */
    buf[o] = '\0';
    if (len) *len = o;
    return buf;
}

static bool is_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == ',' || c == '}' || c == ']';
}

static JsonValue *parse_value(JParse *j, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        fail(j, "nesting too deep");
        return NULL;
    }
    skip_ws(j);
    if (j->p >= j->end) {
        fail(j, "unexpected end of input");
        return NULL;
    }
    const char *raw = j->p;

    if (*j->p == '{') {
        j->p++;
        JsonValue *v = new_value(j, JSON_OBJECT);
        if (!v) return NULL;
        v->keys = array_new(sizeof(char *));
        v->values = array_new(sizeof(JsonValue *));
        if (!v->keys || !v->values) {
            fail(j, "out of memory");
            return NULL;
        }
        skip_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
        } else {
            for (;;) {
                skip_ws(j);
                if (j->p >= j->end || *j->p != '"') {
                    fail(j, "expected a member name");
                    return NULL;
                }
                char *key = parse_string(j, NULL);
                if (!key) return NULL;
                skip_ws(j);
                if (j->p >= j->end || *j->p != ':') {
                    fail(j, "expected ':'");
                    return NULL;
                }
                j->p++;
                JsonValue *value = parse_value(j, depth + 1);
                if (!value) return NULL;
                if (array_push(v->keys, &key) != 0 ||
                    array_push(v->values, &value) != 0) {
                    fail(j, "out of memory");
                    return NULL;
                }
                skip_ws(j);
                if (j->p < j->end && *j->p == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->end && *j->p == '}') {
                    j->p++;
                    break;
                }
                fail(j, "expected ',' or '}'");
                return NULL;
            }
        }
        v->raw = raw;
        v->raw_len = (size_t)(j->p - raw);
        return v;
    }

    if (*j->p == '[') {
        j->p++;
        JsonValue *v = new_value(j, JSON_ARRAY);
        if (!v) return NULL;
        v->items = array_new(sizeof(JsonValue *));
        if (!v->items) {
            fail(j, "out of memory");
            return NULL;
        }
        skip_ws(j);
        if (j->p < j->end && *j->p == ']') {
            j->p++;
        } else {
            for (;;) {
                JsonValue *item = parse_value(j, depth + 1);
                if (!item) return NULL;
                if (array_push(v->items, &item) != 0) {
                    fail(j, "out of memory");
                    return NULL;
                }
                skip_ws(j);
                if (j->p < j->end && *j->p == ',') {
                    j->p++;
                    continue;
                }
                if (j->p < j->end && *j->p == ']') {
                    j->p++;
                    break;
                }
                fail(j, "expected ',' or ']'");
                return NULL;
            }
        }
        v->raw = raw;
        v->raw_len = (size_t)(j->p - raw);
        return v;
    }

    if (*j->p == '"') {
        JsonValue *v = new_value(j, JSON_STRING);
        if (!v) return NULL;
        v->string = parse_string(j, &v->string_len);
        if (!v->string) return NULL;
        v->raw = raw;
        v->raw_len = (size_t)(j->p - raw);
        return v;
    }

    if (*j->p == 't' && j->end - j->p >= 4 && strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        JsonValue *v = new_value(j, JSON_BOOL);
        if (!v) return NULL;
        v->boolean = true;
        v->raw = raw;
        v->raw_len = 4;
        return v;
    }
    if (*j->p == 'f' && j->end - j->p >= 5 && strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        JsonValue *v = new_value(j, JSON_BOOL);
        if (!v) return NULL;
        v->boolean = false;
        v->raw = raw;
        v->raw_len = 5;
        return v;
    }
    if (*j->p == 'n' && j->end - j->p >= 4 && strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
        JsonValue *v = new_value(j, JSON_NULL);
        if (!v) return NULL;
        v->raw = raw;
        v->raw_len = 4;
        return v;
    }

    if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
        char *end = NULL;
        double number = strtod(j->p, &end);
        if (end == j->p) {
            fail(j, "bad number");
            return NULL;
        }
        /* The number must end where the value does. */
        if (end < j->end && !is_delimiter(*end)) {
            fail(j, "bad number");
            return NULL;
        }
        JsonValue *v = new_value(j, JSON_NUMBER);
        if (!v) return NULL;
        v->number = number;
        j->p = end;
        v->raw = raw;
        v->raw_len = (size_t)(j->p - raw);
        return v;
    }

    fail(j, "unexpected character");
    return NULL;
}

JsonValue *json_parse(Arena *arena, const char *text, const char **err) {
    JParse j;
    j.arena = arena;
    j.p = text;
    j.end = text + strlen(text);
    j.err = NULL;

    JsonValue *v = parse_value(&j, 0);
    if (v) {
        skip_ws(&j);
        if (j.p != j.end) {
            v = NULL;
            fail(&j, "trailing characters");
        }
    }
    if (!v && err) *err = j.err ? j.err : "parse error";
    return v;
}

JsonValue *json_get(const JsonValue *value, const char *key) {
    if (!value || value->kind != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < array_length(value->keys); i++) {
        char *k = *(char **)array_get(value->keys, i);
        if (k && strcmp(k, key) == 0)
            return *(JsonValue **)array_get(value->values, i);
    }
    return NULL;
}

const char *json_string(const JsonValue *value) {
    if (!value || value->kind != JSON_STRING) return NULL;
    return value->string;
}

bool json_int(const JsonValue *value, long long *out) {
    if (!value || value->kind != JSON_NUMBER) return false;
    long long i = (long long)value->number;
    if ((double)i != value->number) return false;
    *out = i;
    return true;
}

void json_escape(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char single[2] = { (char)*p, '\0' };
        char buf[8];
        const char *rep = single;
        switch (*p) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b"; break;
            case '\f': rep = "\\f"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(buf, sizeof buf, "\\u%04x", (unsigned)*p);
                    rep = buf;
                }
                break;
        }
        size_t rl = strlen(rep);
        if (o + rl + 1 > cap) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}
