# OBY Language & Prelude: Architectural, Ergonomic, and Stylistic Review

This document presents an in-depth review of the OBY language syntax, semantic rules, and standard prelude implementation across `src/prelude/*.oby`.

---

## 1. Executive Summary

OBY aims to be a compact, statically typed scripting language compiled to stack-based bytecode. Its syntax is lean, modern, and deliberately uniform: almost all declarations follow the `target = value` pattern. It introduces compelling features such as byte-tight `packed` vector types with named component access and expressive string interpolation.

However, an examination of the language rules and `src/prelude/` reveals several areas of friction:
1. **Prelude Inconsistencies:** Discrepancies in naming conventions, method receiver syntax styles, member resolution (`.` vs `::`), string conversion nomenclature, and global scope hygiene.
2. **Grammar & Lexer Sensitivities:** Operator overload declaration syntax depends on whitespace tokens (`T::+ =` vs `T::+= =`).
3. **Semantic Surprises:** Type member initializers act as non-evaluating "type witnesses" rather than field defaults, and receiver parameters shadow the type name within method bodies.
4. **Type Ergonomics:** Absence of a distinct boolean type weakens API signatures, and manual memory management (`object::free`) without RAII creates tension with high-level features like closures and interpolation.

---

## 2. Inconsistencies & Flaws in `src/prelude`

### 2.1 Casing and Type Naming Conventions
* **Mixed Namespace/Type Casing:**
  * PascalCase: `File`, `Terminal`, `Input`, `String`.
  * Lowercase: `io` (`io = type { ... }`), `vec2i`, `vec3i`, `vec2`, `vec3`.
  * *Impact:* Lacks a unified convention for whether user types and namespaces must be PascalCase or lowercase.
* **Constant & Enum Casing:**
  * `Terminal::stdin`, `Terminal::stdout`, `Terminal::stderr` use lowercase names.
  * `io::READ`, `io::WRITE` use uppercase static names.
  * `Terminal::Color` uses uppercase enum members (`DEFAULT = -1`, `BLACK`, `RED`, ...).

### 2.2 Inconsistent String Conversion Method Names
* `src/prelude/core.oby` (L16):
  ```oby
  int::to_string = extern int_to_str(int i) -> String
  ```
* `src/prelude/vec.oby` (L52, L108, L157, L213):
  ```oby
  vec2i::to_str = (vec2i) -> String
  vec3i::to_str = (vec3i) -> String
  vec2::to_str = (vec2) -> String
  vec3::to_str = (vec3) -> String
  ```
* *Impact:* Code must alternate between calling `.to_string()` and `.to_str()` depending on whether the receiver is an integer or a vector. Standard library APIs should unify around one name.

### 2.3 Enum Member Access Syntax (`.` vs `::`)
* In `docs/oby.md` and `examples/patterns.oby`, enum constants are documented as static members accessed with `::`:
  ```oby
  if c == Color::RED { ... }
  ```
* In `src/prelude/Terminal.oby` (L46, L56) and games (`render.oby`), enum variants are accessed via dot notation `.`:
  ```oby
  if c == Terminal::Color.DEFAULT { ... }
  ```
* *Impact:* Having both `Type::Enum.MEMBER` and `Type::Enum::MEMBER` works due to compiler permissiveness, but leads to mixed idioms across codebases and documentation.

### 2.4 Receiver Parameter Syntax Fragmentation
OBY allows two ways to write method receivers:
1. **Anonymous / Type-as-Parameter:**
   ```oby
   vec2i::+ = (vec2i, vec2i b) -> vec2i {
       vec2i.x += b.x
       return vec2i
   }
   ```
2. **Explicit Parameter Name:**
   ```oby
   File::is_open = (File f) -> int {
       if f.fd < 0 { return 0 }
       return 1
   }
   ```
* *Impact:* Having prelude files split between these two styles produces arbitrary stylistic differences across core libraries.

### 2.5 Namespace Pollution & Pseudo-Namespaces
* **Empty `type` as Pseudo-Namespace:** Because OBY does not provide a dedicated `module` or `namespace` keyword, prelude files invent empty nominal types as namespace containers:
  * `Input = type {}` (in `src/prelude/input.oby`)
  * `Terminal = type { ... }` (in `src/prelude/Terminal.oby`)
  * `io = type { ... }` (in `src/prelude/io.oby`)
* **Unnamespaced Top-Level Globals:**
  * `src/prelude/math.oby` exports top-level `rand` rather than `Math::rand`.
  * `src/prelude/File.oby` exports top-level `read_file` and `write_file` directly into global scope alongside `File`.
  * `src/prelude/core.oby` exports top-level `version = 0` and `print`.

### 2.6 Weak Typing at Function Boundaries
* `src/prelude/input.oby` (L95):
  ```oby
  Input::is_key_pressed = extern input_is_key_pressed(int key) -> int
  ```
  The signature accepts a raw `int key` rather than the strongly-typed `Input::Key key` enum defined above it in the same file.
* `src/prelude/io.oby` (L9):
  ```oby
  io::open = extern io_open(String path, int mode) -> int
  ```
  Accepts raw `int mode` instead of an `io::Mode` or `io::Access` type.
* In contrast, `Terminal.oby` correctly enforces `Terminal::fg_code = (Terminal::Color c) -> int`.

### 2.7 Formatting & Style Divergence
* **Indentation:**
  * `core.oby` (L9–37) uses 2-space indentation for integer/float constructors (`  return n`).
  * `core.oby` (L48) and the remainder of all prelude files use tabs.
