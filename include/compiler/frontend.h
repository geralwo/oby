#ifndef OBY_FRONTEND_H
#define OBY_FRONTEND_H

#include <stdbool.h>

#include "compiler/ast.h"
#include "compiler/diag.h"
#include "compiler/sema.h"
#include "core/arena.h"

/* One compilation in progress: the frontend's arena, diagnostics, and the
   checked program. Everything the frontend allocated lives in the arena. */
typedef struct {
    Diagnostics diag;
    Arena       arena;
    Sema        sema;
    ASTNode    *program;
    char       *source;  /* owned */
} Frontend;

/* A zeroed frontend, ready for `frontend_run`. */
void frontend_init(Frontend *fe);

/* Parse and typecheck `source`, which is named `label` in diagnostics and is
   the base `use` resolves against. Takes ownership of `source`. Returns false
   when any diagnostic was an error. Call `frontend_free` before running a
   frontend again. */
bool frontend_run(Frontend *fe, const char *label, char *source);

/* Release everything and return to the state `frontend_init` leaves. */
void frontend_free(Frontend *fe);

#endif // OBY_FRONTEND_H
