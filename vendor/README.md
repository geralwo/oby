# vendor

Third-party code, vendored verbatim — do not edit. Re-fetch and re-pin when
upgrading.

## json.h

* Source: <https://github.com/sheredom/json.h> (branch `master`)
* Fetched: 2026-10-13
* sha256: `299b71946e8b1e8201565d4fb7eb9fd8b1f770cdb45854a814ca9ce56f04e073`
* License: The Unlicense (public domain), stated in the file's header.

A JSON parser that allocates once for the whole document, with precise
failure positions (`json_parse_ex` reports the error kind, byte offset, line
and column) and a serializer back to a fresh allocation
(`json_write_minified` / `json_write_pretty`). Builds warning-free under the
project's `-std=c11 -Wall -Wextra -Werror -pedantic`.

Used by `src/native/json.c` for the scripts' `Json` module (see
`src/prelude/Json.oby`).
