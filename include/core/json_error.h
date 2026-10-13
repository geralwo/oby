#ifndef OBY_JSON_ERROR_H
#define OBY_JSON_ERROR_H

#include <stddef.h>
#include <stdio.h>

/* json.h's parse error codes rendered for people. The scripts' Json natives
   and the language server are the two reporters of these; the mapping lives
   here so both read the same. The numbers are `json_parse_error_e` from
   vendor/json.h, named in the comments. */

static inline const char *oby_json_error_text(size_t code) {
    switch ((int)code) {
        case 0:  return "no error";                                      /* none */
        case 1:  return "expected ',' or a closing bracket";             /* expected_comma_or_closing_bracket */
        case 2:  return "expected ':'";                                  /* expected_colon */
        case 3:  return "expected '\"'";                                 /* expected_opening_quote */
        case 4:  return "invalid escape sequence";                       /* invalid_string_escape_sequence */
        case 5:  return "invalid number";                                /* invalid_number_format */
        case 6:  return "invalid value";                                 /* invalid_value */
        case 7:  return "unexpected end of input";                       /* premature_end_of_buffer */
        case 8:  return "invalid string";                                /* invalid_string */
        case 9:  return "out of memory";                                 /* allocator_failed */
        case 10: return "unexpected characters after the document";      /* unexpected_trailing_characters */
        case 11: return "the document nests too deeply";                 /* recursion */
        case 12: return "unterminated comment";                          /* unterminated_comment */
        default: return "invalid JSON";                                  /* unknown */
    }
}

/* "invalid value at byte 12 (line 1, column 13)" into `buf` — the line and
   column as `json_parse_result_s` reports them, so `column` is 1-based.
   Returns what snprintf would. */
static inline int oby_json_error_message(char *buf, size_t cap, size_t code,
                                         size_t byte, size_t line, size_t column) {
    return snprintf(buf, cap, "%s at byte %zu (line %zu, column %zu)",
                    oby_json_error_text(code), byte, line, column);
}

#endif // OBY_JSON_ERROR_H
