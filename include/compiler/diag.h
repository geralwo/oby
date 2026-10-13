#ifndef OBY_DIAG_H
#define OBY_DIAG_H

#include "compiler/lexer.h"
#include "core/array.h"

/* Central error/warning reporting.
 *
 * Holds the current file name and source text so diagnostics can render a
 * `file:line:col: error: message` header followed by the offending source
 * line and a caret underline. Lexer, parser and sema all report through one
 * of these. */
#define DIAG_MAX_INCLUDE 32

/* One link in the import chain: a `use` at `file:line`. */
typedef struct {
    const char *file;
    int line;
} IncludeFrame;

/* One recorded diagnostic, for callers that want structured results instead of
   printed ones (see `diag_set_capture`). */
typedef struct {
    int severity;        /* 1 = error, 2 = warning */
    const char *file;    /* the file recorded when it was reported */
    int line;            /* 1-based */
    int column;          /* 1-based byte column within the line */
    size_t length;       /* token length in bytes, 0 = unknown */
    char message[256];   /* formatted text, truncated if it does not fit */
} DiagEntry;

typedef struct {
    const char *file;      /* current file path (may be NULL) */
    const char *source;    /* current source text (may be NULL) */
    int error_count;
    int warning_count;
    IncludeFrame includes[DIAG_MAX_INCLUDE];
    int include_depth;
    bool capturing;        /* record entries instead of printing */
    Array *entries;        /* Array of DiagEntry, created on first record */
} Diagnostics;

void diag_init(Diagnostics *d);
/* Release `entries`, when capture mode recorded any. */
void diag_free(Diagnostics *d);
void diag_set_source(Diagnostics *d, const char *file, const char *source);

/* Capture mode: `diag_error`/`diag_warning` record into `entries` instead of
   writing to stderr, and `diag_summary` does nothing. */
void diag_set_capture(Diagnostics *d, bool capturing);

/* Track the import chain so errors can show how they were reached. */
void diag_push_include(Diagnostics *d, const char *file, int line);
void diag_pop_include(Diagnostics *d);

/* Report an error/warning anchored at a token. */
void diag_error(Diagnostics *d, Token tok, const char *fmt, ...);
void diag_warning(Diagnostics *d, Token tok, const char *fmt, ...);

/* Print a one-line summary if any diagnostics were emitted. */
void diag_summary(Diagnostics *d);

#endif // OBY_DIAG_H
