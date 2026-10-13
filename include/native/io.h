#ifndef OBY_IO_H
#define OBY_IO_H

/* Register the I/O natives (read, write, ...) with the native registry. */
void io_register_natives(void);

/* Buffered stdout, shared by `print` and the terminal escape natives: output
   is appended here and flushed when the buffer fills, on a newline when stdout
   is a terminal, before reading input, before a raw `write` to stdout, and at
   exit. `write` itself stays a raw write(2). */
void io_out_write(const char *buf, size_t n);
void io_out_flush(void);

#endif // OBY_IO_H
