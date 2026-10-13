#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "compiler/diag.h"

void diag_init(Diagnostics *d) {
    d->file = NULL;
    d->source = NULL;
    d->error_count = 0;
    d->warning_count = 0;
    d->include_depth = 0;
    d->capturing = false;
    d->entries = NULL;
}

void diag_free(Diagnostics *d) {
    if (d->entries) {
        array_free(d->entries);
        d->entries = NULL;
    }
}

void diag_set_capture(Diagnostics *d, bool capturing) {
    d->capturing = capturing;
}

void diag_set_source(Diagnostics *d, const char *file, const char *source) {
    d->file = file;
    d->source = source;
}

void diag_push_include(Diagnostics *d, const char *file, int line) {
    if (d->include_depth < DIAG_MAX_INCLUDE) {
        d->includes[d->include_depth].file = file;
        d->includes[d->include_depth].line = line;
        d->include_depth++;
    }
}

void diag_pop_include(Diagnostics *d) {
    if (d->include_depth > 0) d->include_depth--;
}

/* 1-based column of `pos` within its line. */
static int compute_col(const char *source, const char *pos) {
    if (!source || !pos || pos < source) return 1;
    const char *line_start = pos;
    while (line_start > source && line_start[-1] != '\n') line_start--;
    return (int)(pos - line_start) + 1;
}

/* Locate the `line`-th line (1-based) in `source`. */
static void get_line(const char *source, int line, const char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (!source || line < 1) return;
    const char *p = source;
    int cur = 1;
    while (cur < line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return;
    const char *s = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    *out = s;
    *len = (size_t)(p - s);
}

static void print_location(Diagnostics *d, int line, const char *pos, size_t len) {
    int col = compute_col(d->source, pos);

    const char *ls;
    size_t ll;
    get_line(d->source, line, &ls, &ll);
    if (!ls) return;

    int w = snprintf(NULL, 0, "%d", line);
    if (w < 1) w = 1;

    fprintf(stderr, " %*d | %.*s\n", w, line, (int)ll, ls);

    /* Caret line: align under the token, clamped to the line. */
    size_t caret = len ? len : 1;
    if ((size_t)(col - 1) >= ll) {
        /* Token starts past the visible line; point at the end. */
        col = (int)ll + 1;
        caret = 1;
    } else if ((size_t)(col - 1) + caret > ll) {
        caret = ll - (size_t)(col - 1);
    }

    fprintf(stderr, " %*s | ", w, "");
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    for (size_t i = 0; i < caret; i++) fputc('^', stderr);
    fprintf(stderr, "\n");
}

static void vreport(Diagnostics *d, int severity, const char *level, int line,
                    const char *pos, size_t len, const char *fmt, va_list ap) {
    int col = compute_col(d->source, pos);

    if (d->capturing) {
        if (!d->entries) d->entries = array_new(sizeof(DiagEntry));
        if (d->entries) {
            DiagEntry e;
            e.severity = severity;
            e.file = d->file;
            e.line = line;
            e.column = col;
            e.length = len;
            vsnprintf(e.message, sizeof(e.message), fmt, ap);
            array_push(d->entries, &e);
        }
        return;
    }

    const char *file = d->file ? d->file : "<input>";
    fprintf(stderr, "%s:%d:%d: %s: ", file, line, col, level);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    print_location(d, line, pos, len);
    /* Show how we got here: the chain of `use`s that led to this file. */
    for (int i = d->include_depth - 1; i >= 0; i--) {
        fprintf(stderr, "  note: imported from %s:%d\n",
                d->includes[i].file ? d->includes[i].file : "<input>",
                d->includes[i].line);
    }
}

void diag_error(Diagnostics *d, Token tok, const char *fmt, ...) {
    d->error_count++;
    va_list ap;
    va_start(ap, fmt);
    vreport(d, 1, "error", tok.line, tok.start, tok.length, fmt, ap);
    va_end(ap);
}

void diag_warning(Diagnostics *d, Token tok, const char *fmt, ...) {
    d->warning_count++;
    va_list ap;
    va_start(ap, fmt);
    vreport(d, 2, "warning", tok.line, tok.start, tok.length, fmt, ap);
    va_end(ap);
}

void diag_summary(Diagnostics *d) {
    if (d->capturing) return;
    if (d->error_count == 0 && d->warning_count == 0) return;
    fprintf(stderr, "%d error%s, %d warning%s\n",
            d->error_count, d->error_count == 1 ? "" : "s",
            d->warning_count, d->warning_count == 1 ? "" : "s");
}
