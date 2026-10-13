#ifndef OBY_ARRAY_H
#define OBY_ARRAY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

/* Array flags */
#define ARRAY_F_FIXED     (1u << 0)  /* Fixed-size: cannot grow or shrink */
#define ARRAY_F_OWNED     (1u << 1)  /* Owns the buffer (can free it) */

/* Array structure */
typedef struct {
    unsigned int  flags;
    size_t        length;
    size_t        capacity;
    size_t        size_of_element;
    void         *buffer;
} Array;

/* Construction / destruction */
void    array_init(Array *arr, size_t size_of_element);
Array  *array_new(size_t size_of_element);
void    array_free(Array *arr);
void    array_destroy(Array *arr);

/* Capacity */
size_t  array_capacity(const Array *arr);
size_t  array_length(const Array *arr);
int     array_reserve(Array *arr, size_t new_capacity);
int     array_shrink_to_fit(Array *arr);

/* Set the logical length (`new_length` must be <= capacity). */
static inline void array_set_length(Array *arr, size_t new_length) {
    assert(arr != NULL);
    assert(new_length <= arr->capacity);
    arr->length = new_length;
}

/* Element access */
void   *array_get(const Array *arr, size_t index);
void   *array_back(const Array *arr);
void   *array_data(const Array *arr);

/* Element mutation */
int     array_set(Array *arr, size_t index, const void *value);
int     array_push(Array *arr, const void *value);
bool    array_pop(Array *arr, void *out);
int     array_insert(Array *arr, size_t index, const void *value);
bool    array_remove(Array *arr, size_t index, void *out);

/* Bulk operations */
int     array_append(Array *arr, const void *values, size_t count);
int     array_copy(Array *dest, const Array *src);
Array  *array_clone(const Array *src);

/* Comparison */
bool    array_equal(const Array *a, const Array *b);

/* Internal / VM utilities */
void    array_swap(Array *arr, size_t i, size_t j);

/* Iterator structure */
typedef struct {
    const Array *arr;
    size_t current_index;
} ArrayIterator;

void array_iter_init(ArrayIterator *it, const Array *arr);
void *array_next(ArrayIterator *it);

/* Iterate: `array_foreach(arr, T, var, it) { ... }` binds `var` to a `T *`,
   NULL at the end. */
#define array_foreach(arr, T, var, it) \
    for (array_iter_init(&(it), (arr)); \
         ((var) = (T *)array_next(&(it))) != NULL; )

#endif  // OBY_ARRAY_H