* **Punctuation Spacing:**
  * `core.oby` (L48): `io::write(Terminal::stdout,s.buffer,s.length)` lacks whitespace after commas.

---

## 3. Evaluation of Language Syntax & Grammar Ergonomics

### 3.1 What Works Well

1. **Clean, Assignment-Centric Grammar:**
   * Declaring everything via `=` creates an uncluttered, modern syntax:
     ```oby
     val = 10
     fn  = (int x) -> int { return x * 2 }
     Obj = type { x = 0 }
     Enum = enum { A, B }
     ```
2. **Packed Types as Inline Value Aggregates:**
   * Packed types (`packed int[2]`) bridge the gap between low-level performance and scripting ergonomics. Component projections (`v.x`, `v.y`) over stack-allocated fixed-size buffers provide zero-overhead vector math without heap allocation.
3. **String Interpolation:**
   * String interpolation `${expr}` is intuitive, readable, and avoids verbose formatting boilerplates.

---

### 3.2 Syntactic & Semantic Pitfalls

1. **Receiver Identifier Shadows the Enclosing Type:**
   * In `Point::translate = (Point, int dx, int dy) -> Point`, the token `Point` is rebound as a local variable representing the receiver instance.
   * *Problem:* The type `Point` can no longer be referred to inside its own method body to invoke static helpers or constructors (e.g. `Point::new` or constructing another `Point` value).
   * *Recommended Alternative:* Dedicated receiver keyword (`self` / `this`) or requiring named receivers (`(Point self, int dx, int dy)`).

2. **Whitespace-Sensitive Operator Overload Definitions:**
   * Defining `+` requires `T::+ = ...` (a mandatory space between `+` and `=`), while compound assignment requires `T::+= = ...`.
   * *Problem:* Having grammar semantics hinge on whitespace between two adjacent punctuation characters is brittle and error-prone.

3. **Type Initializers as "Type Witnesses" Instead of Defaults:**
   * In `T = type { x = 10 }`, writing `10` does *not* provide a default value. An instantiated `T t = {}` leaves `x` zero-initialized (`0`).
   * *Problem:* In modern programming languages, field assignments in type declarations set default initial values. Re-purposing `=` purely for type inference ("type witness") violates developer intuition.

4. **Lack of a First-Class `bool` Type:**
   * Conditions, comparisons, and status queries (`File::is_open`, `Input::poll`) return `int` (`1` or `0`).
   * *Problem:* Weakens static type checking and makes boolean logic prone to accidental arithmetic operations.

5. **Structural vs. Nominal Object Duality:**
   * `{ ... }` generates an anonymous structural heap object, while `type { ... }` declares a nominal type.
   * *Problem:* A `{}` literal only adopts the nominal type when inferred from explicit context (`Point p = { x = 1 }`), whereas `p = { x = 1 }` creates an unconstrained structural object. This duality creates subtle behavioral differences across assignment boundaries.

6. **Manual Memory Management in a High-Level Scripting Model:**
   * OBY combines high-level concepts (closures, structural objects, string interpolation) with manual C-style deallocation (`object::free`).
   * *Problem:* Closures that capture heap state create untracked allocations with no lifetime owners, leading to leaks or dangling references unless manual tracking is maintained.

---

## 4. Summary Table of Prelude Audit

| File | Primary Issues Identified |
|---|---|
| `core.oby` | Mixed 2-space vs tab indentation; missing spacing after commas; `int::to_string` name differs from `vec::to_str`. |
| `File.oby` | Top-level globals `read_file` / `write_file` pollute root scope; uses named receiver `(File f)` unlike `vec.oby`. |
| `String.oby` | No native `String` constructor in prelude; uses `to_int`/`to_float` rather than unified conversion names. |
| `Terminal.oby` | Inconsistent casing (`stdin` vs `DEFAULT`); uses `Terminal::Color.DEFAULT` (dot) instead of `::`; pseudo-namespace `type`. |
| `input.oby` | `Input::is_key_pressed` takes raw `int` instead of `Input::Key` enum; empty `Input = type {}` used solely as namespace. |
| `io.oby` | Lowercase type name `io`; raw `int mode` parameter; static integer constants instead of enum. |
| `math.oby` | Exports top-level `rand` into global namespace rather than `Math::rand`. |
| `vec.oby` | Lowercase type names (`vec2i`, `vec3`); uses `to_str` instead of `to_string`; uses anonymous receiver binding `(vec2i)`. |

---

## 5. Recommended Next Steps

1. **Standardize Prelude Naming & Style:**
   * Pick either `to_string` or `to_str` for all types.
   * Decide on a uniform receiver parameter convention (e.g. always use `(Type self)`).
   * Standardize all type and namespace names to PascalCase (`Io`, `Vec2i`, `Vec3i`, `Math`).
2. **Clarify Enum Syntax:**
   * Standardize enum variant access across documentation and code to `Type::Variant` (or `Type.Variant`).
3. **Enhance Type Safety at API Boundaries:**
   * Use enum types for function signatures in `input.oby` (`Input::Key`) and `io.oby` (`Io::Mode`).
4. **Refine Language Rules:**
   * Introduce `self` or avoid type shadowing in receiver definitions.
   * Consider introducing an explicit `bool` primitive.
   * Allow type field initializers to serve as actual default values when nominal objects are instantiated.
