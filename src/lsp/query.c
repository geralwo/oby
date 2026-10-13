#include <string.h>

#include "lsp/query.h"

size_t lsp_line_start(const char *source, size_t source_len, int line) {
    if (line <= 0) return 0;
    size_t offset = 0;
    for (int current = 0; current < line && offset < source_len; current++) {
        while (offset < source_len && source[offset] != '\n') offset++;
        if (offset < source_len) offset++;   /* past the '\n' */
    }
    return offset;
}

/* Where a line's content ends: at its '\n' or a preceding '\r' (a CRLF
   terminator is not part of the line — positions cannot denote it). */
static size_t line_content_end(const char *source, size_t source_len, size_t start) {
    size_t end = start;
    while (end < source_len && source[end] != '\n' && source[end] != '\r') end++;
    return end;
}

/* UTF-16 code units in `source[start..stop)`. A malformed byte counts one, so
   the result stays monotone on broken text. */
static int utf16_units(const char *source, size_t start, size_t stop) {
    int units = 0;
    for (size_t i = start; i < stop; i++) {
        unsigned char c = (unsigned char)source[i];
        if ((c & 0xC0) == 0x80) continue;                   /* continuation */
        units += (c & 0xF8) == 0xF0 ? 2 : 1;                /* 4-byte lead: a pair */
    }
    return units;
}

LspPosition lsp_position_of(const char *source, size_t source_len,
                            size_t offset, const char *encoding) {
    if (offset > source_len) offset = source_len;

    int line = 0;
    size_t start = 0;
    for (size_t i = 0; i < offset; i++) {
        if (source[i] == '\n') {
            line++;
            start = i + 1;
        }
    }

    size_t content_end = line_content_end(source, source_len, start);
    size_t stop = offset < content_end ? offset : content_end;

    LspPosition p;
    p.line = line;
    p.character = strcmp(encoding, "utf-8") == 0
                      ? (int)(stop - start)
                      : utf16_units(source, start, stop);
    return p;
}

size_t lsp_offset_of(const char *source, size_t source_len,
                     LspPosition position, const char *encoding) {
    size_t start = lsp_line_start(source, source_len, position.line);
    if (position.character <= 0) return start;
    size_t content_end = line_content_end(source, source_len, start);

    if (strcmp(encoding, "utf-8") == 0) {
        size_t offset = start + (size_t)position.character;
        return offset < content_end ? offset : content_end;
    }

    /* UTF-16: walk whole characters, stopping on the last boundary that does
       not pass the requested count. */
    int units = 0;
    size_t offset = start;
    while (offset < content_end) {
        unsigned char c = (unsigned char)source[offset];
        int width = (c & 0xC0) == 0x80 ? 0 : ((c & 0xF8) == 0xF0 ? 2 : 1);
        if (units + width > position.character) break;
        units += width;
        offset++;
        while (offset < content_end && ((unsigned char)source[offset] & 0xC0) == 0x80) {
            offset++;   /* the rest of this character */
        }
    }
    return offset;
}
