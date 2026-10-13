# The OBY Language

OBY is a small, statically typed scripting language that compiles to bytecode
for a stack-based virtual machine. This document specifies the language as it
is currently implemented by the compiler in `src/compiler/`.

It reflects only what OBY actually does today. Features that are not yet
implemented are listed in [§13](#13-not-yet-implemented).

---

## 1. Source files

- A program is one or more source files. The compiler is given an entry file.
- Source is plain text. `//` starts a comment that runs to the end of the line.
- **Statements are terminated by a newline.** There are no semicolons.
- Blank lines are ignored.
- A statement also ends earlier, wherever the grammar cannot continue — so
  `a = 1 b = 2` is two statements even on one line.
- A newline is ignored when the line ends with a token that cannot end a
  statement: a comma, a binary or unary operator (`+`, `-`, `==`, `<`, `>`,
  `and`, `or`, `not`), an opening bracket (`(`, `[`), or `.`, `::`, `->`, `=`.
  This lets lists and expressions span lines:

  ```
  foo(1,
      2)
  a = 1 +
      2
  ```

  A closing bracket must stay on the last element's line (`foo(1,` ⏎ `2)` is
  fine; `foo(1,` ⏎ `2` ⏎ `)` is not).

## 2. Lexical structure

### 2.1 Tokens

| Token | Form |
|-------|------|
| Identifier | `[A-Za-z][A-Za-z0-9_]*` |
| Integer | `[0-9]+` decimal, `0x1F` hex, `0b1010` binary (no octal); `_` between digits is a separator (`1_000_000`) |
| Float | `[0-9]+ '.' [0-9]+` or with an exponent `[eE][+-]?[0-9]+`; separators allowed too |
| String | `"` … `"` — characters up to the next `"` |
| Character | `'` … `'` — one character or escape, a `byte` |
| Comment | `//` … end of line |
| Punctuation | `( ) { } [ ] , . $ : :: -> = == != + - * / % += -= *= /= %= < > <= >= & \| ^ ~ << >> &= \|= ^= <<= >>=` |
| Newline | statement terminator |

Notes:

- Identifiers always start with a letter.
- A `.` only joins a number when a digit follows, so `1.method` is `1` `.` `method`.
- **Integer literals** are `0x` hex or `0b` binary only when they start with `0`;
  digits must fit the base (`0x1Fg` and `0b2` are errors), and there is no octal
  (`010` is ten).
- A `_` between two digits is a **digit separator**: `1_000_000`, `0xFF_FF`,
  `1_000.5`, `1e1_0`. Anywhere else it is an error (`1_`, `1__000`, `0x_FF`),
  and `String::to_int` / `String::to_float` apply the same rule.
- A `//` starts a comment that runs to the end of the line.
- **Character literals** are one `byte`: `'A'`, `'\n'`, `'\''`, `'\x41'`. They use
  the same escapes as strings and hold exactly one character (`'ab'` is an
  error). They compare directly with a string's bytes: `s.buffer[i] == 'A'`.
  A character literal cannot span a line.
