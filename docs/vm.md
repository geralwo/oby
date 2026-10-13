# The OBY virtual machine

OBY compiles to bytecode for a small **register-based** VM (`src/vm/`). The
compiler gives every value a byte offset on the stack and a size from its type;
the VM executes 64-bit instructions against a byte-addressed stack.

This document describes the machine as it is implemented today.

## 0. Why this shape

- **Register-based, not stack-based.** Static typing makes allocation easy —
  every expression's type is known before codegen, so a slot per local and
  temporary is mechanical, with no spilling heuristics. `a = b + c` is one
  `ADD a b c` instead of `LOAD b; LOAD c; ADD; STORE a`, and dispatch dominates
  execution cost. Lua 5.4 is register-based and among the fastest dynamic VMs.
- **Typed opcodes.** Static types let the compiler emit `ADD_INT`/`ADD_FLOAT`
  and skip tag dispatch entirely.
- **Byte addressing.** A register is a byte offset, not a `Value` slot, so a
  packed value costs exactly its bytes (`vec3i` is 12, not 16) and packed types
  can live inline with no allocation.
- **64-bit instructions.** Byte offsets need 16 bits, so 32-bit instructions
  cannot hold two offsets plus an opcode. `op:8 | A:16 | B:16 | C:16 | N:8`
  costs 2× code size, which is irrelevant at script scale; the alternative (a
  per-function offset table) would add a table load to every operand.
- **Closures are explicit.** A function literal that uses an enclosing local is
  compiled as a closure: the compiler copies the captured values into a heap
  `Closure` and the body reads them with `GETUPVAL`. There is no implicit
  environment pointer and no GC — a closure is freed like any heap value.

## 1. Values on the stack

A stack value is one of two representations:

| Representation | Types | Size |
|----------------|-------|------|
| **Tagged** | `String`, `Array`, `{}` objects, functions, closures, `bool`, `null` | 16 B (a `Value`) |
| **Packed** | `byte` 1, `int`/`uint`/`float` 4, `int64`/`uint64`/`double` 8 | raw bytes |
| **Packed (user)** | `NAME = packed BASE[N]` | `N × sizeof(BASE)` bytes |

A tagged value is a `Value` (`include/core/value.h`): a tag plus a payload. A
packed value is just its bytes — no tag, no allocation. `vec3i = packed int[3]`
is 12 bytes on the stack; a `String` is 16.

Instructions that touch packed data carry a **`ValueKind`** in their `N` field
(`include/vm/opcode.h`), so the VM never guesses a width:

| Kind | Meaning |
|------|---------|
| `KIND_TAGGED` | a 16-byte `Value` |
| `KIND_BYTE` | 1 byte |
| `KIND_INT32` / `KIND_FLOAT32` | 4 bytes |
| `KIND_INT64` / `KIND_FLOAT64` | 8 bytes |

A user packed type is **not** a scalar: `kind_of` classifies it as
`KIND_TAGGED` so it is byte-copied, never converted or arithmetically mixed.
Member access (`v.x`) and constant indexing (`v[0]`) are pure offset arithmetic
on the value's storage — the compiler folds `index × sizeof(element)` into the
byte offset, so no opcode is needed. Writes to a copy (a global, or a packed
member of an object) are read-modify-written by the compiler.

**Objects** store their fields byte-tight too: `Object` is a `size` plus a
`uint8_t *data` buffer, and the compiler lays each field out at a byte offset
sized from its type. `GETFIELD`/`SETFIELD` are therefore plain `memcpy`s — a
packed field is inline, a tagged field is a 16-byte `Value`. `String` and
`Array` are not `Object`s: their intrinsic fields (`buffer`, `length`) are
addressed by index and computed by the VM.

## 2. Instructions

An instruction is 64 bits:

```
op:8 | A:16 | B:16 | C:16 | N:8
```

`A`, `B`, `C` are register offsets (bytes from the frame base) or small
immediates; `N` is a `ValueKind` or an auxiliary field. `OP_BX` reads a 32-bit
operand from `B+C`; `OP_SBX` sign-extends it for jumps.

