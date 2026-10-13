#include <stdlib.h>
#include <string.h>
#include "core/string.h"

static size_t calculate_hash(const char *data, size_t length) {
    size_t hash = 5381;
    for (size_t i = 0; i < length; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }
    return hash;
}

String *string_new_len(const char *data, size_t length) {
    String *str = (String *)malloc(sizeof(String));
    if (!str) return NULL;

    str->data = (char *)malloc(length + 1);
    if (!str->data) {
        free(str);
        return NULL;
    }

    memcpy(str->data, data, length);
    str->data[length] = '\0';
    str->length = length;
    str->hash = calculate_hash(data, length);
    str->view = NULL;
    return str;
}

String *string_new(const char *data) {
    return string_new_len(data, strlen(data));
}

void string_free(String *str) {
    if (str) {
        /* The view shares `data`; free the view struct, not its buffer. */
        if (str->view) array_free(str->view);
        free(str->data);
        free(str);
    }
}

String *string_concat(const String *a, const String *b) {
    size_t new_len = a->length + b->length;
    char *new_data = (char *)malloc(new_len + 1);
    if (!new_data) return NULL;

    memcpy(new_data, a->data, a->length);
    memcpy(new_data + a->length, b->data, b->length);
    new_data[new_len] = '\0';

    String *res = string_new_len(new_data, new_len);
    free(new_data);
    return res;
}

bool string_equal(const String *a, const String *b) {
    if (a == b) return true;
    if (a->length != b->length) return false;
    if (a->hash != b->hash) return false;
    return memcmp(a->data, b->data, a->length) == 0;
}

size_t string_hash(const String *str) {
    return str->hash;
}

String *string_interpolate(const String *fmt, const Array *args) {
    (void)fmt;
    (void)args;
    return NULL; 
}