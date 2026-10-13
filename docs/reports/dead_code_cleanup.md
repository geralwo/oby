# Dead Code Cleanup

Findings and decisions from the dead-code pass done on top of `8fc22ed`
("Move print/flush to io; read_line keeps endings"), verified with
`bash compile.sh test`: 142 passed, 0 failed.

## Removed

| Symbol | Where it lived | Why it was dead |
|---|---|---|
| `string_interpolate` | `src/core/string.c` | Stub returning `NULL`; `${expr}` is expanded at codegen time via `to_str` / `str_concat` |
| `diag_error_line` | `src/compiler/diag.c` | No callers; every diagnostic site passes a `Token` |
| `OBJ_STRING`, `OBJ_ARRAY` | `include/core/obj.h` | `String`/`Array` are primitives with their own `ValueTag`s; only `OBJ_OBJECT` is read |
| `AST_STMT_LIST` | `include/compiler/ast.h` | Never produced by the parser |
| `ARRAY_F_ZEROED` | `include/core/array.h` | Never set or queried |
| `array_at`, `array_front`, `array_is_empty`, `array_clear` | `src/core/array.c` | No callers anywhere |
| `array_push_int`, `array_push_ptr` | `include/core/array.h` | Unused macros |
| `array_new_fixed` | `src/core/array.c` | No callers; the VM creates fixed arrays via `OP_NEWARRAY` + `OP_ARRAY_FIXED` |

## Kept — load-bearing or covered (do not re-remove)

- **`ARRAY_F_OWNED`, `ARRAY_F_FIXED`** — the flags implement the array growth
  rules: set by `array_new` (and `OP_ARRAY_FIXED`), checked in `array_free`,
  `nat_free`, and the VM's append/pop/insert/remove guards.
- **`array_capacity`, `array_shrink_to_fit`, `array_copy`, `array_clone`,
  `array_equal`, `array_swap`** — exercised by `tests/array_test.c`, which
  `compile.sh test` builds and runs.
- **`string_hash`** — accessor for `String.hash` (live; `string_equal` uses
  it); the natural entry point when the hashing backlog item lands.
- **`array_set_length`** — now `static inline` in `include/core/array.h`, the
  one interface for writers outside `array.c`: `new_bytes_value` and
  `nat_read_bytes` (`src/native/io.c`) and `string_buffer_view`
  (`src/vm/vm.c`) go through it. `array.c`'s own mutators write `length`
  directly — the field stays private to the module.

## Header merge

`include/core/obj.h` no longer exists. `value.h` forward-declares
`typedef struct Obj Obj;` (it only ever needed the pointer), and `object.h`
defines `ObjKind` + `struct Obj` next to `struct Object`.

## Notes for the next pass

- Check `tests/*.c` before calling a C helper dead — six helpers the first pass
  listed are test-covered, not dead.
- A flag or field can be load-bearing with no "callers": `ARRAY_F_OWNED` is set
  by constructors and read by `array_free`, `nat_free` and the VM growth
  guards.
