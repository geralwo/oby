# The `extern` Keyword in OBY

`extern` declares a function whose implementation is **native** — written in C
rather than OBY. It is how a script reaches the outside world (files,
terminals, allocation, and so on).

## Syntax

```
extern NAME(param, ...) [-> type]
```

- `NAME` is the **C symbol**: the key the function is registered under in the
  native registry.
- The parameter list and optional return type describe the OBY-facing
  signature. Omitting `-> type` means the function returns `void`.
- `extern` may appear in three positions:

| Position | Example | Binds |
|----------|---------|-------|
| top level | `extern native_add(int a, int b) -> int` | the name `native_add` |
| assignment | `write = extern write(int fd, byte[] buf, uint count) -> int` | the name `write` |
| member | `std::write = extern write(int fd, byte[] buf, uint count) -> int` | `std::write` |

The OBY name (left of `=`) and the C symbol (after `extern`) are independent,
so one native can back several names:

```
int::to_str = extern int_to_str(int n) -> String
int_to_text = extern int_to_str(int n) -> String
```

## Resolution

`extern` names are resolved **at compile time** against the native registry. If
the C symbol is not registered, compilation fails:

```
script.oby:1:1: error: unknown native 'no_such_native'
```

A typo is therefore caught before the program ever runs.

## Writing a native in C

Every native has a uniform signature — it receives tagged values and returns
one:

```c
typedef Value (*NativeFn)(Value *args, int argc);
```

A `Value` carries a tag and a payload (`include/core/value.h`):

```c
typedef enum {
    VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT,
    VAL_STRING, VAL_ARRAY, VAL_OBJ,
} ValueTag;

typedef struct {
    ValueTag tag;
    union { bool b; int64_t i; double f; String *str; Array *arr; Obj *obj; } as;
} Value;
```

`String` and `Array` are built-in primitives with their own tags (`VAL_STRING`,
`VAL_ARRAY`); `VAL_OBJ` is for dynamic/nominal `{}` objects.

So a native can inspect a value's type. The static signature is still checked
at compile time; the tag is what makes dynamic features and a generic
marshaller possible. Build values with `val_int`, `val_float`, `val_string`,
`val_array`, `val_obj`, and so on.

Implementations live in a domain file under `src/native/` (for example
`src/native/io.c`) and expose a registration hook:

```c
/* src/native/io.c */
#include <unistd.h>
#include "native/io.h"
#include "native/native.h"

static Value nat_write(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    Array *buf = args[1].as.arr;
    size_t count = (size_t)args[2].as.i;
    return val_int((int64_t)write(fd, buf ? buf->buffer : NULL, count));
}

void io_register_natives(void) {
    native_add("write", nat_write);   /* "write" is the C symbol */
}
```

Register the domain once, from `native_register_std()` in
`src/native/native.c`:

```c
void native_register_std(void) {
    native_init();
    io_register_natives();
}
```

Then add the source file to `.sources` and declare the function in the prelude
so scripts can see it:

```
write = extern write(int fd, byte[] buf, uint count) -> int
```

## Example

With the native registered and declared, a script calls it like any other
function:

```
write(1, "hello\n", 6)
```

- `1` is stdout.
- `"hello\n"` arrives as a `VAL_STRING` value (a `String`).
- `6` is the byte count.

The call resolves to the registered `"write"` native, which calls POSIX
`write()`.

## Notes

- `native_add` rejects duplicate names, so registration is idempotent.
- `native_register_std()` runs from `sema_init`, so natives are always present
  before any `extern` is resolved.
- Heap values (`String`, `Array`, `{}`) are freed manually — there is no GC.
  `String` and `Array` are built-in primitives (`VAL_STRING`, `VAL_ARRAY`);
  `{}` objects are `Obj`s (`VAL_OBJ`).
