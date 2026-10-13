# String Primitive in OBY

The `String` type in OBY is designed as a core primitive for the virtual machine. Unlike standard C strings (`char*`), OBY strings are structured to be efficient for VM operations like dictionary lookups, concatenation, and interpolation.

## Structure

The `String` struct is defined in `include/core/string.h`:

```c
typedef struct {
    size_t length;
    size_t hash;
    char  *data;
    Array *view;   /* lazily-created `byte[]` view of `data`; owned by the String */
} String;
```

### Fields
- **`length`**: Stores the exact length of the string. This allows OBY to support strings containing null bytes (`\0`) and provides $O(1)$ access to string length.
- **`hash`**: A precomputed hash value (using the DJB2 algorithm). This is critical for performance, as it allows the VM to compare strings or perform hash table lookups in $O(1)$ time after the initial hash check.
- **`data`**: A heap-allocated, null-terminated character buffer.
- **`view`**: The `byte[]` view handed out for `s.buffer`, created on first use and freed with the string. It shares `data` and is not owned by the caller.

## Design Philosophy

1.  **Immutability**: OBY strings are intended to be treated as immutable. Once created, they should not be modified. This simplifies memory management and allows for future optimizations like string interning.
2.  **Performance**: By storing the length and hash, we avoid repeated `strlen()` or re-hashing calls during VM execution.
3.  **Ownership**: The `String` struct owns its `data` buffer. When a `String` is destroyed, its buffer is automatically freed.

## Usage

### Lifecycle
- **Creation**: Use `string_new()` for standard strings or `string_new_len()` if you have a specific length (useful for parsing).
- **Destruction**: Always call `string_free()` to prevent memory leaks.

### Interpolation
The compiler expands `${expr}` at codegen time: each part is converted with the
`to_str` native and joined with `str_concat`. `string_interpolate()` in
`src/core/string.c` is dead code kept for reference.

## Future Considerations
- **Interning**: To save memory and speed up comparisons, OBY may implement a global string pool where identical string literals are shared.
- **GC Integration**: Strings are a built-in primitive (`VAL_STRING`), not an `Obj`. If a GC is added, it will need to track them separately.
