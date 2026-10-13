#ifndef OBY_PARSER_H
#define OBY_PARSER_H

#include "compiler/lexer.h"
#include "compiler/ast.h"
#include "compiler/diag.h"
#include "core/arena.h"

typedef struct {
    Lexer *lexer;
    Token current;
    Token previous;
    bool had_error;
    bool quiet;      /* speculative parse: suppress diagnostics */
    Arena *arena;
    Diagnostics *diag;
    int  type_arg_depth;   /* >0 while parsing a `Name<...>` argument list */
    bool pending_gt;       /* a consumed `>>` still owes a `>` to an enclosing list */
} Parser;

void parser_init(Parser *p, Lexer *lexer, Arena *arena, Diagnostics *diag);
ASTNode *parse_program(Parser *p);

#endif // OBY_PARSER_H