#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "core/array.h"

void array_init(Array *arr, size_t size_of_element) {
    assert(arr != NULL);
    arr->flags = 0;
    arr->length = 0;
    arr->capacity = 0;
    arr->size_of_element = size_of_element;
    arr->buffer = NULL;
}

Array *array_new(size_t size_of_element) {
    Array *arr = (Array *)malloc(sizeof(Array));
    if (arr == NULL) {
        return NULL;
    }
    array_init(arr, size_of_element);
    arr->flags |= ARRAY_F_OWNED;
    return arr;
}

void array_free(Array *arr) {
    if (arr != NULL && (arr->flags & ARRAY_F_OWNED)) {
        free(arr->buffer);
    }
    free(arr);
}

void array_destroy(Array *arr) {
    array_free(arr);
}

size_t array_capacity(const Array *arr) {
    return arr ? arr->capacity : 0;
}

size_t array_length(const Array *arr) {
    return arr ? arr->length : 0;
}

int array_reserve(Array *arr, size_t new_capacity) {
    assert(arr != NULL);
    if (new_capacity < arr->capacity) {
        if (new_capacity == arr->length) {
            void *new_buffer = realloc(arr->buffer, new_capacity * arr->size_of_element);
            if (new_buffer != NULL || new_capacity == 0) {
                arr->buffer = new_buffer;
                arr->capacity = new_capacity;
            }
        }
        return 0;  /* Already sufficient capacity */
    }
    
    if (arr->flags & ARRAY_F_FIXED) {
        return -1;  /* Cannot resize fixed-size arrays */
    }
    
    size_t new_size = new_capacity * arr->size_of_element;
    void *new_buffer = realloc(arr->buffer, new_size);
    if (new_buffer == NULL) {
        return -1;  /* Allocation failed */
    }
    
    arr->buffer = new_buffer;
    arr->capacity = new_capacity;
    return 0;
}

int array_shrink_to_fit(Array *arr) {
    assert(arr != NULL);
    if (arr->length == 0) {
        free(arr->buffer);
        arr->buffer = NULL;
        arr->capacity = 0;
        return 0;
    }
    
    if (arr->flags & ARRAY_F_FIXED) {
        return 0;  /* Cannot shrink fixed-size arrays */
    }
    
    return array_reserve(arr, arr->length);
}

void *array_get(const Array *arr, size_t index) {
    assert(arr != NULL);
    assert(index < arr->length);
    return (char *)arr->buffer + (index * arr->size_of_element);
}

void *array_back(const Array *arr) {
    if (arr->length == 0) {
        return NULL;
    }
    return array_get(arr, arr->length - 1);
}

void *array_data(const Array *arr) {
    return arr->buffer;
}

int array_set(Array *arr, size_t index, const void *value) {
    assert(arr != NULL);
    assert(index < arr->length);
    memcpy((char *)arr->buffer + (index * arr->size_of_element), value, arr->size_of_element);
    return 0;
}

int array_push(Array *arr, const void *value) {
    assert(arr != NULL);
    if (arr->length == arr->capacity) {
        if (arr->flags & ARRAY_F_FIXED) {
            return -1;  /* Cannot grow fixed-size arrays */
        }
        size_t new_capacity = arr->capacity ? arr->capacity * 2 : 4;
        if (array_reserve(arr, new_capacity) != 0) {
            return -1;  /* Allocation failed */
        }
    }
    
    memcpy((char *)arr->buffer + (arr->length * arr->size_of_element), value, arr->size_of_element);
    arr->length++;
    return 0;
}

bool array_pop(Array *arr, void *out) {
    assert(arr != NULL);
    if (arr->length == 0) {
        return false;
    }
    arr->length--;
    if (out != NULL) {
        memcpy(out, (char *)arr->buffer + (arr->length * arr->size_of_element), arr->size_of_element);
    }
    return true;
}

