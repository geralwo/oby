#ifndef OBY_LSP_QUERY_H
#define OBY_LSP_QUERY_H

#include <stddef.h>

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

#endif // OBY_LSP_QUERY_H
