#ifndef OBY_ARRAY_H
#define OBY_ARRAY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Array flags */
#define ARRAY_F_FIXED     (1u << 0)  /* Fixed-size: cannot grow or shrink */
#define ARRAY_F_OWNED     (1u << 1)  /* Owns the buffer (can free it) */
#define ARRAY_F_ZEROED    (1u << 2)  /* Buffer is zero-initialized */

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
Array  *array_new_fixed(size_t size_of_element, size_t length);
void    array_free(Array *arr);
void    array_destroy(Array *arr);

/* Capacity */
size_t  array_capacity(const Array *arr);
size_t  array_length(const Array *arr);
bool    array_is_empty(const Array *arr);
int     array_reserve(Array *arr, size_t new_capacity);
int     array_shrink_to_fit(Array *arr);

/* Element access */
void   *array_get(const Array *arr, size_t index);
void   *array_at(const Array *arr, size_t index);
void   *array_front(const Array *arr);
void   *array_back(const Array *arr);
void   *array_data(const Array *arr);

/* Element mutation */
int     array_set(Array *arr, size_t index, const void *value);
int     array_push(Array *arr, const void *value);
bool    array_pop(Array *arr, void *out);
int     array_insert(Array *arr, size_t index, const void *value);
bool    array_remove(Array *arr, size_t index, void *out);
void    array_clear(Array *arr);

/* Bulk operations */
int     array_append(Array *arr, const void *values, size_t count);
int     array_copy(Array *dest, const Array *src);
Array  *array_clone(const Array *src);

/* Comparison */
bool    array_equal(const Array *a, const Array *b);

/* Internal / VM utilities */
void    array_swap(Array *arr, size_t i, size_t j);
void    array_set_length(Array *arr, size_t new_length);

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

/* Helper macros for pushing common types */
#define array_push_int(arr, val) do { int v = (val); array_push(arr, &v); } while (0)
#define array_push_ptr(arr, val) do { void *v = (val); array_push(arr, &v); } while (0)

#endif  // OBY_ARRAY_H
