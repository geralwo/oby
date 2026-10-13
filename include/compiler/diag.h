#ifndef OBY_DIAG_H
#define OBY_DIAG_H

#include "compiler/lexer.h"

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

typedef struct {
    const char *file;      /* current file path (may be NULL) */
    const char *source;    /* current source text (may be NULL) */
    int error_count;
    int warning_count;
    IncludeFrame includes[DIAG_MAX_INCLUDE];
    int include_depth;
} Diagnostics;

void diag_init(Diagnostics *d);
void diag_set_source(Diagnostics *d, const char *file, const char *source);

/* Track the import chain so errors can show how they were reached. */
void diag_push_include(Diagnostics *d, const char *file, int line);
void diag_pop_include(Diagnostics *d);

/* Report an error/warning anchored at a token. */
void diag_error(Diagnostics *d, Token tok, const char *fmt, ...);
void diag_warning(Diagnostics *d, Token tok, const char *fmt, ...);

/* Report without a token (no source line/caret). */
void diag_error_line(Diagnostics *d, int line, const char *fmt, ...);

/* Print a one-line summary if any diagnostics were emitted. */
void diag_summary(Diagnostics *d);

#endif // OBY_DIAG_H