- **Strings support escapes and interpolation.** The escapes `\\`, `\"`, `\$`,
  `\n`, `\t`, `\r`, `\0`, `\e` and `\xHH` are expanded, and `${expr}` embeds
  an expression (see [§2.3](#23-string-literals)).
- A lone `:` is recognised by the lexer but is not used by the grammar;
  encountering it is a syntax error. A lone `!` is an error (use `not`).
- `;` is only used inside a repeat literal (`[expr; n]`, [§4.3](#43-arrays)).

### 2.2 Keywords

Keywords are **contextual**: the lexer emits plain identifiers, and the parser
recognises these words only where the grammar expects them:

```
use   type   if   else   while   for   switch   case   default   return   break
continue   extern   and   or   not   local
```

Any of them may still be used as an ordinary name elsewhere.

### 2.3 String literals

A string is written `"..."`. A `\"` inside it does not end it. The following
escapes are expanded:

| Escape | Meaning |
|--------|---------|
| `\\` `\"` `\$` | literal `\`, `"`, `$` |
| `\n` `\t` `\r` `\0` | newline, tab, carriage return, NUL |
| `\e` | ESC (`0x1B`) |
| `\xHH` | the byte `HH` (two hex digits) |

Any other `\c` is the literal character `c`.

**Interpolation.** `${expr}` evaluates `expr` and splices its value into the
string. The value is formatted as text (`int`, `float`, `bool`) or used as-is
(`String`):

```
name = "oby"
print("hello ${name}!")
```

An unterminated `${` is an error. **The body is ordinary code**: the enclosing
literal's escapes do not apply inside it, so a string there is written plainly,
and its escapes follow the usual rules. Braces balance outside nested string
literals, so `${f({a=1})}` is one expression, and a `}` inside a nested literal
is just a byte of it:

```
m = Map<String, int>()
print("${m.get("a", 0)}")     // quotes are plain here
print("${ "a}b".length }")    // 3: a `}` inside a nested string
print("${ "a\nb".length }")   // 3: one escape level — the nested string's own
```

## 3. Names and paths

A **path** is a sequence of names joined by `.` (instance access) or `::`
(static access):

```
player.move
int::to_str
test::new
```

The first segment is resolved in the current scope; each following segment is
resolved as a member of the previous one.

**Globals are explicit: `$name`.** A path may start with `$`, which resolves the
name in the **global scope only** — from anywhere, including inside a function:

```
score = 0                  // a global
bump  = () { $score += 1 } // writes it
show  = () -> int { return $score }
$score += 1                // works at file scope too (the `$` is redundant there)
```

Inside a function a bare name **never** reaches a global *variable*: it is a
local, a parameter, or a global *function*. So `score = 1` in a function creates
a local `score` and leaves the global alone; reading `score` there is an error
(`'score' is a global; inside a function read it as '$score'`). Functions stay
visible bare, since calling one is not touching state. Write `local` when a
function introduces a new binding ([§5](#5-bindings)): assigning to an
undeclared name still declares a local, but warns.

`$x = value` **declares** the global when it does not exist yet, the same way
`x = value` does at file scope; reading a `$name` that was never assigned is an
error (`unknown global`). A type is not a value, so `$String` is rejected.

`::` is a **static-access operator only**. There is no relative form: to name a
static member you always write the full path (`Type::member`). Inside a type
body, `::name = value` is a shorthand for declaring `Type::name = value` (see
[§7](#7-statements)).

A segment in a **type** position may carry type arguments, `Name<...>` — that is
how a generic type is written ([§4.7](#47-generic-types)):

```
Box<int> b = {}
Pair<String, int> p = {}
List<List<int>> grid = List<List<int>>()
```

In an *expression* the arguments are kept only where they cannot be confused
with comparisons: when the closing `>` is followed by `::` (static access,
`Box<int>::count`) or by `(` (the constructor call, `Box<int>()`). An ordinary
comparison like `a < b` is never read as type arguments.

## 4. Types

OBY is statically typed: every expression and binding has a type known before
code generation.

A type name may be defined only once; redefining one (including a builtin) is an
error — see [§9](#9-imports).

A **single uppercase letter** is reserved for generic parameters (`Box<T>`,
`Pair<A, B>`), so it cannot be a type name: `A = type { ... }` is an error
(`type name 'A' is reserved for generic parameters`). Lowercase single letters
(`t`) are not reserved.

| Kind | Examples | Meaning |
|------|----------|---------|
| Packed | `byte`, `int`, `uint`, `int64`, `uint64`, `float`, `double`, `packed` types | Fixed-width value, stored inline on the stack |
| Object | `object`, `type { ... }` types | Heap value, reference semantics |
| String | `String` | Built-in string primitive, reference semantics |
| Array | `int[8]`, `byte[]` | Fixed or growable sequence |
| Function | `(int, int) -> int` | Callable value |
| `void` | — | No value |

**Built-in types:** `byte`, `int`, `uint`, `int64`, `uint64`, `float`,
`double`, `object`, `void`, `String`.

The native numeric types are built in so the compiler knows their
representation. `float` (4 bytes) and `double` (8 bytes) additionally carry
floating-point semantics that a `packed` declaration cannot express. The
prelude attaches their constructors and methods (see
[§11](#11-standard-library)).

### 4.1 Packed types

```
vec2i = packed int[2] {
    x = vec2i[0]
    y = vec2i[1]
}
```

`NAME = packed BASE[N]` declares `NAME` as a packed type: `N` elements of
`BASE`, byte-tight. A packed value is fixed inline storage, not a heap object.
`packed` is a contextual keyword.

- The element type must itself be packed (`byte`, `int`, ..., or another packed
  type), and the whole type must fit in 255 bytes.
- The body is optional. Each entry maps a member name to an element index, so
  `v.x` and `v[0]` are the same access.
- Indexing a packed value requires a **constant** index; the compiler turns it
  into a byte offset.
- An array literal whose expected type is a packed type builds one:
  `vec2i v = [3, 4]`, `return [x, y]`.
- `T()` with no matching `::new` is the intrinsic **zero value** (see
  [§6](#6-functions)); a 0-argument `::new` overrides it.
- A packed value cannot cross a tagged boundary yet: it cannot be stored in an
  object field, passed to a native, or interpolated into a string.

```
vec2i::new = (int x, int y) -> vec2i {
    return [x, y]
}

v = vec2i(3, 4)
v.x = 10
zero = vec2i()      // (0, 0)
```

The native types `byte`, `int`, `uint`, `int64`, `uint64`, `float`, `double`
are already built in; this form is for defining new ones.

### 4.2 Object types

```
test = type {
    name  = "test"
    count = 0
    ::new = () -> test { return {} }
}
```

`NAME = type { ... }` defines a **nominal** object type. The body is a list of
members:

- `member = value` — an instance member; its type is inferred from the value.
- `Type member = value` — an instance member with a **declared** type. The value
  must be assignable to it (`int n = "x"` is an error). The initializer is a
  **type witness, not a default**: see below.
- `::member = value` — a static member of the type.
- `member = { ... }` — a member whose value is a **dynamic object**.

Every member has an initializer: `Type member` (no `=`) is an error, and any
other entry in the body (`String s` without a value, a stray expression) is
reported rather than silently dropped from the type.

By convention, **object types and namespaces are PascalCase** (`File`, `Terminal`,
`Input`) and **built-in primitives and packed types are lowercase** (`int`,
`byte`, `float`, `vec2i`); `io` is the one lowercase namespace
([`prelude.md`](prelude.md)).

Members are visible to one another within the body, so a field value can name a
sibling member. A member name therefore shadows a type or a local/parameter of
the same name for the whole body; referring to such a name is an error (`member
'x' shadows a local of the same name`) rather than silently binding to the
member.

A bare `{ ... }` (not preceded by `type`) is a **dynamic object** value. Each
`{ ... }` literal gets its own **structural type** carrying its members, so
members can be added and read statically:

```
test = { name = "test" }
test.count = 1        // adds a member
test.count            // int
```

Only `type { ... }` defines a **nominal** type; `{ ... }` is structural, and two
objects with the same members are still distinct types.

**Constructing a nominal type.** When a `{}` literal's expected type is a
nominal object, the literal takes that type and is built with its layout, so no
conversion is needed at runtime:

```
test t = { name = "x" }     // a `test`, not a structural object
return { count = 1 }        // from a function returning `test`
f({ name = "x" })           // a `test` argument
```

- Every member the literal writes must exist in the target, with an assignable
  type; an unknown member is an error (`type 'test' has no member 'y'`).
- A member the literal **omits takes its declared default** — the value written
  in the type body. `T = type { count = 5 }` gives `T t = {}` a `t.count` of 5.
  A member the literal writes keeps the literal's value.
- A **tagged default allocates a fresh value per construction** (`s = ""`,
  `xs = [0; 16]`), so two instances never share one array or string.
- A member may be defaulted to `null` on purpose (`String s = null`); reading
  through it is then a runtime error. Nothing else can leave a *reference* member
  unset.
- A default is evaluated on its own: it may not read a **sibling member**
  (`b = a + 1`), since those values are being built at the same time.
- A `::name = value` entry in a type body is a **static** — initialized once, not
  per construction — and one inside a `{}` literal refers to a **static** of the
  target.
- Nested literals coerce recursively.

A `{}` literal with **no** expected type keeps its own structural type, as does
one bound to `object` (which accepts any value).

**Members are stored byte-tight.** The compiler lays each member out at a byte
offset sized from its type, so a packed member (`vec2i`, `int`, ...) is stored
inline with no boxing:

```
player = {
    pos  = vec2i(1, 2)     // 8 bytes inline
    name = "oby"           // a 16-byte Value
}
player.pos.x = 9           // writes 4 bytes; `y` is untouched
```

`object` accepts any value in an assignment, but a packed value cannot be stored
in an `object`-typed slot (that slot is a 16-byte `Value`).

### 4.3 Arrays

```
int[8] cells = [1,2,3,4,5,6,7,8]
int[]  grow  = [1,2,3]
```

- `T[n]` is a fixed array of `n` elements.
- `T[]` is a growable array of unknown length.
- An array literal `[a, b, c]` infers its element type (all elements share a
  type; numeric elements widen). With no expected type it is a growable `T[]`.
- When the expected type is known, the literal takes it instead: `int64[] a =
  [1, 2, 3]` widens the elements and `vec2i[] v = [[1, 2], [3, 4]]` coerces each
  nested literal. A `T[n]` target requires exactly `n` elements, and every
  element must be assignable to the element type.
- A **repeat literal** `[expr; n]` builds `n` copies of `expr`, evaluating
  `expr` once. A constant `n` can be coerced to a fixed `T[n]` like any other
  literal; a runtime `n` (an `int`) gives a growable `T[]`, and a negative one
  is a runtime error. `[vec2i(); 800]` is 800 zero vectors.
- Indexing `a[i]` yields the element type; the index must be an integer. An
  out-of-range index is a runtime error.
- `a.length` is the element count (`int`).
- Growth: `a.push(x)` appends `x`; `a.pop()` removes and returns the last
  element; `a.insert(i, x)` inserts `x` before index `i`; `a.remove(i)` removes
  and returns element `i`. These are compiler intrinsics, not real methods, and
  only a growable `T[]` can be resized. A fixed `T[n]` cannot — neither directly
  (a compile error) nor through a `T[]` alias (a runtime error).

Assignability: `int[3]` is assignable to `int[]`; `int[3]` is **not** assignable
to `int[4]`.

An array is a **heap value with reference semantics**: `b = a` aliases the same
array, and passing one to a function passes the reference. Two arrays are equal
only if they are the same array (`==` is identity, not element-wise). Elements
are stored byte-tight, so `int[8]` is 32 bytes of `int` and `vec2i[4]` is 32
bytes of raw `vec2i` (no per-element boxing).

**Memory is manual.** There is no GC: free a heap value with `object::free`
when you are done with it (see [§11](#11-prelude)).

```
nums = [1, 2, 3]
print(int::to_str(nums[1]))    // 2
nums[1] = 20
print(int::to_str(nums.length))  // 3
object::free(nums)
```

Growth mutates the heap array in place: `push`/`insert` reallocate the buffer
when it is full. A string's `buffer` view is not a heap array and cannot grow;
popping an empty array, or an out-of-range insert/remove index, is a runtime
error.

### 4.4 Function types

`(int, int) -> int` is the type of a function taking two `int`s and returning an
`int`. A function with no `->` returns `void`.

`void` is **not a value**: an expression that returns nothing can only be used
as a statement (or returned from a `-> void` function). Binding, assigning,
passing, interpolating, or testing one is an error — including a `::new` that
returns nothing, since `T()` would then have no value to produce.

A function type may appear wherever a type can. Function values are
first-class: they can be passed as arguments, returned, and stored in members.

```
apply = ((int) -> int f, int x) -> int { return f(x) }
inc   = (int x) -> int { return x + 1 }
apply(inc, 4)                 // 5

pick = () -> (int) -> int { return inc }
pick()(9)                     // 10
```

Two function types are assignable when they take the same number of arguments
and each parameter and the result are assignable. Function types nest to any
depth. A parenthesized function type takes the suffixes any type does, so
`((int) -> int)[]` is an array of functions and `List<((int) -> int)>` a list of
them; a binding may also spell the type inline (`(int) -> int f = inc`).

A **function-type alias** names a function type once, so a parameter can be
written in the uniform `Type name` form instead of spelling the type inline:

```
SomeFn = (int, int) -> int
apply  = (SomeFn fn, int x) -> int { return fn(x, x) }
SomeFn g = inc
```

The right side must be a function type, and the name is a type — it cannot
redefine an existing type and cannot be used as a value. Only a function type
can be aliased this way: any other type expression (`Bytes = byte[]`,
`N = { x = 0 }`, `A = SomeType`) could also be read as a value, so
`name = <type>` would be ambiguous.

An alias may take **type parameters**, and is then instantiated per argument
list (each instantiation is the one interned function type for those
arguments):

```
Comparator<T> = (T, T) -> int
take = (Comparator<int> cmp) -> int { return cmp(1, 2) }

Bag = type<T> {
    sort_by = (Comparator<T> cmp) -> void { ... }
}
```

As the example shows, `Comparator<T>` written in a generic type's body uses
that instance's argument, so the alias replaces the `((T, T) -> int cmp)`
spelling where it is most awkward.

The aliased type is only built when the alias is used — it has no meaning before
the parameters are bound — but the names on the right side are checked at the
declaration, so a typo does not wait for a use.

Read a parameter list by its **names**: in `((int) -> int f, int x)` the first
parameter is `f`, of function type `(int) -> int`, and the second is `x`, an
`int`. With no `->` after the list the function itself returns `void` — which is
worth writing out when a parameter is also a function type, since the `->` inside
the list belongs to the parameter:

```
// One parameter, `cmp`; the parameter list's own `)` is the one after the
// name. `(T, T)` closes itself, `-> int` belongs to the parameter's type.
sort_by = ((T, T) -> int cmp) -> void {
    if cmp(a, b) > 0 { ... }
}
```

### 4.5 Enums

```
Color = enum {
    RED
    GREEN
    BLUE
}
```

`NAME = enum { ... }` defines an enum type over a fixed **underlying type**.
Each item is a member of the enum type:

- `ITEM` takes the previous value plus one; the first is `0`. This only applies
  to integer enums.
- `ITEM = expr` sets an explicit value of any type. A literal item takes the
  type the earlier items established (`A = uint(1)`, `B = 2`).

All values must share a type; that type becomes the enum's underlying type
(`int` if every item is auto-numbered). Items are read as `Color::RED`. An
item's type is the enum, and an enum value is assignable to its underlying type
— but a value of the underlying type is not assignable to the enum.

An **integer constant** item (`ITEM = 5`) is a compile-time value, usable in
repeat counts and constant indexes. An item whose value is not constant (a
constructor call, a string) is a static with storage: it is initialized in
declaration order at the enum's definition, and reading it is an ordinary load.

```
e = enum {
    GOOD = "good"
    BAD  = "bad"
}
String s = e::GOOD

Flags = enum {
    A = uint(1)
    B = 2
    C = 4
}
```

### 4.6 Null

`null` is the one value with no data. Its type is `null`, and it is assignable
to any **tagged** type — `String`, `Array`, `{}` objects, and function types:

```
String s = null
int[] a = null
(int) -> int f = null
```

Packed values are raw bytes and can never be null: `int n = null` is an error
(`cannot bind 'null' to 'int'`).

Because a tagged value may be null, the type checker no longer promises that a
`String` is usable at run time: reading a member of null is a **runtime error**
(`member access on null`), not a silent value.

`null` cannot supply an inferred type, so a declaration has to name one:

- `x = null` — error (`cannot infer a type from 'null'; write 'Type name = null'`)
- `local x = null` — error
- a member `s = null` with no declared type — error
- `[null, null]` with no declared element type — error

Assigning to an *existing* binding is fine, because its type is already known:
`s = null` clears a `String`, and `c = null` drops a reference to a function or
closure.

`null` is falsey, compares with `==` / `!=` against tagged values (`x == null`),
and renders as `null` when interpolated.

### 4.7 Generic types

A type body may take **type parameters**, and each argument list gives a
separate concrete type. Parameters are written as single uppercase letters
(`T`, `K`, `V`, `A`, `B`), and the two sides cannot collide: a type name cannot
be a single uppercase letter (`A = type { ... }` is an error, [§4](#4-types)),
and a parameter cannot reuse an existing type's name:

```
Box  = type<T>    { T[] items = [] }
Pair = type<K, V> { K[] keys = []  V[] values = [] }

Box<int> b = {}
c = Box<int>()                  // the constructor call, as for any type
Box<String> s = {}
```

`Box` on its own is not a type (`'Box' expects type arguments ('Box<...>')`),
and the arguments must be types (`'x' is not a type; 'Box' expects 'T'`).

**Every instantiation is a distinct type.** `Box<int>` and `Box<String>` share
nothing — the body is checked once per argument list with each parameter
replaced by its argument, and compiled concretely. So:

- members take the argument's type (`T[] items` is a real `int[]` inside
  `Box<int>`), a packed element stays packed, nothing is boxed and no cast is
  needed;
- statics and constructors are per instantiation: `Box<int>::count` and
  `Box<String>::count` are separate globals, and `::new = () -> Box { ... }`
  builds that instantiation only;
- a body is checked **when it is instantiated**, so a body that holds for one
  argument and not another is an error only where it is wrong, reported on the
  line inside the template:

```
Opt = type<T> { T v = null }    // `null` fits String (tagged), not int
Opt<String> s = {}
Opt<int> i = {}                 // error: cannot bind 'null' to 'int'
```

**Inside the body** the type's own name means *this* instantiation (a return
type can say `-> Box`), the parameters are ordinary type names, and members
behave as in any nominal type ([§4.2](#42-object-types)): a function member is a
method with its receiver implicit.

Notes:

- An argument may be any type: `Box<int[]>`, `Box<Box<int>>`,
  `Map<String, vec2i>` — `>>` closes two nested lists.
- A body may use whatever its parameters offer (`xs[i] < x`, `k == key`,
  `x.burn()`); using a type the body does not hold for is an error at that
  instantiation. There are no bounds to declare yet.
- A member of a parameter type needs a default valid for **every** argument, so
  container members are arrays (`T[] items = []`), which work for all `T`.
- Instantiation is capped (64 levels of nesting), so a type whose arguments grow
  (`Foo<T[]> next`) is a diagnostic rather than a hang.
- `Option<T>`-shaped types wait for sum types: there is no pattern matching, and
  a packed payload has no `null` default.
- The prelude's `List<T>`, `Map<K, V>` and `Pair<A, B>` are ordinary generic
  types written in OBY ([§11](#11-prelude)) — there is no privileged tier.
- **Generic functions** ([§6](#6-functions)) are a separate form: a free
  function may take its own parameters (`f = <T>(T x) -> T { ... }`) and is
  instantiated per call, with the arguments inferred. A **function member** may
  too (`map = <U>((T) -> U f) -> List<U>`): the receiver supplies the enclosing
  type's parameters, `U` comes from the call, and the type's own name means that
  receiver — `Box<U>` names a *new* instantiation.

## 5. Bindings

- **Inferred:** `name = value` — the binding takes the value's type.
- **Explicit:** `Type name = value` — the value must be assignable to `Type`.
- **Local:** `local name = value` or `local Type name = value` — declares a new
  binding in the current block, shadowing any enclosing binding of that name.

At file scope a binding is a global; inside a function it is a local slot. Plain
`name = value` **reuses** an existing binding of that name from any enclosing
scope **of the same function**, so a block can update a variable declared
earlier in the function body. It never reaches a *global variable*: inside a
function a bare name is a local, a parameter, or a (global) function, and globals
are written and read as `$name` ([§3](#3-names-and-paths)). `local` is the
explicit way to force a fresh binding, shadowing even a binding of the same name
in an enclosing block; the binding ends with its block, and `local` is only
allowed inside a function.

A declared binding (`Type name = value`) declares a **variable**. A later
`name = <function literal>` therefore writes that variable instead of adding
another overload; overloads come from repeated `name = (params) { ... }`
definitions with no declared binding.

A fresh binding should be declared with `local`. `name = value` writing a name
that is not already bound declares a local too (inside a function), but the
compiler warns — `'n' is undeclared; the assignment declares a new local (write
'local n = ...')` — because the reader cannot otherwise tell a new binding from
an update. At file scope the same form simply declares a global and does not
warn.

A binding may not reuse a **type** name — `int`, `String`, or a user type:
`int int = 2` is an error (`cannot reuse the type name 'int' as a variable`),
because it would hide the type. The one exception is a **receiver** parameter
(`(vec2i) -> ...`), which binds the type's name on purpose. Member names are
unaffected.

A type name is not itself a value: using one in an expression — `x = int` — is
an error (`'int' is a type, not a value`). It can still head a `::` path
(`int::to_str`) or name a constructor (`vec2i(1, 2)`).

## 6. Functions

```
add = (int a, int b) -> int {
    return a + b
}
```

A function literal is `(params) [-> type] { block }`. Omitting `-> type` means
the function returns `void`. A literal is a value: it can be assigned, passed
as an argument, returned, and defined inside another function.

**Closures.** A literal that refers to a local or parameter of an enclosing
function *captures* it. Captures are **by value** — the value is copied when the
closure is created — and **read-only**: assigning to a captured name is an
error.

```
make_adder = (int n) -> (int) -> int {
    return (int x) -> int { return x + n }
}
add5 = make_adder(5)
add5(3)                 // 8
```

Because the value is copied at creation, a later change to the original local
is not seen by the closure. To keep mutable state, capture a reference value and
mutate *that*:

```
make_counter = () -> () -> int {
    state = { n = 0 }
    return () -> int { state.n += 1; return state.n }
}
```

A closure is a heap value: free it with `object::free`, like an array or object.

**Closures and ownership.** A capture is a copy, so it does not take ownership of
what it points at. A closure that *creates* its own heap state and hands back
only itself leaves that state reachable from nothing:

```
make_counter = () -> () -> int {
    state = { n = 0 }          // only the closure knows this object
    return () -> int { state.n += 1; return state.n }
}
```

Freeing the closure strands `state` (there is no GC), so it leaks. Prefer
capturing values the caller owns, or keep mutable state in a type with methods
and pass the instance in. Lifting the read-only restriction on captures (shared
cells) would make this automatic, and is in the backlog.

Parameters may be written:

| Form | Meaning |
|------|---------|
| `Type name` | typed parameter |
| `name` | untyped parameter (type unknown) |
| `Type` | the **receiver** |

A lone identifier that names a type is a receiver: the method's receiver has
that type, and inside the body the receiver is bound under the type's name.

```
vec2i::length2 = (vec2i) -> int {
    return vec2i.x * vec2i.x + vec2i.y * vec2i.y
}
```

Both forms reach the type's statics — `vec2i::new(...)` and `vec2i.x` work inside
`(vec2i, int s) -> vec2i` just as they do with a named receiver — so the choice
is style: the bare form keeps small arithmetic-like bodies short
(`vec2i.x += b.x`), while naming the receiver (`(vec2i v)`) reads better once the
body also constructs values, or when `v` says more than the type name. The
prelude's `vec.oby` uses the bare form throughout ([`prelude.md`](prelude.md)).

**Calling a method.** `value.method(args)` is sugar for
`Type::method(value, args)`: when `value`'s type has a function member `method`
whose first parameter is the receiver, the receiver is passed as the first
argument.

```
v = vec2i(3, 4)
v.length2()          // same as vec2i::length2(v)
c.bump()             // mutates `c` through its receiver
```

Overloads are resolved with the receiver as the first argument. If no method
overload matches, the call is an error (`no matching method 'm' for 'T'`); the
explicit `Type::method(value, args)` form always works.

**Inside a method, a bare instance member is `self.name`.** A function member of
a nominal `type { ... }` body gets the receiver as an implicit first parameter,
named after the type: `bump = () { n += 1 }` means `self.n += 1`, and the
receiver is also reachable by the type's name (`Counter.n`). The same method may
be written out instead — `Counter::bump = (Counter c) { c.n += 1 }` — which is
the only way to write one outside the type body.

```
Counter = type {
    n = 0
    bump = () { n += 1 }            // implicit receiver
    value = () -> int { return n }
    ::new = () -> Counter { return {} }
}
c = Counter()
c.bump()                            // same as Counter::bump(c)
```

A function member of a `{}` literal is **not** a method: it is a plain value (a
callback), so `box.op(21)` passes exactly `21`. A `{}` literal's structural type
has no name, so its function members cannot take a receiver at all — put shared
state in a nominal type, or pass the object in.

`extern` declares a native function implemented in C:

```
extern native_add(int a, int b) -> int
```

The same function name may be defined more than once with different parameter
types; a call is resolved to the overload whose parameter types match (exact
types first, then coercible ones).

`T(args)`, where `T` names a type, is sugar for `T::new(args)` — a type's
constructor is called by naming the type:

```
int(2)      // same as int::new(2)
```

A type that defines no `::new` **has no constructor**, so calling it is an
error (`type 'T' has no constructor`). If `::new` exists but no overload
matches the arguments, that is also an error.

The exception is a **packed type** (scalar or aggregate): `T()` with no
matching `::new` is its intrinsic **zero value** (every byte zero). A
0-argument `::new` overrides it:

```
vec2i v = vec2i()      // (0, 0)
int n = int()          // 0
```

**Operator overloads.** A type may define the binary operators as static members
named `+`, `-`, `*`, `/`, `&`, `|`, `^`, `<<`, `>>`, and the compound-assignment
operators as `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`:

```
vec2i::+ = (vec2i, vec2i b) -> vec2i {
    vec2i.x += b.x
    vec2i.y += b.y
    return vec2i
}
```

The first parameter is the receiver (a bare type name), the second the right
operand. `a + b` calls `T::+` when the **left** operand's type defines it;
otherwise the built-in operator applies. `a += b` calls `T::+=` when defined,
else falls back to `a = a + b` (which may itself call `T::+`).

> **Writing the name.** The operator token must be followed by a space before
> `=`: `vec2i::+ = ...` defines the binary `+`, while `vec2i::+= = ...` (no
> space) defines the compound `+=`. The same holds for `-`, `*`, `/`.

**Generic functions.** A function declaration may take **type parameters**, and
each call instantiates it for the argument types ([§4.7](#47-generic-types)):

```
identity = <T>(T x) -> T {
    return x
}

max_of = <T>(T a, T b) -> T {
    if a < b {
        return b
    }
    return a
}

print(identity(42))        // T = int
print(identity("hi"))      // T = String: a second instantiation
print(max_of(3, 7))        // 7
```

The type arguments are **inferred** from the call's own arguments: a parameter
that names `T` binds it, a parameter that is an instance binds its arguments
(`<T>(List<T> xs)` against a `List<int>`), and a function-typed parameter binds
through its signature. A parameter that determines nothing has to be named at
the call:

```
pick = <T>(int n) -> int { return n }
pick(9)          // error: cannot infer type parameter 'T' for 'pick'
pick<int>(9)     // ok
```

Like a generic type, each instantiation is its own function: the body is checked
with the parameters replaced, so a body that holds for one argument and not
another fails only where it is wrong
(`add_all = <T>(T a, T b) -> T { return a + b }` works for `int` and is an error
for `String`), and a generic function may call itself. A generic function is a
**template, not a value**: naming it without a call is an error, and it cannot
share a name with a concrete overload.

A **function member** may take its own type parameters the same way —
`map = <U>((T) -> U f) -> List<U>` inside `List<T>` is called `xs.map(f)`, with
`U` inferred from `f` and the receiver supplying `T`. A concrete member wins
when a name has several; a generic member is the fallback.

## 7. Statements

- `use "file"` — import ([§9](#9-imports)).
- `if expr block [else (if ... | block)]`
- `while expr block`
- `for (init; cond; step) block` — C-style `for`. `init` and `step` are
  statements (typically `local i = 0` and `i += 1`); `cond` is an expression.
  `init` runs once, then `cond` is tested, the body runs, `step` runs, and it
  repeats. `continue` runs `step` before re-testing `cond`. Declaring the counter
  with `local` in `init` keeps it inside the loop — `local` is allowed in the
  `for` initializer even at file scope, where it would otherwise be rejected.
- `break` — leave the innermost loop
- `continue` — skip to the next iteration of the innermost loop
- `switch` — match one value against a list of cases, first match wins:

  ```
  switch k {
  case 0 {
      return "zero"
  }
  case 1..10 {
      return "small"
  }
  case "add" if count < 10 {
      return "add"
  }
  default {
      return "many"
  }
  }
  ```

  Each case value must be assignable to the switch value's type. There is **no
  fallthrough** — only the matching body runs — so `break` is not needed (and a
  `break` inside a case leaves an enclosing *loop*, since a switch is not a
  loop). `case` and `default` are contextual keywords, and a body takes one
  `default`.

  A case may add a **guard** (`case value if cond`): it matches only when the
  condition holds too, so a guarded case is a narrower reading of the same
  value. A case may also be a **range**, `case lo..hi` — half-open like
  `String::slice`: `lo` is included, `hi` is not. Both bounds must be constant
  integers (enum items count), and the switch value must be an integer kind or
  an enum. A value can match several cases; the first one wins.

  A switch over an enum with no `default` is **checked for coverage**: every
  item must be covered by an unguarded case (a range may cover items), or the
  compiler warns (`switch on 'Color' has no case for 'BLUE' and no default`).
  When every item is covered, the switch cannot fall through — a function can
  end with it and needs no trailing `return`.
- `return [expr]`
- `name = value` and `Type name = value`
- `local name = value` and `local Type name = value` — declare in the current block
- `target.member = value` and `target::member = value`
- `::member = value` — declares a static member of the enclosing type
- `target op= value` — compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`,
  `|=`, `^=`, `<<=`, `>>=`)
- an expression, evaluated and discarded

A **block** is `{ ... }` containing newline-separated statements.

**Compound assignment.** `target op= value` is `target = target op value`, except
that if the target's type defines an operator overload `T::op=` it is called
instead: `target = T::op=(target, value)`. The target is read before it is
written, so `v.x += 1` and `v[0] += 1` work on packed values.

The reversed spellings are not operators and are diagnosed: `x =* 2`, `x =/ 2`,
and the rest are errors naming the `op=` to write (`'*' cannot start an
expression; did you mean '*='?`), and `x =+ 1` is an error because unary `+`
does not exist. `-` is the one real unary operator, so `x =- 1` is accepted — it
assigns the negation — but warns, since `-= 1` is almost always meant.

## 8. Expressions

Precedence, loosest to tightest:

1. `or`
2. `and`
3. `==`, `!=`, `<`, `>`, `<=`, `>=` (non-associative)
4. `|`
5. `^`
6. `&`
7. `<<`, `>>`
8. `+`, `-`
9. `*`, `/`, `%`
10. unary `-`, `not`, `~`
11. postfix: `.name`, `::name`, `(args)`, `[index]`
12. primary: number, string, path, `(expr)`, `{ ... }`, `[ ... ]`,
    function literal, `extern`

`and`, `or`, and `not` operate on integers: `0` is false, anything else is true.
`not` maps `0` to `1` and anything else to `0`; `and` and `or` yield `0` or `1`.
**`and` and `or` short-circuit**: the right operand is only evaluated when the
left does not decide the result, so `x != 0 and 1 / x > 2` is safe. Comparisons
yield `int` (`0` or `1`); strings compare bytewise. Integer division truncates
and `%` is the remainder; both need integer operands, and a zero divisor is a
runtime error.

Unary `-` negates; it widens `byte` to `int`, and negating an unsigned value is
an error (convert to a signed type first). Unary `+` is **not** part of the
language — `x =+ 1` reads as `= (+1)` — and neither is any other reversed
assignment spelling (see **Compound assignment** in §7).

**Bitwise operators** need integer operands (`&`, `|`, `^`, and `~` reject
floating point). `&`, `|`, and `^` promote like `+`: the wider operand's type
wins, and a signed/unsigned mix must be explicit. `~` keeps the operand's own
type. They bind **tighter than comparisons** — unlike C, `flags & MASK != 0` is
`(flags & MASK) != 0`.

**Shifts** (`<<`, `>>`) take the left operand's type; the count may be any
integer and is read as a 64-bit value. A count outside `0..width-1` for that
type is a compile error when it is constant (`1 << 32` on an `int`) and a
runtime error otherwise. `>>` copies the sign bit for signed values
(`int`, `int64`) and shifts in zeros for unsigned ones (`byte`, `uint`,
`uint64`).

## 9. Imports

`use "path"` loads another source file. A path is resolved relative to the file
containing the `use` (never to the working directory). A given file is loaded at
most once (pragma-once semantics): paths are normalized — `.`, `..` and `\` are
folded — before that check, so two spellings of one file are one file and an
import cycle (including one back to the entry script) is a no-op. A `use` is
only allowed at the top level of a file — in a block (a function body, an
`if`/`for` body) it is an error.

A file is compiled **after** everything it imports, so an imported file's
top-level definitions and initialisation run first:

```
// main.oby
use "helpers.oby"
run()          // helpers.oby has already defined `run`
```

Because each file is compiled after its imports, an imported file's names are
visible to the files that `use` it. A **type** name, however, cannot be
redefined: defining a type whose name already exists — a builtin like `int`, a
prelude type like `vec2i`, or an earlier one of your own — is an error
(`cannot redefine 'vec2i'`).

## 10. Type checking

- **Assignment / binding:** the value's type must be assignable to the target's.
- **Call:** the callee must be a function; arity and argument types are checked;
  the result type is the function's return type.
- **Return:** the returned value must be assignable to the enclosing function's
  return type, and a function that returns a value must return on every path —
  falling off the end is an error. A `while 1 { ... }` (or a `for` with a
  constant-true condition) with no `break` never falls through, so no trailing
  return is needed.
- **Member access:** `expr.member` resolves against `expr`'s type;
  `expr::member` against the type named by `expr`.
- **Numeric conversion** is value-preserving: a value fits a target when every
  value of the source type is representable in the target. Same signedness may
  widen (`byte` → `int` → `int64`, `uint` → `uint64`), and an unsigned value fits
  a strictly wider signed type (`uint` → `int64`). Everything else across
  signedness is **explicit** with a constructor: `uint(i)`, `int(u)`,
  `uint64(x)`. These wrap like `byte(300)` does — `uint(-1)` is `0xFFFFFFFF`.
- **Mixed arithmetic and comparison** follow the same rule: `uint + int64` is
  `int64` and `byte + int` is `int`, but `int + uint` is an error (`convert one
  side explicitly`) — as is `i < u`. Comparisons no longer compare a mixed
  signed/unsigned pair silently.
- **Integer literals adapt.** In a typed position a literal takes that type when
  its value fits: `uint u = 2`, `u + 1`, `byte b = 65`, `int64 x = 3000000000`.
  A value that does not fit is an error (`value 300 does not fit 'byte'`),
  never a truncation. With no expected type a literal is the smallest of `int`,
  `int64`, `uint64` that holds it.
- **Unsigned arithmetic wraps** (defined), like `byte`; there is no overflow
  checking. Unary `-` needs a signed operand: `-u` is an error and `int(u)` (or
  `uint(0) - u`) says what is meant. A `byte` operand widens to `int` there.
- **Floating point:** `float` and `double` are mutually assignable (a `double`
  may narrow to a `float`). An integer is assignable to a float at least as wide
  as it; a float is never assignable to an integer (that would be lossy). In
  arithmetic, if either operand is a float the result is a float — `double` if
  either operand is `double` or the integer is wider than the float.
- **Packed values** are assignable only to the same packed type. They are
  inline storage, so they cannot cross into tagged storage (object fields,
  natives, string interpolation) yet — that is an error, not a silent copy.
- **Comparisons** yield `int`. `==` and `!=` work on any two values of the same
  type; packed values compare by their bytes, and objects compare by identity.
  `<`, `>`, `<=`, `>=` need an ordered type (numbers, strings) — ordering packed
  values is an error.
- **A nominal type can define its own comparisons**: `::==` and `::<` take two of
  the type and return `int` (0/1), and the other four operators follow from them
  (`a != b` is `not T::==(a, b)`, `a > b` is `T::<(b, a)`, and so on). They apply
  when both operands have that type, so `List::sort` (which uses `<=`), `Map`
  keys (which use `==`) and `switch` cases all follow the type's own rule:

  ```
  Vec = type {
  	x = 0
  	y = 0
  	::== = (Vec a, Vec b) -> int { return a.x == b.x and a.y == b.y }
  	::< = (Vec a, Vec b) -> int { return a.x < b.x or (a.x == b.x and a.y < b.y) }
  }
  ```

  A type without them keeps the built-in `==` (identity for objects), and a type
  with `::==` but no `::<` cannot be ordered.
- **Null:** `null` is assignable to any tagged type and to nothing else, and a
  `null` literal cannot determine an inferred type. See §4.6.

## 11. Prelude

The compiler injects a single entry file, `src/prelude/core.oby`, before the
user script, so its definitions are available without a `use`. `core.oby`
`use`s whatever else it needs (for example `io.oby` and `Terminal.oby`), so the
prelude is one file plus its imports. The directory comes from
`OBY_PRELUDE_DIR` (default `src/prelude`; set in `compile.sh`). The prelude is
baked into the binary at build time (see [`prelude.md`](prelude.md)), so the
compiler and any binary built from it work from any working directory.

`core.oby` attaches the native numeric types' constructors (`int::new`,
`uint::new`, ...), defines `print` and `args`, and declares `object::free`:

```
`object::free = extern ob_free(object o) -> void
```

`object::free(v)` frees a heap value (array, string, or `{}` object). There is no
GC, so this is how memory is reclaimed. The value must not be used afterwards,
and must not be freed twice or through an alias. Values that are not heap
allocated (numbers, functions) are ignored. A view such as `s.buffer` is owned
by its string and is not freed.

`args()` is the command line as the process saw it: `args()[0]` is how the binary
was invoked — `./build/oby`, or the executable itself when the program is bundled
— then the script path and its arguments, so `./build/oby tool.oby a b` sees all
four. Nothing after the script path is interpreted by the compiler: a flag there
belongs to the script.

`byte::new` is the explicit narrowing conversion (`byte(300)` is `44`); the type
system never narrows implicitly. The signed ↔ unsigned constructors
(`int(u)`, `uint(i)`, `uint64(i64)`, ...) are natives for the same reason:
crossing signedness is always explicit (see §10). `uint::to_str` and
`uint64::to_str` format unsigned values (`int::to_str` takes a signed argument).

`vec.oby` defines the packed vector types `vec2i`, `vec3i` (int components) and
`vec2`, `vec3` (float components), each with `::new`, the binary operators, and
`::to_str`.

`String` is built in. Its intrinsic fields are `buffer` (`byte[]`, the backing
bytes) and `length` (`int`). `String.oby` adds `slice`, `from_bytes`,
`to_int`/`to_float`, and the search/split/trim kit (`find`, `find_from`,
`contains`, `starts_with`, `ends_with`, `split`, `trim`, `trim_start`,
`trim_end`, `is_blank`).

Output: `print` and the `Terminal` helpers write through one buffered stdout,
flushed at exit, before input, and by `io::flush()` (with `Terminal::flush()` as
an alias); `io::write` stays a raw `write(2)`.

The rest of the prelude (see `prelude.md`):

- `io.oby` — the file layer: descriptors and `io::Open` flags, buffered readers
  (`read_line`, `read_char`), `seek`/`tell`, and the filesystem (`exists`,
  `rename`, `mkdir`, `read_dir`, ...).
- `File.oby` — `File` handles (`open_read`, `open_append`, `read_line`, ...)
  and the `read_file`/`write_file` helpers.
- `Socket.oby` — TCP sockets (`Socket::connect`, `Socket::listen`, `accept`,
  `poll`, `set_nonblocking`) over the `net::` natives; a socket is a descriptor,
  so the `io` reads and writes work on it unchanged. Plaintext TCP only.
- `Terminal.oby` — terminal modes (raw mode, mouse reporting, `restore`), size,
  colours, cursor movement, and printing.
- `Input.oby` — the polled key set, key modifiers, ordered typed text, and
  mouse state.
- `Time.oby` — `Time::sleep_ms` and the monotonic `Time::now_ms` /
  `Time::now_us`.
- `List.oby`, `Map.oby`, `Set.oby`, `Pair.oby` — the generic containers
  ([§4.7](#47-generic-types)): `List<T>` (a growable `T[]` with
  `push`/`pop`/`at`/`set`/`insert`/`remove`/`index_of`/`contains`/`clone`/
  `clear`, plus `sort`/`sort_by`/`sorted` and `map`/`filter`/`fold`),
  `Map<K, V>` and `Set<T>` (linear scans over parallel arrays), and
  `Pair<A, B>`. They are ordinary generic types written in OBY — a script's own
  have the same power — and their names are the prelude's, so a script cannot
  declare its own `List`/`Map`/`Set`/`Pair`.

## 12. Standalone executables

`oby --bundle <script.oby> <output>` compiles a program — the prelude and every
file it `use`s included — into a standalone executable:

```
oby --bundle game/main.oby game
./game
```

The output is **a copy of `oby` with the compiled program appended**, so it
needs no OBY installation, no `src/prelude/`, no script files and no toolchain:
it compiles nothing at startup. Delete the sources and it still runs with its own
command line: `args()` is the executable followed by whatever it was given.
Imports are baked in at bundle time, so a `use` of something that does not exist
is a bundle-time error like any other diagnostic.

Because the artifact carries the toolchain, it is about **256 KB plus the
program** — the compiled prelude alone is ~17 KB, a small game ~30 KB — rather
than a few tens of KB. That is the price of needing nothing else: `oby` alone
bundles, and the result is one file. An image is tied to the build that
produced it: if the native library changes, an old bundle refuses to run rather
than calling into the wrong function. `chmod 0755` is applied on Unix.

## 13. Not yet implemented

The following appear in the older design but are **not** part of OBY today:

- Comparison operator overloads (`::==`, `::<`, ...) — comparisons are built in.
- `+` on strings — use interpolation (`"${a}${b}"`).
- Implicit narrowing conversions (`int(int64)`, `float` → `int`); only `byte(x)`
  is explicit.
- Packed values crossing tagged boundaries (object fields, natives,
  interpolation).
- Dynamic (non-constant) indexing of packed values.
