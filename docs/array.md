# Array Primitive in OBY

The `Array` type in OBY is a dynamic, generic array primitive designed for the virtual machine. It provides a flexible way to store and manipulate collections of data.

## Structure

The `Array` struct is defined in `include/core/array.h`:

```c
typedef struct {
    unsigned int  flags;
    size_t        length;
    size_t        capacity;
    size_t        size_of_element;
    void         *buffer;
} Array;
```

### Fields
- **`flags`**: Bitmask for array behavior (e.g., `ARRAY_F_FIXED`, `ARRAY_F_OWNED`).
- **`length`**: The number of elements currently stored in the array.
- **`capacity`**: The total number of elements the array can hold before needing to reallocate.
- **`size_of_element`**: The size (in bytes) of each element. This allows the array to be generic, storing any data type.
- **`buffer`**: A heap-allocated pointer to the raw data.

## Design Philosophy

1.  **Genericity**: By storing `size_of_element`, the array can handle any data type (integers, pointers, custom structs) without needing to know the type at compile time.
2.  **Efficiency**: The array uses an amortized $O(1)$ growth strategy (doubling capacity when full), similar to `std::vector` in C++ or `list` in Python.
3.  **Memory Safety**: The array manages its own memory buffer. When an array is destroyed, it automatically frees its internal buffer if the `ARRAY_F_OWNED` flag is set.

## Usage

### Lifecycle
- **Creation**: Use `array_new(size_of_element)` to create a new, empty array.
- **Destruction**: Use `array_destroy()` to free both the array struct and its internal buffer.

### Manipulation
- **Adding**: Use `array_push()` to add an element to the end.
- **Accessing**: Use `array_get()` to retrieve a pointer to an element at a specific index.
- **Iteration**: For optimal performance and safety, use the `array_foreach` macro:
  ```c
  array_foreach(my_array, int, val, it) {
      printf("Element: %d\n", *val);
  }
  ```

### Bulk Operations
- **Appending**: `array_append()` allows adding multiple elements at once.
- **Copying/Cloning**: `array_copy()` and `array_clone()` provide efficient ways to duplicate arrays.

## Future Considerations
- **GC Integration**: Arrays are a built-in primitive (`VAL_ARRAY`), not an `Obj`. If a GC is added, it will need to track them separately.
- **Slicing**: Future updates may include support for views/slices into existing arrays to avoid unnecessary copies.
