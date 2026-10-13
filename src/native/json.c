#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"   /* vendor/json.h — public domain, see vendor/README.md */

#include "core/json_error.h"
#include "core/string.h"
#include "native/native.h"

/* A parsed document is a single allocation (json.h makes one malloc for the
   whole encoding). The handle a script holds is that pointer as an int64 —
   the same opaque-cookie shape `io::open`'s fd has — and `json_free` gives it
   back. */

static struct json_value_s *document_of(int64_t handle) {
    return (struct json_value_s *)(intptr_t)handle;
}

static int64_t handle_of(const struct json_value_s *value) {
    return (int64_t)(intptr_t)value;
}

/* The last parse failure, for `json_error`. */
static bool g_error_valid = false;
static struct json_parse_result_s g_error;

static Value string_value(const char *data, size_t length) {
    String *s = string_new_len(data, length);
    if (!s) return val_null();
    return val_string(s);
}

/* The text of a number, NUL-terminated for strtod. Numbers are short; a huge
   one is refused rather than truncated. */
static bool number_text(const struct json_number_s *number, char *buf, size_t cap) {
    if (!number || number->number_size == 0 || number->number_size >= cap) return false;
    memcpy(buf, number->number, number->number_size);
    buf[number->number_size] = '\0';
    return true;
}

/* The writers count their terminating NUL in `*out_size`; a String wants the
   bytes without it. */
static size_t text_length(const char *text, size_t length) {
    return (length > 0 && text[length - 1] == '\0') ? length - 1 : length;
}

/* json_parse(String text) -> int64 : the document handle, or 0 when the text
   is not JSON (`json_error` explains). */
static Value nat_json_parse(Value *args, int argc) {
    (void)argc;
    if (args[0].tag != VAL_STRING) return val_int(0);
    const String *text = args[0].as.str;

    memset(&g_error, 0, sizeof g_error);
    struct json_value_s *root = json_parse_ex(text->data, text->length,
                                              json_parse_flags_default, NULL, NULL,
                                              &g_error);
    g_error_valid = root == NULL;
    return val_int(handle_of(root));
}

/* json_free(int64 handle) -> void */
static Value nat_json_free(Value *args, int argc) {
    (void)argc;
    free(document_of(args[0].as.i));
    return val_null();
}

/* json_kind(int64 handle) -> int : 1 object, 2 array, 3 string, 4 number,
   5 bool, 6 null; 0 for the handle-0 "no document" case. */
static Value nat_json_kind(Value *args, int argc) {
    (void)argc;
    const struct json_value_s *value = document_of(args[0].as.i);
    if (!value) return val_int(0);
    switch ((int)value->type) {
        case json_type_object: return val_int(1);
        case json_type_array:  return val_int(2);
        case json_type_string: return val_int(3);
        case json_type_number: return val_int(4);
        case json_type_true:
        case json_type_false:  return val_int(5);
        case json_type_null:   return val_int(6);
        default:               return val_int(0);
    }
}

/* json_len(int64 handle) -> int : an object's member count, an array's element
   count, 0 for anything else. */
static Value nat_json_len(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    struct json_object_s *object = value ? json_value_as_object(value) : NULL;
    if (object) return val_int((int64_t)object->length);
    struct json_array_s *array = value ? json_value_as_array(value) : NULL;
    if (array) return val_int((int64_t)array->length);
    return val_int(0);
}

/* json_key_at(int64 handle, int index) -> String : an object member's name,
   "" out of range or off an object. */
static Value nat_json_key_at(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    struct json_object_s *object = value ? json_value_as_object(value) : NULL;
    if (!object || args[1].as.i < 0) return string_value("", 0);

    size_t index = (size_t)args[1].as.i;
    struct json_object_element_s *element = object->start;
    for (size_t i = 0; element && i < index; i++) element = element->next;
    if (!element || !element->name) return string_value("", 0);
    return string_value(element->name->string, element->name->string_size);
}

/* json_at(int64 handle, int index) -> int64 : an array's element or an
   object's value by position, 0 out of range. */
static Value nat_json_at(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    if (!value || args[1].as.i < 0) return val_int(0);
    size_t index = (size_t)args[1].as.i;

    struct json_array_s *array = json_value_as_array(value);
    if (array) {
        struct json_array_element_s *element = array->start;
        for (size_t i = 0; element && i < index; i++) element = element->next;
        return val_int(handle_of(element ? element->value : NULL));
    }

    struct json_object_s *object = json_value_as_object(value);
    if (object) {
        struct json_object_element_s *element = object->start;
        for (size_t i = 0; element && i < index; i++) element = element->next;
        return val_int(handle_of(element ? element->value : NULL));
    }
    return val_int(0);
}

