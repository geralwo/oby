#ifndef OBY_UTIL_H
#define OBY_UTIL_H

#include <stdbool.h>
#include <stddef.h>

char* read_file(const char* path);

/* True when `c` is a digit of `base` (2, 10, or 16). */
bool digit_in_base(int base, char c);

/* Drop digit separators (`_`) from `text` in place. With a nonzero `base` a
   separator is only valid between two digits of that base, and a misplaced one
   returns false (the text is still cleaned, so the caller must treat it as a
   non-number). `base` 0 skips the check, for text whose separators the lexer
   has already validated. */
bool strip_digit_separators(char *text, int base);

/* Parse an integer literal's text: `0x` hex, `0b` binary, else decimal.
   Returns false when the value does not fit 64 bits. */
bool parse_int_literal(const char *text, unsigned long long *out);

#endif // OBY_UTIL_H
