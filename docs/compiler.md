# The OBY compiler

The compiler turns OBY source into bytecode for the VM (see `docs/vm.md`). It is
a straight pipeline of four stages, all under `src/compiler/`:

```
source ──▶ lexer ──▶ parser ──▶ sema ──▶ codegen ──▶ bytecode
           tokens     AST        typed AST   Functions
```

`src/main.c` drives it: read the file, lex and parse it, typecheck it (which also
loads the prelude and any `use`d files), then compile it and hand the result to
the VM. `-check` stops after sema; `-printast` also prints the typed AST.

Sources are looked up in the **embedded table** first (`core/embed.h`) and read
from disk otherwise. The prelude is baked into the binary by `compile.sh`
(`tools/embed_gen.c`), so the compiler finds it from any working directory and
a built binary does not need `src/prelude/` next to it — see `docs/prelude.md`.

Everything the frontend allocates — AST nodes, types, symbols, scopes — lives in
one **arena** (`core/arena.h`): a bump allocator over 64 KiB chunks, freed in a
single call when compilation ends. Nothing in the frontend calls `free`.

## 1. Lexer

`lexer.c` turns source text into tokens (`TokenKind` in
`include/compiler/lexer.h`). `scan_token` is called on demand; the parser pulls
one token at a time.

- **Newlines are tokens.** `TOKEN_NEWLINE` is emitted, not skipped, because the
  grammar uses it as a statement terminator.
- **Comments** (`// ...`), spaces, tabs and carriage returns are skipped.
- **Numbers** are `digit+ ('.' digit+)? ([eE][+-]? digit+)?`. A `.` joins the
  number only when a digit follows, so `1.method` is `1` `.` `method`.
- **Strings** are scanned to the closing `"`; `\"` does not end them. Escapes and
  interpolation are *not* handled here — the raw text is kept and the parser
  expands it.
- **Identifiers** must start with a letter. Keywords are not distinguished: the
  parser recognises them contextually.
- A bad character yields `TOKEN_ERROR` carrying a message; the parser reports it.

## 2. Parser

`parser.c` is a hand-written recursive-descent parser. It produces an `ASTNode`
tree (`include/compiler/ast.h`) allocated in the arena.

**Statement termination.** A newline ends a statement. This is implemented in
`advance`: after reading a token, if the next token is a newline *and* the token
just read cannot end a statement, the newline is skipped as whitespace.
`continues_line` lists those tokens — `,` `+` `-` `==` `<` `>` `(` `[` `.` `::`
`->` `=` and the keywords `and` `or` `not`. So `a = 1 b = 2` is two statements,
while `a = 1 +` ⏎ `2` is one.

**Error recovery.** A parse error sets `had_error`; `parse_program` then
`synchronize`s to the next newline and keeps going, so one bad line does not hide
the rest.

**Strings.** `parse_string` expands escapes (`\n`, `\t`, `\r`, `\0`, `\e`, `\\`,
`\"`, `\$`, `\xHH`) and splits `${...}` interpolation into an
`AST_INTERPOLATED_STRING` whose parts are literals and expressions. The
interpolation body is parsed by a **sub-parser** bounded to the `${...}` text, so
diagnostics inside it keep real line positions.

## 3. The AST

Every node has a `kind`, the `token` it started at, and two slots the later
stages fill in:

- `type` — the resolved `Type` (set by sema).
- `sym` — the resolved `Symbol` (set by sema), so codegen does not re-resolve.

`ASTNode.as` is a union of per-kind payloads; `include/compiler/ast.h` lists them
all.

## 4. Sema

`sema.c` is the typechecker. It runs in phases, because OBY allows a definition
to be used before it is written:

1. **`collect_types`** — walks the program and loads `use`d files (pragma-once),
   registering every type name (`NAME = type {...}`, `NAME = packed BASE[N] {...}`,
   enums). Imports resolve relative to the importing file (normalized before
   the pragma-once check).
2. **`collect_defs`** — registers globals, function signatures, members and
   static members, and resolves `extern` names against the native registry.
   Signatures are built here so calls can be checked before bodies are.
3. **Check bodies** — walks every statement and expression, resolving paths to
   symbols and annotating each node with its type. Type errors, unknown names and
   arity mismatches are reported here.
4. **`assign_field_offsets` / `assign_global_slots`** — once all types are final,
   lays out each object type's instance members at byte offsets (and records the
   type's total field size), then assigns each global and static member a byte
   offset in the global block (see `docs/vm.md` §4 and `docs/obj.md`).

**Scopes and symbols.** A `Scope` is a name → `Symbol` list with a parent link;
`scope_lookup` walks outward, `scope_lookup_local` looks in one scope. A `Symbol`
carries its `Type`, its kind (`SYM_TYPE`, `SYM_VALUE`, `SYM_FUNC`, `SYM_MEMBER`),
its slot, and — for functions — a `next` link to the next overload.

**Overloads.** Several functions may share a name; `pick_overload` chooses by
exact parameter types first, then by assignability.

**Captures.** Each function body gets a nesting id, and every local remembers
the function that declared it. When a body resolves a name belonging to an
enclosing function, `note_capture` records it on every function literal nested
inside that declaration — so the innermost creation site can copy the value and
each intermediate body can pass it along (`OP_GETUPVAL`).