/* json_get(int64 handle, String key) -> int64 : a member's value, or 0. */
static Value nat_json_get(Value *args, int argc) {
    (void)argc;
    if (args[1].tag != VAL_STRING) return val_int(0);
    const String *key = args[1].as.str;
    struct json_value_s *value = document_of(args[0].as.i);
    struct json_object_s *object = value ? json_value_as_object(value) : NULL;
    if (!object) return val_int(0);

    for (struct json_object_element_s *element = object->start; element;
         element = element->next) {
        if (element->name && element->name->string_size == key->length &&
            memcmp(element->name->string, key->data, key->length) == 0)
            return val_int(handle_of(element->value));
    }
    return val_int(0);
}

/* json_as_string(int64 handle) -> String : a string's bytes, a number's text,
   a boolean's word; "" for anything else. */
static Value nat_json_as_string(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    if (!value) return string_value("", 0);
    struct json_string_s *string = json_value_as_string(value);
    if (string) return string_value(string->string, string->string_size);
    struct json_number_s *number = json_value_as_number(value);
    if (number) return string_value(number->number, number->number_size);
    if (json_value_is_true(value)) return string_value("true", 4);
    if (json_value_is_false(value)) return string_value("false", 5);
    return string_value("", 0);
}

/* json_as_int(int64 handle) -> int64 : a number, truncated toward zero; 0 for
   anything else, or a number outside the 64-bit range. */
static Value nat_json_as_int(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    struct json_number_s *number = value ? json_value_as_number(value) : NULL;
    char buf[64];
    if (!number_text(number, buf, sizeof buf)) return val_int(0);
    double d = strtod(buf, NULL);
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) return val_int(0);
    return val_int((int64_t)d);
}

/* json_as_float(int64 handle) -> float : a number, 0.0 otherwise. */
static Value nat_json_as_float(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    struct json_number_s *number = value ? json_value_as_number(value) : NULL;
    char buf[64];
    if (!number_text(number, buf, sizeof buf)) return val_float(0.0);
    return val_float(strtod(buf, NULL));
}

/* json_as_bool(int64 handle) -> int : 1 for true, 0 for anything else. */
static Value nat_json_as_bool(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    return val_int((value && json_value_is_true(value)) ? 1 : 0);
}

/* json_to_minified(int64 handle) -> String */
static Value nat_json_to_minified(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    if (!value) return string_value("", 0);

    size_t length = 0;
    char *text = (char *)json_write_minified(value, &length);
    if (!text) return val_null();
    Value result = string_value(text, text_length(text, length));
    free(text);
    return result;
}

/* json_to_pretty(int64 handle, String indent, String newline) -> String */
static Value nat_json_to_pretty(Value *args, int argc) {
    (void)argc;
    struct json_value_s *value = document_of(args[0].as.i);
    if (!value) return string_value("", 0);

    const String *indent = args[1].tag == VAL_STRING ? args[1].as.str : NULL;
    const String *newline = args[2].tag == VAL_STRING ? args[2].as.str : NULL;
    /* The writer takes NUL-terminated strings (OBY's are). */
    size_t length = 0;
    char *text = (char *)json_write_pretty(value,
                                           indent && indent->data ? indent->data : "\t",
                                           newline && newline->data ? newline->data : "\n",
                                           &length);
    if (!text) return val_null();
    Value result = string_value(text, text_length(text, length));
    free(text);
    return result;
}

/* json_error() -> String : why the last parse failed, or "" when it did not.
   The column counts bytes from 1. */
static Value nat_json_error(Value *args, int argc) {
    (void)argc;
    (void)args;
    if (!g_error_valid) return string_value("", 0);

    char message[160];
    int n = oby_json_error_message(message, sizeof message, g_error.error,
                                   g_error.error_offset, g_error.error_line_no,
                                   g_error.error_row_no + 1);
    if (n < 0) return string_value("", 0);
    size_t length = (size_t)n < sizeof message ? (size_t)n : sizeof message - 1;
    return string_value(message, length);
}

void json_register_natives(void) {
    native_add("json_parse", nat_json_parse);
    native_add("json_free", nat_json_free);
    native_add("json_kind", nat_json_kind);
    native_add("json_len", nat_json_len);
    native_add("json_key_at", nat_json_key_at);
    native_add("json_at", nat_json_at);
    native_add("json_get", nat_json_get);
    native_add("json_as_string", nat_json_as_string);
    native_add("json_as_int", nat_json_as_int);
    native_add("json_as_float", nat_json_as_float);
    native_add("json_as_bool", nat_json_as_bool);
    native_add("json_to_minified", nat_json_to_minified);
    native_add("json_to_pretty", nat_json_to_pretty);
    native_add("json_error", nat_json_error);
}