| Opcode | Operands | Effect |
|--------|----------|--------|
| `HALT` | — | stop execution |
| `LOADK` | `A Bx N` | `R[A] = K[Bx]`, as kind `N` |
| `LOADNIL` | `A` | `R[A] = null` |
| `ZERO` | `A B` | zero `B` bytes at `R[A]` |
| `MOVE` | `A B` | copy one tagged value |
| `MOVEN` | `A B N` | copy `N` bytes |
| `GETGLOBAL` | `A Bx N` | copy `N` bytes from `G[Bx]` to `R[A]` |
| `SETGLOBAL` | `A Bx N` | copy `N` bytes from `R[A]` to `G[Bx]` |
| `ADD_INT` `SUB_INT` `MUL_INT` `DIV_INT` `MOD_INT` | `A B C N` | integer arithmetic, kind `N`; a zero divisor is a runtime error |
| `ADD_FLOAT` `SUB_FLOAT` `MUL_FLOAT` `DIV_FLOAT` | `A B C N` | float arithmetic, kind `N` |
| `NEG_INT` `NEG_FLOAT` | `A B N` | negate |
| `EQ` `NE` `LT` `GT` `LE` `GE` | `A B C N` | compare, result is a raw `int32` 0/1 |
| `EQPACKED` `NEPACKED` | `A B C N` | compare `N` bytes of packed data, result is a raw `int32` 0/1 |
| `NOT` | `A B N` | logical not, result is a raw `int32` |
| `JMP` | `sBx` | `pc += sBx` |
| `TEST` | `A N` | if `R[A]` is truthy, skip the next instruction |
| `GETFIELD` | `A B C N` | copy `N` bytes from `R[B].field[C]` to `R[A]` |
| `SETFIELD` | `A B C N` | copy `N` bytes from `R[C]` to `R[A].field[B]` |
| `NEWOBJ` | `A Bx` | allocate an object with `Bx` bytes of field storage |
| `NEWARRAY` | `A B C` | allocate an array with element size `B` and capacity `C` |
| `APPEND` | `A B N` | append `N` bytes from `R[B]` to the array at `R[A]` |
| `GETINDEX` | `A B C` | `R[A] = arr(R[B])[R[C]]`; `C` is an `int32` index, bounds-checked |
| `SETINDEX` | `A B C` | `arr(R[A])[R[B]] = R[C]`; `B` is an `int32` index, bounds-checked |
| `ARRAY_POP` | `A B` | `R[B] = pop(arr(R[A]))`; heap arrays only |
| `ARRAY_INSERT` | `A B C` | insert `R[C]` into `arr(R[A])` at `int32` index `R[B]` |
| `ARRAY_REMOVE` | `A B C` | `R[C] = remove(arr(R[A]), int32 index R[B])` |
| `ARRAY_FIXED` | `A` | mark `arr(R[A])` fixed: it can no longer be resized |
| `CALL` | `A B C` | call `R[A]` with `B` arguments |
| `CALLNATIVE` | `A B C` | call native `B` with `C` arguments |
| `RETURN` | `A B` | return `B` bytes from `R[A]` |
| `BOX` | `A B N` | `R[A] = box(kind N, R[B])` |
| `UNBOX` | `A B N` | `R[A] = unbox(kind N, R[B])` |
| `CONV` | `A B C N` | convert `R[B]` from kind `C` to kind `N` |
| `GETUPVAL` | `A B C` | copy `C` bytes from the current closure's upvalue `B` to `R[A]` |
| `CLOSURE` | `A B C` | `R[A] = closure(K[B].fn)`, captures copied from `R[C..]` |

`R(i)` denotes the bytes at `stack + base + i`. Tagged operations read those
bytes as a `Value`; packed operations read them as the kind's C type.

## 3. Frames and calls

A frame is one executing function:

```c
typedef struct {
    Function *fn;
    Closure  *closure;   /* captured state when executing a closure; else NULL */
    uint32_t  ip;
    size_t    base;      /* byte offset of R[0] */
    size_t    ret_base;  /* caller's byte offset for results */
} Frame;
```

Registers are not fixed slots: the compiler allocates each value a byte offset
and a size, and `Function.frame_size` is the high-water mark of the frame.

**Calling convention.** The caller lays out

```
R[a]        the callee (a function or closure `Value`, 16 bytes)
R[a+16..]   the arguments, back to back, each sized by the callee's parameter type
```

then executes `CALL a argc`. The VM pushes a frame with `base = caller_base + a +
16` and `ret_base = caller_base + a`, so the callee's parameters are already in
place as its first registers. `RETURN A B` copies `B` bytes from `R[A]` to
`ret_base` — the result lands where the callee value was — and pops the frame.