int array_insert(Array *arr, size_t index, const void *value) {
    assert(arr != NULL);
    assert(index <= arr->length);
    
    if (arr->length == arr->capacity) {
        size_t new_capacity = arr->capacity ? arr->capacity * 2 : 4;
        if (arr->flags & ARRAY_F_FIXED) {
            return -1;  /* Cannot grow fixed-size arrays */
        }
        if (array_reserve(arr, new_capacity) != 0) {
            return -1;  /* Allocation failed */
        }
    }
    
    if (index < arr->length) {
        memmove((char *)arr->buffer + ((index + 1) * arr->size_of_element),
                (char *)arr->buffer + (index * arr->size_of_element),
                (arr->length - index) * arr->size_of_element);
    }
    
    memcpy((char *)arr->buffer + (index * arr->size_of_element), value, arr->size_of_element);
    arr->length++;
    return 0;
}

bool array_remove(Array *arr, size_t index, void *out) {
    assert(arr != NULL);
    assert(index < arr->length);
    
    if (out != NULL) {
        memcpy(out, (char *)arr->buffer + (index * arr->size_of_element), arr->size_of_element);
    }
    
    if (index < arr->length - 1) {
        memmove((char *)arr->buffer + (index * arr->size_of_element),
                (char *)arr->buffer + ((index + 1) * arr->size_of_element),
                (arr->length - index - 1) * arr->size_of_element);
    }
    
    arr->length--;
    return true;
}

int array_append(Array *arr, const void *values, size_t count) {
    assert(arr != NULL);
    assert(values != NULL);
    
    if (arr->length + count > arr->capacity) {
        size_t needed = arr->length + count;
        size_t new_capacity = arr->capacity ? arr->capacity * 2 : 4;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        if (arr->flags & ARRAY_F_FIXED) {
            return -1;  /* Cannot grow fixed-size arrays */
        }
        if (array_reserve(arr, new_capacity) != 0) {
            return -1;  /* Allocation failed */
        }
    }
    
    memcpy((char *)arr->buffer + (arr->length * arr->size_of_element), values, count * arr->size_of_element);
    arr->length += count;
    return 0;
}

int array_copy(Array *dest, const Array *src) {
    assert(dest != NULL);
    assert(src != NULL);
    assert(dest->size_of_element == src->size_of_element);
    
    if (array_reserve(dest, src->length) != 0) {
        return -1;  /* Allocation failed */
    }
    
    memcpy(dest->buffer, src->buffer, src->length * src->size_of_element);
    dest->length = src->length;
    return 0;
}

Array *array_clone(const Array *src) {
    assert(src != NULL);
    Array *clone = array_new(src->size_of_element);
    if (clone == NULL) {
        return NULL;
    }
    if (array_copy(clone, src) != 0) {
        array_destroy(clone);
        return NULL;
    }
    return clone;
}

bool array_equal(const Array *a, const Array *b) {
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->size_of_element != b->size_of_element) {
        return false;
    }
    if (a->length != b->length) {
        return false;
    }
    return memcmp(a->buffer, b->buffer, a->length * a->size_of_element) == 0;
}

void array_swap(Array *arr, size_t i, size_t j) {
    assert(arr != NULL);
    assert(i < arr->length);
    assert(j < arr->length);
    
    if (i == j) {
        return;
    }
    
    char *temp = malloc(arr->size_of_element);
    if (temp == NULL) {
        return;  /* Allocation failed, but we can't return an error from void function */
    }
    
    void *elem_i = (char *)arr->buffer + (i * arr->size_of_element);
    void *elem_j = (char *)arr->buffer + (j * arr->size_of_element);
    
    memcpy(temp, elem_i, arr->size_of_element);
    memcpy(elem_i, elem_j, arr->size_of_element);
    memcpy(elem_j, temp, arr->size_of_element);
    
    free(temp);
}

void array_iter_init(ArrayIterator *it, const Array *arr) {
    it->arr = arr;
    it->current_index = 0;
}

void *array_next(ArrayIterator *it) {
    if (it->current_index >= it->arr->length) {
        return NULL;
    }
    return array_get(it->arr, it->current_index++);
}