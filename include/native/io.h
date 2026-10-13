#ifndef OBY_IO_H
#define OBY_IO_H

#include "core/value.h"

/* Register the I/O natives (read, write, ...) with the native registry. */
void io_register_natives(void);

/* Record `errno` for `io::last_error` and return the failure value: -1 for a
   count, null for a value. Shared with the other native modules (net). */
Value io_fail(void);
Value io_fail_null(void);

/* Buffered stdout, shared by `print` and the terminal escape natives: output
   is appended here and flushed when the buffer fills, on a newline when stdout
   is a terminal, before reading input, before a raw `write` to stdout, and at
   exit. `write` itself stays a raw write(2). */
void io_out_write(const char *buf, size_t n);
void io_out_flush(void);

#endif // OBY_IO_H
