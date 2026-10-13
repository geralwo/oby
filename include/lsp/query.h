#ifndef OBY_LSP_QUERY_H
#define OBY_LSP_QUERY_H

#include <stddef.h>

#include "compiler/ast.h"
#include "compiler/lexer.h"
#include "compiler/type.h"

/* A zero-based position: `character` counts from the start of the line, in the
   negotiated position encoding — bytes for "utf-8", UTF-16 code units for the
   protocol default "utf-16" (see `lsp/json.h`'s neighbors, §4.1 of the plan). */
typedef struct {
    int line;
    int character;
} LspPosition;

typedef struct {
    LspPosition start;   /* inclusive */
    LspPosition end;     /* exclusive */
} LspRange;

/* The byte offset at which line `line` (0-based) starts. A line past the end
   of the text maps to the end of the text. */
size_t lsp_line_start(const char *source, size_t source_len, int line);

/* The position of byte `offset`. A character past the line's content clamps
   to the line's end (the protocol requires it); an offset inside a multi-byte
   character or a surrogate pair rounds down to the character's boundary. */
LspPosition lsp_position_of(const char *source, size_t source_len,
                            size_t offset, const char *encoding);

/* The byte offset of `position` — the reverse, clamped the same way, and
   rounded down when the position falls inside a character. */
size_t lsp_offset_of(const char *source, size_t source_len,
                     LspPosition position, const char *encoding);

/* What sits under the cursor: the innermost node whose token covers the byte
   `offset`, with the symbol and type sema recorded for it where it did. */
typedef struct {
    ASTNode *node;
    Symbol  *sym;
    Type    *type;
} LspTarget;

/* Tokens from other files (imports, cloned generics) never match `source`, so
   a target is always within the document being asked about. */
LspTarget lsp_target_at(ASTNode *program, const char *source, size_t source_len,
                        size_t offset);

/* The declaration `target` points at — `sym->decl`, then `sym->decl_tok`, then
   the type's `decl_tok`. False when nothing is recorded. */
bool lsp_target_decl_token(const LspTarget *target, Token *out);

/* True when `tok`'s text lies inside `source[0..source_len)` — the same-file
   test that keeps a definition from jumping into another file. */
bool lsp_token_in_source(const char *source, size_t source_len, Token tok);

/* What the text just before the cursor asks to complete. */
typedef enum {
    LSP_COMPLETE_NAME,     /* a bare identifier: keywords, types, values */
    LSP_COMPLETE_MEMBER,   /* `expr.` — the expression's members */
    LSP_COMPLETE_STATIC    /* `Type::` — the type's statics */
} LspCompletionKind;

typedef struct {
    LspCompletionKind kind;
    char prefix[64];       /* the partial identifier being typed (possibly empty) */
    size_t prefix_len;
    size_t receiver_end;   /* where the receiver's text ends (the `.`/`::` offset) */
    char receiver[128];    /* the name before the `.`/`::` (possibly empty) */
    size_t receiver_len;
} LspCompletion;

/* Read the text before `offset` and decide what is being completed. */
LspCompletion lsp_completion_at(const char *source, size_t source_len, size_t offset);

/* The bindings a bare name at `offset` can mean beyond the globals: the
   enclosing function's parameters and the binds written before the cursor (or
   the top-level binds before it). Appends `Symbol*`, in declaration order. */
void lsp_visible_bindings(ASTNode *program, const char *source, size_t source_len,
                          size_t offset, Array *out);

#endif // OBY_LSP_QUERY_H