**Closures.** A function literal that references an enclosing local is compiled
as a closure body. The compiler gives each captured value a byte range in
`Function.up_off`/`up_size`; `CLOSURE A B C` allocates a `Closure`
(`{ fn, up_total, up }`) and copies the captured values from `R[C..]` according
to those tables, so the copy is offset-for-offset. `Frame.closure` points at the
running closure, and `GETUPVAL A B C` copies `C` bytes from `closure->up +
fn->up_off[B]` into `R[A]`.

Captures are **by value and read-only**: the compiler rejects assigning to a
captured name, and mutable state is shared by capturing a reference value (an
object or array) and mutating that. A closure is a heap value; `object::free`
releases the closure and its captured bytes (not the values they point at).

The stack starts at `VM_STACK_INIT` (4096 bytes) and doubles as needed; at most
`VM_FRAMES_MAX` (256) frames may be active.

## 4. Globals

Globals live in a separate byte-addressed block (`VM.globals`), sized by
`global_size`. The type checker assigns every global and every static member a
byte offset after checking (`assign_global_slots` in `src/compiler/sema.c`), so
`GETGLOBAL`/`SETGLOBAL` are plain sized copies. A value global whose type carries
static members (`io = {}` with `io::write`) owns their storage too.

## 5. The constant pool

Each `Function` owns a pool of `Value` constants (`function_add_const`). `LOADK`
copies a tagged constant as-is, or unboxes a packed constant (an `int` or
`float` literal) into a raw register.

## 6. The native boundary

Natives have a uniform signature:

```c
typedef Value (*NativeFn)(Value *args, int argc);
```

They always see tagged values, so the compiler boxes packed arguments at the call
site (`BOX`) and unboxes a packed result (`UNBOX`). `CALLNATIVE a native argc`
reads `argc` 16-byte slots starting at `R[a]` and writes the result to `R[a]`.

## 7. Objects and fields

`{}` objects store their fields byte-tight (see `docs/obj.md`): the compiler
lays each field out at a byte offset sized from its type, so `GETFIELD` and
`SETFIELD` are plain `memcpy`s — a packed field is inline, a tagged field is a
16-byte `Value`. `String`'s intrinsic fields are special-cased in the VM: field
`0` is a `byte[]` view over its bytes, field `1` is its length (and an array's
field `0` is its length).

## 8. Comparisons and truthiness

Comparisons (`==`, `!=`, `<`, `>`, `<=`, `>=`) have type `int` in OBY, so the VM
writes a raw `int32` 0/1. `TEST` and `NOT` read their operand using its kind: a
packed integer is tested as raw bytes, a tagged value by its tag.

## 9. Invariants

- The stack is not zeroed; generated code writes a register before reading it.
- `CALL`'s `C` operand is reserved (the VM ignores it).
- `Function.param_count` is informational; the caller lays out arguments from the
  callee's function type.

## 10. Images

A **program image** is how a compiled program travels without a compiler: `oby
--bundle` compiles the prelude and the script, `src/vm/image.c` writes the
result, and a runner (`obyrun`, or any `oby`) loads it and runs it — no source
files, no filesystem. See `docs/oby.md` §12 for the command and
`plans/done/standalone_executable_bundling.md` for the format at large.

The layout is a header (magic, format version, native fingerprint,
`global_size`, function count, entry index) and then one record per function:
its name, the raw `uint64` instruction array, the constant pool, and the closure
metadata (`up_off`/`up_size`/`up_total`). Everything is little-endian, field by
field, so the layout does not depend on the machine.

Two properties are worth knowing:

- **The constant pool only carries `int`, `float`, `string` and `function`.**
  The loader resolves function constants in a second pass, because a constant
  can name a function that comes later in the image.
- **An image is coupled to the build that runs it.** `CALLNATIVE` carries a
  native *id*, which depends on registration order, so the header records FNV-1a
  over the native names and the loader refuses a mismatch instead of calling the
  wrong C function. Objects need nothing extra: `NEWOBJ` carries a field count,
  so no type table travels.

## Not yet

- **NaN-boxing** — tagged values are 16 bytes; shrinking them is a separate step.
- **Packed values at the native boundary** — natives see `Value`s, so a packed
  aggregate cannot be passed to one or interpolated yet (a compile error, not a
  silent copy). Object fields are fine: they are byte-tight.
- **GC** — heap values are freed manually (`object::free`).
- **Superinstructions**, **inline caching**.
- **Dynamic object member-add overflow** — a member added after a value of that
  type was created has no slot in the older value.
