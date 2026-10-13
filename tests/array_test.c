#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/array.h"

int main() {
    printf("Testing dynamic array implementation...\n");
    
    // Test with integers
    Array *int_array = array_new(sizeof(int));
    if (int_array == NULL) {
        printf("Failed to create array\n");
        return 1;
    }
    
    // Test push
    for (int i = 0; i < 10; i++) {
        if (array_push(int_array, &i) != 0) {
            printf("Failed to push %d\n", i);
            array_destroy(int_array);
            return 1;
        }
    }
    
    printf("Pushed 10 integers. Length: %zu, Capacity: %zu\n", 
           array_length(int_array), array_capacity(int_array));
    
    // Test get
    for (int i = 0; i < 10; i++) {
        int *value = array_get(int_array, i);
        if (*value != i) {
            printf("Value mismatch at index %d: expected %d, got %d\n", i, i, *value);
            array_destroy(int_array);
            return 1;
        }
    }
    printf("Get test passed\n");
    
    // Test insert
    int insert_val = 99;
    if (array_insert(int_array, 5, &insert_val) != 0) {
        printf("Failed to insert at index 5\n");
        array_destroy(int_array);
        return 1;
    }
    
    int *value = array_get(int_array, 5);
    if (*value != 99) {
        printf("Insert test failed: expected 99, got %d\n", *value);
        array_destroy(int_array);
        return 1;
    }
    printf("Insert test passed\n");
    
    // Test pop
    int popped;
    if (array_pop(int_array, &popped)) {
        printf("Popped value: %d\n", popped);
    } else {
        printf("Failed to pop\n");
        array_destroy(int_array);
        return 1;
    }
    
    // Test remove
    if (array_remove(int_array, 0, &popped)) {
        printf("Removed first element: %d\n", popped);
    } else {
        printf("Failed to remove\n");
        array_destroy(int_array);
        return 1;
    }
    
    // Test array_equal with actually equal arrays
    Array *int_array2 = array_new(sizeof(int));
    Array *int_array3 = array_new(sizeof(int));
    
    // Create two identical arrays
    for (int i = 0; i < 5; i++) {
        int val = i * 2;
        array_push(int_array2, &val);
        array_push(int_array3, &val);
    }
    
    if (!array_equal(int_array2, int_array3)) {
        printf("Identical arrays should be equal but aren't\n");
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        return 1;
    }
    printf("Equality test passed\n");
    
    // Test inequality
    array_push(int_array3, &(int){99});  // Make them different
    if (array_equal(int_array2, int_array3)) {
        printf("Different arrays should not be equal but are\n");
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        return 1;
    }
    printf("Inequality test passed\n");
    
    // Test with strings
    Array *str_array = array_new(sizeof(char *));
    char *hello = "Hello";
    char *world = "World";
    
    array_push(str_array, &hello);
    array_push(str_array, &world);
    
    char **str1 = array_get(str_array, 0);
    char **str2 = array_get(str_array, 1);
    
    if (strcmp(*str1, "Hello") != 0 || strcmp(*str2, "World") != 0) {
        printf("String array test failed\n");
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    printf("String array test passed\n");
    
    // Test reserve and shrink_to_fit
    Array *test_array = array_new(sizeof(int));
    for (int i = 0; i < 5; i++) {
        array_push(test_array, &i);
    }
    
    if (array_reserve(test_array, 20) != 0) {
        printf("Reserve failed\n");
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    if (array_capacity(test_array) < 20) {
        printf("Reserve didn't work properly\n");
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    printf("Reserve test passed. Capacity: %zu\n", array_capacity(test_array));
    
    if (array_shrink_to_fit(test_array) != 0) {
        printf("Shrink to fit failed\n");
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    if (array_capacity(test_array) != array_length(test_array)) {
        printf("Shrink to fit didn't work properly\n");
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    printf("Shrink to fit test passed. Final capacity: %zu\n", array_capacity(test_array));
    
    // Test copy and clone
    Array *original = array_new(sizeof(int));
    for (int i = 0; i < 3; i++) {
        array_push(original, &i);
    }
    
    Array *copy = array_new(sizeof(int));
    if (array_copy(copy, original) != 0) {
        printf("Copy failed\n");
        array_destroy(original);
        array_destroy(copy);
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    if (!array_equal(original, copy)) {
        printf("Copy didn't work properly\n");
        array_destroy(original);
        array_destroy(copy);
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    printf("Copy test passed\n");
    
    Array *clone = array_clone(original);
    if (clone == NULL) {
        printf("Clone failed\n");
        array_destroy(original);
        array_destroy(copy);
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    
    if (!array_equal(original, clone)) {
        printf("Clone didn't work properly\n");
        array_destroy(original);
        array_destroy(copy);
        array_destroy(clone);
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    printf("Clone test passed\n");
    
    // Test swap
    int a = 100, b = 200;
    array_push(original, &a);
    array_push(original, &b);
    
    array_swap(original, 3, 4);  // Swap the last two elements
    
    int *val1 = array_get(original, 3);
    int *val2 = array_get(original, 4);
    
    if (*val1 != 200 || *val2 != 100) {
        printf("Swap failed: expected 200,100 got %d,%d\n", *val1, *val2);
        array_destroy(original);
        array_destroy(copy);
        array_destroy(clone);
        array_destroy(test_array);
        array_destroy(int_array);
        array_destroy(int_array2);
        array_destroy(int_array3);
        array_destroy(str_array);
        return 1;
    }
    printf("Swap test passed\n");
    
    // Test iterator
    printf("Testing iterator...\n");
    ArrayIterator it;
    array_iter_init(&it, original);
    int *val;
    int count = 0;
    while ((val = (int *)array_next(&it)) != NULL) {
        printf("Iterator element %d: %d\n", count++, *val);
    }
    printf("Iterator test passed\n");
    
    // Cleanup
    array_destroy(original);
    array_destroy(copy);
    array_destroy(clone);
    array_destroy(test_array);
    array_destroy(int_array);
    array_destroy(int_array2);
    array_destroy(int_array3);
    array_destroy(str_array);
    
    printf("All tests passed!\n");
    return 0;
}