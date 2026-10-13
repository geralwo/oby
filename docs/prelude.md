# The OBY prelude

The prelude is OBY code the compiler injects before the user script, so its
definitions are available without a `use`. It lives in `src/prelude/`, and
`compile.sh` bakes it into the binary: `tools/embed_gen.c` turns the directory
into `build/prelude_embed.c`, a table of file contents (`include/core/embed.h`).
The compiler consults that table before the disk, so a built `oby` works from
any working directory and does not need `src/prelude/` next to it. Editing the
prelude means rebuilding — the embedded copy shadows the file on disk.
`OBY_PRELUDE_DIR` (default `src/prelude`, set in `compile.sh`) is both the
table's path prefix and the fallback for a file the table does not have.

`core.oby` is the entry point. It `use`s whatever else it needs, so the prelude
is one file plus its imports. Imports are pragma-once, and the whole prelude is
typechecked like any other source — an error in it is reported with a
`note: imported from ...` chain.

A script does **not** `use` the prelude; it is already there. (`use` resolves
relative to the importing file, so `use "core.oby"` from elsewhere would not find
it.)

**Prelude functions take their state as parameters.** A bare name inside a
function is a local, a parameter, or a function — never a global *variable*,
which would be written `$name` ([§3](../oby.md#3-names-and-paths)). A helper
therefore cannot accidentally touch a script's globals; prelude temporaries are
declared `local` for clarity and shadowing.

**Layout.** The directory is flat, one file per unit. A file that defines a
PascalCase namespace or type is named after it (`Terminal.oby`, `Input.oby`,
`File.oby`, `String.oby`); lowercase files hold lowercase definitions or loose
top-level functions (`core.oby`, `io.oby`, `vec.oby`, `math.oby`).

## What ships

**Conventions.** Built-in primitives and packed types are lowercase (`int`,
`byte`, `float`, `vec2i`); object types and namespaces are PascalCase (`File`,
`Terminal`, `Input`). `io` is the one lowercase namespace — kept because
`io::write` reads well and appears everywhere. A few conveniences live at top
level rather than behind a namespace: `print`, `args`, `rand`, `read_file`,
`write_file`.

### `core.oby`

| Definition | Meaning |
|------------|---------|
| `object::free(object) -> void` | free a heap value (`extern ob_free`) |
| `byte::new(int) -> byte` | explicit narrowing (`extern byte_from_int`) |
| `int::new(int) -> int` | identity |
| `int::new(uint) -> int` | explicit signedness flip (`extern int_from_uint`) |
| `uint::new(uint) -> uint` | identity |
| `uint::new(int) -> uint`, `::new(int64)`, `::new(uint64)` | explicit flip / truncation (`extern uint_from_*`) |
| `int64::new(int64) -> int64` | identity |
| `int64::new(uint64) -> int64` | explicit signedness flip (`extern int64_from_uint64`) |
| `uint64::new(uint64) -> uint64` | identity |
| `uint64::new(int64) -> uint64` | explicit signedness flip (`extern uint64_from_int64`) |
| `int::to_str(int) -> String` | `extern int_to_str` |
| `uint::to_str(uint) -> String` | unsigned formatting (`extern uint_to_str`) |
| `uint64::to_str(uint64) -> String` | `extern uint64_to_str` |
| `int64::to_str(int64) -> String` | `extern int64_to_str` |
| `print(String)` | buffered write to stdout (`io::print`); `io::flush()` makes it visible |
| `args() -> String[]` | the process's command line: how the binary was invoked, then the script and its arguments (`extern io_args`) |

Crossing signedness is never implicit, so the constructors that do it are
natives: `uint(i)`, `int(u)`, `uint64(i64)`, `int64(u64)`, and `uint` from the
wider integer types.

The numeric types themselves are **built in** (the compiler knows their
representation); the prelude only attaches constructors and methods to their
names. `print` is:

```
print           = extern io_print(String s) -> void
```

so it takes a `String`; format other values first (for example with
`int::to_str` or `${...}` interpolation).

`print` and the terminal escape natives share one **buffered stdout** (a redraw
costs a copy per cell, not a syscall). The buffer is flushed when it fills, on a
newline when stdout is a terminal (so prompts appear as typed), before reading
input, before a raw `io::write` to stdout, and at exit. `io::flush()`
forces it; `io::write` stays a raw `write(2)`.

### `io.oby`

The file layer: descriptors, buffered readers, and the filesystem. `io` is a
type holding the natives as statics — a namespace, like `Terminal` and `Input`,
so the name cannot be rebound to an object without those members. It is the one
lowercase namespace.

Calls that fail follow C: they report `-1` or `null`, and `io::last_error()`
reads the `errno` the failing call left (`ENOENT` after missing a file, and so
on). A later success does not clear it, so read it right after the failure.

```
io::stdin = 0 / io::stdout = 1 / io::stderr = 2

io::Open = enum {
	READ = 0          // access bits, low two: match O_RDONLY/O_WRONLY/O_RDWR
	WRITE = 1
	READ_WRITE = 2
	CREATE = 4
	TRUNCATE = 8
	APPEND = 16
	EXCLUSIVE = 32
}

io::Seek = enum { SET = 0, CUR = 1, END = 2 }

io::open        = extern io_open(String path, io::Open flags) -> int
io::close       = extern io_close(int fd) -> int
io::read        = extern read(int fd, byte[] buf, int count) -> int
io::read_full   = extern io_read_full(int fd, byte[] buf, int count) -> int
io::read_string = extern io_read_string(int fd, int count) -> String
io::read_bytes  = extern io_read_bytes(int fd, int count) -> byte[]
io::write       = extern write(int fd, byte[] buf, int count) -> int
io::print       = extern io_print(String s) -> void          // `print` aliases this
io::read_line   = extern io_read_line(int fd) -> String
io::read_char   = extern io_read_char(int fd) -> int
io::read_all    = extern io_read_all(int fd) -> String
io::flush       = extern io_flush() -> void
io::seek        = extern io_seek(int fd, int64 offset, io::Seek whence) -> int64
io::tell        = extern io_tell(int fd) -> int64
io::size        = extern io_size(String path) -> int64     // also io_size(int fd)
io::exists      = extern io_exists(String path) -> int
io::is_dir      = extern io_is_dir(String path) -> int
io::is_tty      = extern io_is_tty(int fd) -> int
io::remove      = extern io_remove(String path) -> int     // unlink
io::rename      = extern io_rename(String from, String to) -> int
io::mkdir       = extern io_mkdir(String path) -> int
io::rmdir       = extern io_rmdir(String path) -> int
io::read_dir    = extern io_read_dir(String path) -> String[]
io::cwd         = extern io_cwd() -> String
io::getenv      = extern io_getenv(String name) -> String
io::last_error  = extern io_last_error() -> int
```

Semantics worth knowing:

- `open` flags combine with `|`; the low two bits are the access mode and match
  `open(2)`. It returns a descriptor or `-1`.
- `read` is `read(2)`: it may return fewer bytes than asked (a short read is
  normal on pipes). `read_full` fills the buffer and stops only at the end of
  the file; `read_string`/`read_bytes` do the same into a fresh value.
- `read_line` gives the line *including* its newline — the exact bytes, so `write`
  gives them back unchanged — `null` at the end of the file; `read_char` gives
  one byte or `-1`. Both share a per-fd buffer with `read`, so they can be mixed
  on one handle.
- `seek`/`tell` account for bytes the readers have buffered: `tell` is the
  logical position, not the read-ahead one.
- `size` takes a path or an open descriptor; `read_dir` returns entry names
  (no `.`/`..`) in readdir order.
- `write` is a raw `write(2)`. The only buffered stream is stdout: `io::print`
  (which `print` and `Terminal::print` alias) appends to it, and `io::flush`
  makes it visible now. `write`/`read` on stdout/stdin flush it first, so
  `print` and `io::write` can be mixed safely.

### `String.oby`

Methods on the built-in `String` (call them as `s.slice(1, 3)`):

| Definition | Meaning |
|------------|---------|
| `String::slice(String, int start, int end) -> String` | a copy of `s[start, end)`; bounds are clamped |
| `String::from_bytes(byte[], int start, int count) -> String` | a String built from bytes |
| `String::to_int(String) -> int` | parses the whole text (decimal, `0x` hex, `0b` binary, `_` separators); not a number is `0` |
| `String::to_float(String) -> float` | the same, via `strtod` |
| `String::find(String, String) -> int` | the first index of the needle, or -1 |
| `String::find_from(String, String, int from) -> int` | the same, starting at `from` |
| `String::contains` / `starts_with` / `ends_with` | `1` / `0` |
| `String::split(String, String sep) -> String[]` | the parts around each `sep` (empty parts kept) |
| `String::trim` / `trim_start` / `trim_end` | without leading/trailing whitespace |
| `String::is_blank(String) -> int` | `1` when empty or only whitespace |
| `String::to_lower` / `to_upper` | ASCII case conversion; other bytes copy |
| `String::replace(String, String from, String to) -> String` | every occurrence of `from`, left to right |
| `String::join(String[] parts, String sep) -> String` | the parts joined with `sep` |
| `String::lines(String) -> String[]` | split on `\n`; drops `\r` (CRLF), no empty last line |
| `String::chomp(String) -> String` | without one trailing line ending (`\n`, or `\r\n`); nothing else is touched |

```
s = "hello, world"
s.slice(0, 5)      // "hello"
"42".to_int()      // 42
"0x1F".to_int()    // 31
```

Searching, splitting and trimming are written in OBY over `buffer`/`length`
(`str_slice` does the copying):

```
comma = ","
s.find(comma)          // 5
s.find_from("l", 4)    // 10
s.split(comma)         // ["hello", " world"]
"  x ".trim()          // "x"
```

`String` already carries `buffer` (`byte[]`) and `length` (`int`), and `==` /
`<` compare contents.

### `File.oby`

A file handle on top of `io`, plus whole-file helpers. A failed open produces a
closed `File` (`fd == -1`) rather than an error, and `read_file` gives `null`.
`fd = -1` is the member default, so a freshly constructed `File` is closed.

| Definition | Meaning |
|------------|---------|
| `File::open_read(String) -> File` | `READ` |
| `File::open_write(String) -> File` | `WRITE \| CREATE \| TRUNCATE` |
| `File::open_append(String) -> File` | `WRITE \| CREATE \| APPEND` |
| `File::open_read_write(String) -> File` | `READ_WRITE` |
| `File::is_open(File) -> int` | `1` while the descriptor is valid |
| `File::read(File, int count) -> String` | fills to `count` or the end of the file; `""` at end of file, `null` on error |
| `File::read_bytes(File, int count) -> byte[]` | the same read, as raw bytes; `null` at end of file |
| `File::read_line(File) -> String` | one line including its newline; `null` at end of file (`String::chomp` drops the ending) |
| `File::read_char(File) -> int` | one byte; `-1` at end of file |
| `File::read_all(File) -> String` | the rest of the file |
| `File::write(File, String) -> int` / `::write_line(File, String) -> int` | bytes written |
| `File::write_bytes(File, byte[], int) -> int` | writes from a byte buffer |
| `File::seek(File, int64, io::Seek) -> int64` / `::tell(File) -> int64` | position, with buffered bytes accounted for |
| `File::size(File) -> int64` | size in bytes |
| `File::close(File)` | closes and marks it closed |
| `read_file(String) -> String` | whole file, or `null` if it cannot be opened |
| `write_file(String path, String) -> int` | writes (create/truncate), returns bytes written |

```
f = File::open_append("log.txt")
f.write_line("started")
f.close()

text = read_file("out.txt")
if text == null { print("missing\n") }
```

### `Socket.oby`

TCP sockets on top of `io`: a socket is a file descriptor, so `read`/`write`/
`io::close` work on it unchanged, and this type is the ergonomic wrapper the way
`File` is for files. A failed connect or listen produces a closed `Socket`
(`fd == -1`), and `io::last_error()` says why. The `net::` natives underneath
(`net::connect`, `net::listen`, `net::accept`, `net::local_port`,
`net::resolve`, `net::set_nonblocking`, `net::poll`) take and return descriptors,
so they can be used directly.

| Definition | Meaning |
|------------|---------|
| `Socket::connect(String host, int port) -> Socket` | a connected socket; `""` is the local machine |
| `Socket::listen(String host, int port) -> Socket` | a listening socket (backlog 16); `""` is any interface, port 0 picks a free one |
| `Socket::local_port(Socket) -> int` | the port it is bound to |
| `Socket::resolve(String host) -> String` | the first address, as text; `null` when it does not resolve |
| `Socket::accept(Socket) -> Socket` | the next client of a listener |
| `Socket::is_open(Socket) -> int` | `1` while the descriptor is valid |
| `Socket::read(Socket, int count) -> String` | the io reads, on the socket (`::read_bytes`, `::read_line`, `::read_all`) |
| `Socket::write(Socket, String) -> int` | bytes written (`::write_bytes(Socket, byte[], int)`) |
| `Socket::poll(Socket, int timeout_ms) -> int` | `1` readable, `0` on timeout, `-1` on error |
| `Socket::set_nonblocking(Socket, int on) -> int` | so a game loop need not block |
| `Socket::close(Socket)` | closes and marks it closed |

```
srv = Socket::listen("", 0)
print("on port ${srv.local_port()}\n")
cli = Socket::connect("127.0.0.1", srv.local_port())
peer = srv.accept()
cli.write("ping\n")
print(peer.read_line())
```

TLS is not part of this: the layer is plaintext TCP.

### `Terminal.oby`

```
Terminal = type {}
```

The standard descriptors live on `io` (`io::stdin`, `io::stdout`,
`io::stderr`).

| Definition | Meaning |
|------------|---------|
| `Terminal::raw_mode(int on)` | keys arrive immediately and without echo |
| `Terminal::mouse_mode(int on)` | SGR mouse reporting on/off (events on `Input`) |
| `Terminal::restore()` | undo all modes; also runs at exit |
| `Terminal::get_width()` / `::get_height()` | size in cells; `0` when not a terminal |
| `Terminal::move_to(int x, int y)` | move the cursor; `(0, 0)` is the top-left |
| `Terminal::set_color(int fg, int bg)` | set the SGR colours (codes from `fg_code`/`bg_code`) |
| `Terminal::hide_cursor()` / `::show_cursor()` | |
| `Terminal::Color` | an enum: `DEFAULT = -1`, then `BLACK` … `LIGHTWHITE` numbered from `0` |
| `Terminal::print(String)` | an alias for `io::print` (buffered; see `io::flush`) |
| `Terminal::flush()` | an alias for `io::flush()` |
| `Terminal::fg_code(Color) -> int` / `bg_code(Color) -> int` | ANSI SGR codes |
| `Terminal::printc(String, Color fg, Color bg)` | print with colors |
| `Terminal::print_at(int x, int y, String)` | move the cursor and print |
| `Terminal::clear()` | clear the screen |

### `vec.oby`

Packed vector types, byte-tight inline storage:

| Type | Layout | Components |
|------|--------|------------|
| `vec2i` | `packed int[2]` | `x`, `y` |
| `vec3i` | `packed int[3]` | `x`, `y`, `z` |
| `vec2` | `packed float[2]` | `x`, `y` |
| `vec3` | `packed float[3]` | `x`, `y`, `z` |

Each type defines `::new` (component-wise), the binary operators `::+`, `::-`,
`::*`, `::/` (vector-by-vector, plus `*`/`/` by a scalar: `int` for
`vec2i`/`vec3i`, `float` for `vec2`/`vec3`), and `::to_str`:

```
pos = vec2i(1, 2)
pos += vec2i(10, 20)
print(vec2i::to_str(pos))      // (11, 22)

vel = vec2(1.0, 2.0)
print(vec2::to_str(vel * 2.5)) // (2.5, 5)
```

The methods use the **bare-receiver** form (`(vec2i)`), so the body names the
receiver by the type — `vec2i.x`. Both forms can reach `vec2i::new`, so this is a
style choice ([`oby.md` §6](oby.md)).

`vec2i()` / `vec3i()` / `vec2()` / `vec3()` zero-initialize (the intrinsic
zero value of a packed type).

### `List.oby`, `Map.oby`, `Set.oby`, `Pair.oby`

The generic containers ([`oby.md` §4.7](oby.md)). Each is a plain generic type,
so a script's own containers have the same power:

| Type | What it is | API |
|------|------------|-----|
| `List<T>` | growable list over a `T[]` | `push`, `pop`, `at`, `set`, `insert`, `remove`, `index_of`, `contains`, `length`, `is_empty`, `clear`, `clone`, `sort`, `sort_by(cmp)`, `sorted`, `map(f)`, `filter(keep)`, `fold(init, f)`, `List<T>::with(T[] xs)` |
| `Map<K, V>` | parallel `K[]`/`V[]`, searched linearly | `set`, `get(k, fallback)`, `has`, `remove`, `index_of`, `key_at`, `value_at`, `length`, `clear` |
| `Set<T>` | distinct values over a `T[]` | `add`/`remove` (returning whether it changed), `has`, `index_of`, `at`, `length`, `is_empty`, `clear`, `clone`, `Set<T>::with(T[] xs)` |
| `Pair<A, B>` | a two-element tuple | `Pair<A, B>::of(a, b)`, `first`, `second` |

```
lives = List<int>()
lives.push(3)
lives.push(2)
print(lives.pop())                  // 2

names = Map<String, int>()
names.set("ada", 36)
print(names.get("ada", 0))          // 36
```

`pop`, `at`, `key_at`, `value_at` and `remove` follow the array rules: an index
out of range, or popping an empty list, is a runtime error.

A map lookup needs only `==` on `K`, so a packed key type stays packed (no
boxing) — and the scan is O(n), which is fine at symbol-table sizes. Hashing
waits for generic bounds, which the language does not have yet.

The methods live inside the type body: a method written outside it would have to
name the type's own arguments (`List<T>::push = ...`), which generic
declarations do not support.

`sort` uses `<` on the element type, so instantiating it with a type that has no
ordering is an error; `sort_by(cmp)` takes a comparator instead (`cmp(a, b)`
negative when `a` belongs first).

`map`, `filter` and `fold` are methods with the method's own type parameters
where they need them, so `map` can change the element type:

```
xs = List<int>::with([1, 2, 3])
xs.map((int x) -> int { return x * 2 })            // [2, 4, 6]
xs.map((int x) -> String { return int::to_str(x) }) // a List<String>
xs.fold(0, (int a, int x) -> int { return a + x })  // 6
```

### `Input.oby`

Input is a polled key set plus ordered text and mouse state, not an event
stream. `Input::poll` is the frame tick: it drains everything the terminal has
buffered and starts a new batch; every query answers about that batch.

```
Input::poll()                            // drain pending input; 1 if anything arrived
Input::is_key_pressed(Input::Key::UP)    // did it arrive since the last poll?
Input::wait(1000)                        // the same, but block first (ms; <0 waits forever)
Input::text()                            // bytes typed since poll, in order; null if none
Input::mods(Input::Key::Q)               // modifier bits it arrived with
```

`Input::Key` is an enum of key codes: ASCII for printable keys (`A` = 97,
`ENTER` = 13, `ESCAPE` = 27), `256+` for the special ones (`UP` = 256 …
`F12` = 277). Escape sequences are decoded by the native, so the script never
sees them; letters match either case. Ctrl+letter and Shift+letter fold to the
letter plus a bit in `Input::Mod` (`SHIFT`/`CTRL`/`ALT`), so modifiers are
queryable:

```
if Input::is_key_pressed(Input::Key::UP) and
   (Input::mods(Input::Key::UP) & Input::Mod::CTRL) != 0 { ... }
```

`Input::text` is the one channel a key set cannot express: the printable bytes
in arrival order, which is what typing and paste need. In canonical mode (no
`raw_mode`), `poll` + `text()` reads a line with the terminal's own editing —
the cheap prompt path that needs no editor. The state lives in the native — the
script cannot see or corrupt it.

A game loop polls once per frame:

```
Terminal::raw_mode(1)
while alive {
    Input::poll()
    if Input::is_key_pressed(Input::Key.Q) { alive = 0 }
    tick()
    Time::sleep_ms(120)
}
Terminal::restore()
```

Mouse: `Terminal::mouse_mode(1)` turns SGR reporting on. Coordinates are
0-based, matching `move_to`; `Input::Mouse` names the buttons.

```
Input::poll()
if Input::mouse_clicked(Input::Mouse::LEFT) {
    x = Input::mouse_x()
    y = Input::mouse_y()
}
```

`mouse_clicked`/`mouse_released` are edges since the last poll, `mouse_down` is
the held state, `mouse_dragged` is motion while a button is held, and
`mouse_wheel` is net ticks (+ is up).

### `Time.oby`

| Definition | Meaning |
|------------|---------|
| `Time::sleep_ms(int ms)` | sleep for `ms` milliseconds |
| `Time::now_ms() -> int64` | a monotonic clock in milliseconds; only differences are meaningful |
| `Time::now_us() -> int64` | the same clock in microseconds (`now_us() / 1000` is `now_ms()`) |

### `math.oby`

| Definition | Meaning |
|------------|---------|
| `rand(int n) -> int` | a pseudo-random value in `0..n-1` (`extern ob_rand`), seeded from the clock |

### `Json.oby`

JSON, backed by the vendored public-domain `json.h` (`vendor/README.md`): one
native allocation per parsed document, freed with `Json::free`.

| Definition | Meaning |
|------------|---------|
| `Json::parse(String) -> Json` | parse a document; `valid()` is 0 when the text is not JSON |
| `Json::last_error() -> String` | why the last parse failed, with the byte, line and column; `""` after a success |
| `Json::valid(Json) -> int` | 1 for a real document or value |
| `Json::kind(Json) -> int` | the `Json::Kind` (`OBJECT`, `ARRAY`, `STRING`, `NUMBER`, `BOOL`, `NULL`, or `NONE` for handle 0) |
| `Json::len(Json) -> int` | an object's member count or an array's element count |
| `Json::key_at(Json, int) -> String` | a member's name by position |
| `Json::at(Json, int) -> Json` | an array element (or an object's value) by position |
| `Json::get(Json, String) -> Json` | a member by name; handle 0 when there is none |
| `Json::as_string(Json) -> String` | a string's bytes, a number's text, `"true"`/`"false"`; else `""` |
| `Json::as_int(Json) -> int64` | a number truncated toward zero; 0 out of range or for anything else |
| `Json::as_float(Json) -> float` | a number, else 0.0 |
| `Json::as_bool(Json) -> int` | 1 for `true`, else 0 |
| `Json::to_minified(Json) -> String` | the document (or the value) as compact JSON |
| `Json::to_pretty(Json) -> String` | the same, one field per line, indented with tabs |
| `Json::free(Json)` | give the allocation back |

A JSON `null` is a real value — its kind is `Json::Kind::NULL` — distinct from
a missing member, which is handle 0 (`Json::Kind::NONE`).

## The natives behind the prelude

| C symbol | Signature | Defined in |
|----------|-----------|------------|
| `write` | `(int fd, byte[] buf, int count) -> int` | `src/native/io.c` |
| `read` | `(int fd, byte[] buf, int count) -> int` | `src/native/io.c` |
| `io_read_full` | `(int fd, byte[] buf, int count) -> int` | `src/native/io.c` |
| `io_read_string` | `(int fd, int count) -> String` | `src/native/io.c` |
| `io_read_bytes` | `(int fd, int count) -> byte[]` | `src/native/io.c` |
| `io_read_line` | `(int fd) -> String` | `src/native/io.c` |
| `io_read_char` | `(int fd) -> int` | `src/native/io.c` |
| `io_read_all` | `(int fd) -> String` | `src/native/io.c` |
| `io_open` | `(String path, int flags) -> int` | `src/native/io.c` |
| `io_close` | `(int fd) -> int` | `src/native/io.c` |
| `io_seek` | `(int fd, int64 offset, int whence) -> int64` | `src/native/io.c` |
| `io_tell` | `(int fd) -> int64` | `src/native/io.c` |
| `io_is_tty` | `(int fd) -> int` | `src/native/io.c` |
| `io_size` | `(String path) -> int64` | `src/native/io.c` |
| `io_fd_size` | `(int fd) -> int64` | `src/native/io.c` |
| `io_exists` | `(String path) -> int` | `src/native/io.c` |
| `io_is_dir` | `(String path) -> int` | `src/native/io.c` |
| `io_remove` | `(String path) -> int` | `src/native/io.c` |
| `io_rename` | `(String from, String to) -> int` | `src/native/io.c` |
| `io_mkdir` | `(String path) -> int` | `src/native/io.c` |
| `io_rmdir` | `(String path) -> int` | `src/native/io.c` |
| `io_read_dir` | `(String path) -> String[]` | `src/native/io.c` |
| `io_cwd` | `() -> String` | `src/native/io.c` |
| `io_args` | `() -> String[]` | `src/native/io.c` |
| `io_flush` | `() -> void` | `src/native/io.c` |
| `io_getenv` | `(String name) -> String` | `src/native/io.c` |
| `io_last_error` | `() -> int` | `src/native/io.c` |
| `int_to_str` | `(int) -> String` | `src/core/numbers.c` |
| `int64_to_str` | `(int64) -> String` | `src/core/numbers.c` |
| `uint_to_str` | `(uint) -> String` | `src/core/numbers.c` |
| `uint64_to_str` | `(uint64) -> String` | `src/core/numbers.c` |
| `byte_from_int` | `(int) -> byte` | `src/core/numbers.c` |
| `uint_from_int`, `uint_from_int64`, `uint_from_uint64` | `(int/int64/uint64) -> uint` | `src/core/numbers.c` |
| `int_from_uint` | `(uint) -> int` | `src/core/numbers.c` |
| `int64_from_uint64` | `(uint64) -> int64` | `src/core/numbers.c` |
| `uint64_from_int64` | `(int64) -> uint64` | `src/core/numbers.c` |
| `to_str` | `(any) -> String` | `src/native/strings.c` |
| `str_concat` | `(String, String) -> String` | `src/native/strings.c` |
| `ob_free` | `(object) -> void` | `src/native/memory.c` |
| `term_move_to` | `(int, int) -> void` | `src/native/terminal.c` |
| `term_set_color` | `(int, int) -> void` | `src/native/terminal.c` |
| `time_sleep_ms` | `(int) -> void` | `src/native/time.c` |
| `time_now_ms` | `() -> int64` | `src/native/time.c` |
| `time_now_us` | `() -> int64` | `src/native/time.c` |
| `term_raw_mode` | `(int) -> void` | `src/native/terminal.c` |
| `term_mouse_mode` | `(int) -> void` | `src/native/terminal.c` |
| `term_restore` | `() -> void` | `src/native/terminal.c` |
| `io_print` | `(String s) -> void` | `src/native/io.c` |
| `term_width`, `term_height` | `() -> int` | `src/native/terminal.c` |
| `input_poll` | `() -> int` | `src/native/input.c` |
| `input_wait` | `(int timeout_ms) -> int` | `src/native/input.c` |
| `input_is_key_pressed` | `(int) -> int` | `src/native/input.c` |
| `input_mods` | `(int) -> int` | `src/native/input.c` |
| `input_text` | `() -> String` | `src/native/input.c` |
| `input_mouse_x`, `input_mouse_y` | `() -> int` | `src/native/input.c` |
| `input_mouse_clicked`, `input_mouse_released`, `input_mouse_down` | `(int button) -> int` | `src/native/input.c` |
| `input_mouse_dragged`, `input_mouse_wheel` | `() -> int` | `src/native/input.c` |
| `json_parse` | `(String text) -> int64` | `src/native/json.c` |
| `json_free` | `(int64 handle) -> void` | `src/native/json.c` |
| `json_kind`, `json_len` | `(int64 handle) -> int` | `src/native/json.c` |
| `json_key_at` | `(int64 handle, int index) -> String` | `src/native/json.c` |
| `json_at` | `(int64 handle, int index) -> int64` | `src/native/json.c` |
| `json_get` | `(int64 handle, String key) -> int64` | `src/native/json.c` |
| `json_as_string` | `(int64 handle) -> String` | `src/native/json.c` |
| `json_as_int` | `(int64 handle) -> int64` | `src/native/json.c` |
| `json_as_float` | `(int64 handle) -> float` | `src/native/json.c` |
| `json_as_bool` | `(int64 handle) -> int` | `src/native/json.c` |
| `json_to_minified` | `(int64 handle) -> String` | `src/native/json.c` |
| `json_to_pretty` | `(int64 handle, String indent, String newline) -> String` | `src/native/json.c` |
| `json_error` | `() -> String` | `src/native/json.c` |
| `ob_rand` | `(int) -> int` | `src/native/random.c` |

`to_str` and `str_concat` are not declared in the prelude: the compiler emits
them directly for `${...}` interpolation. `int_to_str` is what `int::to_str`
binds to.

## Extending the prelude

**A pure-OBY helper** goes in a prelude file and is `use`d from `core.oby`. For
example, `src/prelude/math.oby`:

```
int::twice = (int n) -> int {
	return n + n
}
```

and in `src/prelude/core.oby`:

```
use "math.oby"
```

**A new native** needs three things (see `docs/extern.md` for the C side):

1. Implement it in a domain file under `src/native/` (or `src/core/` for a core
   primitive) and register it in `native_register_std()` in
   `src/native/native.c`.
2. Add the source file to `.sources`.
3. Declare it in the prelude so scripts can see it:

```
int::to_str = extern int_to_str(int i) -> String
```

The OBY name and the C symbol are independent, so one native can back several
names.