## 5. Codegen

`codegen.c` walks the typed AST and emits instructions (`docs/vm.md`).

- **Registers are byte offsets.** `reg_alloc(size)` hands out the next free
  offset and `reg_free(size)` gives it back; `frame_size` is the high-water mark.
  A value's size comes from its type (`type_size`).
- **Functions** are compiled on demand: a function literal becomes a `Function`
  with its own code and constant pool, stored as a `VAL_FUNC` constant.
- **Calls** lay out the callee and arguments by size (see `docs/vm.md` §3).
- **Packed vs tagged** is decided by `kind_of`; `gen_expr_as` inserts a `CONV`
  when an expression's kind differs from the target's. A user packed type is
  classified as tagged, so it is byte-copied.
- **Packed member access** is offset arithmetic: `v.x` and `v[0]` fold
  `index × sizeof(element)` into the value's byte offset, and a write to a copy
  (a global, or a packed member of an object) is read-modify-written.
- **Closures** are built at the literal's site: the captured values are
  evaluated into a contiguous register block matching the function's
  `up_off`/`up_size` layout, then `OP_CLOSURE` copies them into a heap `Closure`.
  Inside the body, `gen_symbol_to` falls back to `OP_GETUPVAL` for a symbol that
  is neither a local of the current function (`locals_base` hides enclosing
  ones) nor a global.

The result is a `CompiledProgram`: the top-level `main` function, every function
it defined, and the global block size.

## 6. Diagnostics

`diag.c` renders every error and warning:

```
tests/type_error.oby:1:9: error: cannot bind 'String' to 'int'
 1 | int x = "hello"
   |         ^^^^^^^
```

The lexer, parser and sema all report through one `Diagnostics`. It holds the
current file and source so it can print the offending line and a caret under the
token. Imports push an **include frame** (`diag_push_include`), so an error inside
a `use`d file also shows how it was reached:

```
  note: imported from tests/script_test.oby:1
```

`diag_summary` prints the final `N errors, M warnings` line.

## 7. The CLI

`src/main.c`:

| Invocation | Effect |
|------------|--------|
| `oby <script.oby> [arguments...]` | compile and run |
| `oby -check <script.oby>` | stop after sema (parse + typecheck) |
| `oby -printast <script.oby>` | print the typed AST (when there are no errors) |
| `oby --bundle <script.oby> <output>` | write a standalone executable |
| `oby -h` / `-v` | help / version |

Everything after the script path belongs to the script, not to `oby`: it is what
`args()` returns, and a flag written there is not an option. Options are only
recognized before the script path, so `oby tool.oby -check` runs `tool.oby` with
`-check` as an argument.

`-printast` output comes from `ast_printer.c`: an indented tree where each node
shows its kind, its resolved type, and the names involved.

**Standalone executables.** `--bundle` compiles the entry with the prelude and
its imports (`vm/image.c` serializes the result) and writes `<output>` as a
**copy of the running binary** with the image and the footer appended. Nothing
but `oby` is needed to bundle, and nothing is needed to run the artifact: no
files, no toolchain, no compilation at startup. The price is size — the artifact
is the toolchain (~256 KB) plus the program (17–35 KB); see `docs/oby.md` §12.

**Bundled binaries.** Before the command line is parsed, `main` checks whether
the running executable ends in a bundle footer (`core/bundle.h`) and, if so,
runs its image payload through the VM. A binary without a footer is the plain
compiler, and an unknown payload type is refused rather than ignored. A bundled
binary never reaches the CLI, so `--bundle` cannot be run from one.

Compiling is split so a bundle needs no temporary file: `compile_source` runs
the frontend over an in-memory buffer, and `compile_file` is the read-then-
compile case the CLI uses.

## 8. Tests

`./compile.sh test` builds and runs everything under `tests/`. For `.oby` files
the file name decides how it is run:

| Pattern | Invocation | Expected |
|---------|------------|----------|
| `*_error.oby` | `oby -check <file>` | non-zero exit (compile errors) |
| `*_runtime_error.oby` | `oby <file>` | non-zero exit and a `vm error` line |
| anything else | `oby <file>` | zero exit and output matching `<name>.out` |

So a test that exercises runtime behaviour is simply run; `-check` is what marks
a diagnostics test. `*_runtime_error.oby` is checked before `*_error.oby`,
because the latter is a suffix of the former.

A passing `.oby` test's stdout is compared against a golden file
`tests/<name>.out`, folding CRLF into LF (the repository lives on Windows and
editors can rewrite either side). Apart from line endings the comparison is
exact: a missing or differing golden fails the test, so a wrong value no longer
passes silently — a stray NUL byte or a missing trailing newline is caught.
Behaviour that depends on carriage returns has to be checked inside the test
(see `tests/crlf_test.oby`). After an intended change, `./compile.sh test-update`
rewrites the golden files from the current output.

Each `tests/*.c` is a standalone test of a C primitive (the `Array`, the native
registry). It is compiled against `src/core/` and `src/native/` into its own
binary and must exit 0. A test may also drive the built compiler through the
shell (`tests/relocate_test.c` runs `build/oby` from a foreign working
directory to check the embedded prelude).
