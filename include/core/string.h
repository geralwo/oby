#ifndef OBY_STRING_H
#define OBY_STRING_H

#include <stddef.h>
#include <stdbool.h>
#include "core/array.h"

/*
 * String structure for the OBY VM.
 * Tracks length and hash for efficient VM operations.
 *
 * A String is a built-in primitive, not an Obj; a `Value` holds it under
 * `VAL_STRING`.
 */
typedef struct {
    size_t length;
    size_t hash;
    char  *data;
    Array *view;   /* lazily-created `byte[]` view of `data`; owned by the String */
} String;

/* Lifecycle */
String *string_new(const char *data);
String *string_new_len(const char *data, size_t length);
void    string_free(String *str);

/* Basic Operations */
String *string_concat(const String *a, const String *b);
bool    string_equal(const String *a, const String *b);
size_t  string_hash(const String *str);

/* Interpolation */
/* 
 * Interpolates variables into a format string.
 * 'fmt' is the template (e.g., "Hello ${name}").
 * 'args' is an Array of String pointers representing the scope/variables.
 */
String *string_interpolate(const String *fmt, const Array *args);

#endif // OBY_STRING_H