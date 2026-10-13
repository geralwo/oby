# Object model in oby

A `{}` object (dynamic or nominal) is a heap block:

```c
typedef struct Object {
    Obj      base;   /* kind = OBJ_OBJECT */
    size_t   size;   /* bytes of field storage */
    uint8_t *data;
} Object;
```

- **`size`** is the total bytes of field storage, fixed when the object is
  created. The compiler lays every member out at a byte offset sized from its
  type, so the layout is known statically.
- **`data`** is `calloc`'d, so every field starts zeroed: a packed field is `0`,
  a tagged field is `null`.

Member access is by byte offset, so `obj.member` compiles to a sized `memcpy`
(`GETFIELD`) or store (`SETFIELD`) rather than a name lookup. `NEWOBJ` allocates
an object with a given byte size.

**Fields are byte-tight.** A packed member (`vec2i`, `int`, ...) is stored inline
with no boxing; a tagged member (`String`, array, nested object) takes a 16-byte
`Value` slot. `player.pos.x = 9` writes four bytes and leaves `pos.y` alone.

A `Value` holds an object through `VAL_OBJ`. Objects are freed manually
(`object_free`); there is no GC.

`String` and `Array` are **not** `Object`s: they are built-in primitives with
their own tags, and their intrinsic fields (`buffer`, `length`) are addressed by
index and computed by the VM.

## Not yet

- A runtime **type descriptor** — the layout is static today, but reflection or
  dynamic member lookup would need the object to name its type.
- **Members added after allocation** — `obj.member = v` extends the *type* (the
  byte offset is assigned at compile time), so a value of that type created
  before the addition would not have the field.
