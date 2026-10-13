#include <stdio.h>
#include <string.h>
#include "compiler/diag.h"

/* Capture mode: diagnostics become structured entries instead of stderr text.
   This mirrors what the language server will consume. */

static const char *src =
    "x = 1\n"
    "y = 2\n";

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    Diagnostics d;
    diag_init(&d);
    diag_set_source(&d, "test.oby", src);

    Token tok;
    memset(&tok, 0, sizeof tok);
    tok.start = src + 6;   /* `y`, line 2, column 1 */
    tok.line = 2;
    tok.length = 1;

    /* Without capture, nothing is recorded. */
    diag_error(&d, tok, "plain %s", "error");
    check(d.error_count == 1, "counts increment without capture");
    check(d.entries == NULL, "no entries without capture");

    /* With capture, formatting goes into entries instead of stderr. */
    Diagnostics cap;
    diag_init(&cap);
    diag_set_source(&cap, "test.oby", src);
    diag_set_capture(&cap, true);

    diag_warning(&cap, tok, "watch %s", "out");
    diag_error(&cap, tok, "bad %d", 7);

    check(cap.entries != NULL, "entries recorded");
    check(cap.error_count == 1 && cap.warning_count == 1, "counts increment");
    check(array_length(cap.entries) == 2, "two entries");

    DiagEntry *w = (DiagEntry *)array_get(cap.entries, 0);
    DiagEntry *e = (DiagEntry *)array_get(cap.entries, 1);
    check(w && w->severity == 2, "warning severity");
    check(w && strcmp(w->message, "watch out") == 0, "warning message");
    check(w && w->file && strcmp(w->file, "test.oby") == 0, "recorded file");
    check(w && w->line == 2 && w->column == 1, "line and column");
    check(w && w->length == 1, "token length");
    check(e && e->severity == 1, "error severity");
    check(e && strcmp(e->message, "bad 7") == 0, "error message");

    diag_free(&cap);
    check(cap.entries == NULL, "entries freed");

    diag_free(&d);   /* safe with nothing recorded */

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("diag capture ok\n");
    return 0;
}
