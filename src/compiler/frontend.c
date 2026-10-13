#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/frontend.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

void frontend_init(Frontend *fe) {
    memset(fe, 0, sizeof *fe);
    diag_init(&fe->diag);
}

bool frontend_run(Frontend *fe, const char *label, char *source) {
    assert(fe->source == NULL);

    fe->source = source;
    diag_set_source(&fe->diag, label, source);
    arena_init(&fe->arena);

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer, &fe->arena, &fe->diag);
    fe->program = parse_program(&parser);

    if (parser.had_error) return false;

    sema_init(&fe->sema, &fe->arena, &fe->diag);
    sema_check(&fe->sema, fe->program, label);
    return fe->diag.error_count == 0;
}

void frontend_free(Frontend *fe) {
    arena_free(&fe->arena);
    diag_free(&fe->diag);
    free(fe->source);
    frontend_init(fe);
}
