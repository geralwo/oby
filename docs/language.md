# The OBY Language Handbook

Welcome to OBY — a compact, statically typed scripting language engineered for high performance, byte-tight memory efficiency, and predictable execution on a register-based virtual machine.

---

## 1. Philosophy & Core Principles

1. **Uniform Assignment-Centric Grammar:**
   Almost every declaration in OBY uses the `=` operator:
   ```oby
   x = 42                                     // Variable
   Point = type { x = 0, y = 0 }              // Nominal type
   vec2i = packed int[2] { x = vec2i[0] }     // Packed aggregate
   Color = enum { RED, GREEN, BLUE }          // Enum
   add = (int a, int b) -> int { return a + b } // Function
   Point::translate = ...                     // Method
   ```

2. **Static Typing with Local Type Inference:**
   Every binding and expression has a type known before bytecode generation. Explicit types can be declared (`int x = 5`), but inferred bindings (`x = 5`) keep code clean.

3. **Byte-Tight Inline Storage:**
   Packed types (`vec2i`, `vec3i`, `float`, `int`) and object fields live directly inline on the stack or within an object's heap buffer without boxing overhead.

4. **Register-Based Virtual Machine:**
   OBY compiles to 64-bit 3-address instructions (`ADD_INT a b c`) evaluated against a byte-addressed frame, eliminating stack push/pop dispatch bottlenecks.

5. **Explicit Boundaries & Scoping:**
   - Crossing signedness is explicit (`uint(i)`, `int(u)`).
   - Global variables accessed inside functions must be explicit (`$name`).
   - Narrowing numeric conversions are explicit (`byte(300)`).

---

## 2. Lexical Structure & Conventions

### 2.1 File & Statement Rules
* Statements are terminated by **newlines**. There are no semicolons.
* A newline is ignored if the preceding token cannot end a statement (e.g. `,`, `+`, `-`, `*`, `(`, `[`, `->`, `=`, `.`, `::`).
* Comments start with `//` and extend to the end of the line.

### 2.2 Naming Conventions
* **Nominal Types & Namespaces:** PascalCase (`File`, `Terminal`, `Input`, `String`). *Exception: `io` is lowercase for brevity (`io::read`).*
* **Primitive & Packed Types:** Lowercase (`int`, `uint`, `byte`, `float`, `vec2i`, `vec3`).
* **Variables & Functions:** `snake_case` (`player_name`, `move_to`, `read_line`).
* **Constants & Enum Members:** `SCREAMING_SNAKE_CASE` (`Input::Key::ESCAPE`, `io::Open::READ`).

---

## 3. Variables, Scoping & Bindings

### 3.1 Local and Inferred Bindings
```oby
x = 10                // Inferred as int
String name = "hero"  // Explicitly typed
local temp = 20       // Block-scoped binding (shadows outer scopes)
```

### 3.2 Global Scoping (`$name`)
At file scope, `name = value` declares a global. Inside a function body:
* A bare identifier is **always** a local variable, a parameter, or a global function.
* To read or write a global variable inside a function, prefix it with `$`:

```oby
score = 0

add_score = (int pts) {
    $score += pts      // Modifies the global variable
}

get_score = () -> int {
    return $score      // Reads the global variable
}
```

---

## 4. Primitive & Value Types

| Type | Description | Size | Representation |
|---|---|---|---|
| `byte` | Unsigned 8-bit integer ($0 \dots 255$) | 1 B | Inline (packed) |
| `int` | Signed 32-bit integer | 4 B | Inline (packed) |
| `uint` | Unsigned 32-bit integer | 4 B | Inline (packed) |
| `int64` | Signed 64-bit integer | 8 B | Inline (packed) |
| `uint64` | Unsigned 64-bit integer | 8 B | Inline (packed) |
| `float` | 32-bit IEEE 754 float | 4 B | Inline (packed) |
| `double` | 64-bit IEEE 754 float | 8 B | Inline (packed) |
| `String` | UTF-8 / byte-buffer string | 16 B | Tagged reference |
| `object` | Generic dynamic object reference | 16 B | Tagged reference |
| `void` | Absence of a return value | 0 B | None |

### 4.1 Literals
* **Integers:** Decimal (`42`), Hexadecimal (`0x2A`), Binary (`0b101010`).
* **Characters:** Single-byte character literals `'A'`, `'\n'`, `'\x1B'` (type is `byte`).
* **Floats:** `3.14`, `1.0e-3`.
* **Strings & Interpolation:** Escaped strings `"hello\n"` and interpolated strings `"${player} scored ${score}!"`.

---

## 5. Packed Aggregate Types

Packed types are fixed-width inline byte buffers with named member projections. They are stack-allocated and carry zero heap or pointer overhead.

```oby
vec2i = packed int[2] {
    x = vec2i[0]
    y = vec2i[1]
}

vec2i::new = (int x, int y) -> vec2i {
    return [x, y]
}

// Construction & zero initialization
v = vec2i(10, 20)
v.x += 5             // Modifies 4 bytes in place
zero = vec2i()       // Zero-initialized intrinsic (0, 0)
```

---

## 6. Objects: Nominal & Structural

### 6.1 Nominal Types (`type { ... }`)
Nominal types have a fixed layout and field defaults:

```oby
Player = type {
    name = "Player 1" // Default value
    hp = 100
    pos = vec2i(0, 0) // Packed field stored inline (8 bytes)

    ::new = (String name, int hp) -> Player {
        return { name = name, hp = hp }
    }
}

p1 = Player("Aria", 120)
p2 = Player{}             // Takes declared field defaults (name="Player 1", hp=100)
```

### 6.2 Structural Dynamic Objects (`{ ... }`)
Anonymous objects create standalone structural types:
```oby
config = {
    volume = 80
    fullscreen = 1
}
config.volume = 90
```

---

## 7. Arrays

OBY supports fixed-size (`T[n]`) and growable (`T[]`) arrays. Arrays have reference semantics.

```oby
// Growable array literal
nums = [1, 2, 3, 4]
nums.push(5)
last = nums.pop()        // 5
nums.insert(0, 99)       // Inserts at index 0
removed = nums.remove(0) // Removes element at index 0
len = nums.length        // Signed int

// Fixed-size array
int[3] fixed_nums = [10, 20, 30]

// Repeat literal: [value; count]
grid = [0; 64]           // Array of 64 zeroes
```

---

## 8. Functions, Methods & Closures

### 8.1 First-Class Functions
```oby
add = (int a, int b) -> int {
    return a + b
}

// Void return (no -> required)
log_msg = (String msg) {
    print("[LOG] ${msg}\n")
}
```

### 8.2 Methods & Receivers
Static functions whose first parameter matches the type can be called as instance methods:

```oby
Player::take_damage = (Player self, int dmg) {
    self.hp -= dmg
}

p = Player("Knight", 100)
p.take_damage(25)     // Desugars to Player::take_damage(p, 25)
```

### 8.3 Closures
Functions capture enclosing local bindings **by value (read-only)**:

```oby
make_adder = (int n) -> (int) -> int {
    return (int x) -> int { return x + n }
}

add5 = make_adder(5)
result = add5(10)     // 15
```

---

## 9. Operator Overloading

Types can overload binary and compound operators:

```oby
vec2i::+ = (vec2i, vec2i b) -> vec2i {
    vec2i.x += b.x
    vec2i.y += b.y
    return vec2i
}

a = vec2i(1, 2)
b = vec2i(3, 4)
c = a + b             // vec2i(4, 6)
```

Supported overloadable operators:
`+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `<<`, `>>`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`.

---

## 10. Enums

Enums declare named constants with an underlying type:

```oby
Direction = enum {
    UP
    DOWN
    LEFT
    RIGHT
}

dir = Direction::UP

// Custom explicit values / bitmasks
Flags = enum {
    NONE = 0
    READ = 1
    WRITE = 2
    EXEC = 4
}
```

---

## 11. Control Flow

### 11.1 Conditionals (`if`, `else`)
```oby
if score >= 100 {
    print("Victory!\n")
} else if score > 50 {
    print("Close!\n")
} else {
    print("Try again!\n")
}
```

### 11.2 Loops (`while`, `for`, `break`, `continue`)
```oby
// While loop
i = 0
while i < 10 {
    i += 1
    if i == 5 { continue }
}

// C-style for loop (local variable scoped to loop)
for (local k = 0; k < 10; k += 1) {
    print("${k}\n")
}
```

### 11.3 `switch` Statements
```oby
switch dir {
case Direction::UP {
    move_up()
}
case Direction::DOWN {
    move_down()
}
default {
    stop()
}
}
```

---

## 12. Standard Prelude & System Libraries

The prelude is automatically imported into every script (`core.oby`):

* **`Terminal`:**
  * `Terminal::raw_mode(1)` / `Terminal::restore()`
  * `Terminal::get_width()` / `Terminal::get_height()`
  * `Terminal::move_to(x, y)`, `Terminal::set_color(fg, bg)`
  * `Terminal::Color` enum (`RED`, `GREEN`, `BLUE`, etc.)
* **`Input`:**
  * `Input::poll()` / `Input::wait(ms)`
  * `Input::is_key_pressed(Input::Key::SPACE)`
  * `Input::mods(key)` (`Input::Mod::CTRL`, `SHIFT`, `ALT`)
  * `Input::text() -> String` (typed characters in arrival order)
  * `Input::mouse_x()`, `Input::mouse_y()`, `Input::mouse_clicked(btn)`
* **`Time`:**
  * `Time::sleep_ms(ms)`
  * `Time::now_ms() -> int64` (monotonic millisecond timer)
* **`io` & `File`:**
  * `read_file(path) -> String` / `write_file(path, data) -> int`
  * `File::open_read(path)`, `File::open_write(path)`, `File::read_line()`
  * `io::exists(path)`, `io::mkdir(path)`, `io::read_dir(path)`
* **`Memory`:**
  * `object::free(heap_val)` (manual deallocation of strings, arrays, objects)
