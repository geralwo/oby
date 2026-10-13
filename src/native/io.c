#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "native/io.h"
#include "native/native.h"
#include "core/array.h"
#include "core/os.h"
#include "core/string.h"

/*
 * I/O natives: file descriptors, filesystem queries, and buffered reading.
 *
 *   read(int fd, byte[] buf, int count) -> int       read(2); may be short
 *   io_read_full(int fd, byte[] buf, int count) -> int
 *                                                    fill buf or stop at EOF
 *   io_read_string(int fd, int count) -> String      the same, into a String
 *   io_read_bytes(int fd, int count) -> byte[]       the same, as raw bytes
 *   io_read_line(int fd) -> String                   null at end of file
 *   io_read_char(int fd) -> int                      -1 at end of file
 *   io_read_all(int fd) -> String
 *   write(int fd, byte[] buf, int count) -> int
 *   io_open(String path, io::Open flags) -> int
 *   io_close(int fd) -> int
 *   io_seek(int fd, int64 offset, io::Seek whence) -> int64
 *   io_tell(int fd) -> int64
 *   io_size(String path) -> int64                    -1 on failure
 *   io_fd_size(int fd) -> int64                      -1 on failure
 *   io_exists(String path) -> int
 *   io_is_dir(String path) -> int
 *   io_remove(String path) -> int                    unlink
 *   io_rename(String from, String to) -> int
 *   io_mkdir(String path) -> int
 *   io_rmdir(String path) -> int
 *   io_read_dir(String path) -> String[]
 *   io_is_tty(int fd) -> int
 *   io_cwd() -> String                               the working directory
 *   io_getenv(String name) -> String                 null when unset
 *   io_last_error() -> int                           errno of the last failure
 *
 * `read_line` and `read_char` read through a per-fd buffer so they can be
 * mixed with `read` on one handle; `seek`/`tell` account for the buffered
 * bytes instead of exposing the read-ahead position.
 */

/* Must match io::Open in src/prelude/io.oby. */
#define OBY_OPEN_ACCESS_MASK 3
#define OBY_OPEN_WRITE  1
#define OBY_OPEN_RDWR   2
#define OBY_OPEN_CREATE 4
#define OBY_OPEN_TRUNC  8
#define OBY_OPEN_APPEND 16
#define OBY_OPEN_EXCL   32

/* ------------------------------------------------------------------ */
/* Read buffers                                                        */
/* ------------------------------------------------------------------ */

#define IO_READ_BUF   4096
#define IO_READ_SLOTS 16

typedef struct {
    int fd;            /* -1 when the slot is free */
    size_t len, pos;   /* buffered bytes are [pos, len) */
    uint8_t *buf;
} ReadBuf;

static ReadBuf g_reads[IO_READ_SLOTS];
static uint8_t g_read_buffers[IO_READ_SLOTS][IO_READ_BUF];
static int g_reads_ready = 0;

static void reads_init(void) {
    if (g_reads_ready) return;
    for (int i = 0; i < IO_READ_SLOTS; i++) {
        g_reads[i].fd = -1;
        g_reads[i].len = g_reads[i].pos = 0;
        g_reads[i].buf = g_read_buffers[i];
    }
    g_reads_ready = 1;
}

static ReadBuf *readbuf_find(int fd) {
    if (fd < 0) return NULL;
    reads_init();
    for (int i = 0; i < IO_READ_SLOTS; i++)
        if (g_reads[i].fd == fd) return &g_reads[i];
    return NULL;
}

static ReadBuf *readbuf_get(int fd) {
    ReadBuf *rb = readbuf_find(fd);
    if (rb) return rb;
    if (fd < 0) return NULL;
    reads_init();
    for (int i = 0; i < IO_READ_SLOTS; i++) {
        if (g_reads[i].fd == -1) {
            g_reads[i].fd = fd;
            g_reads[i].len = g_reads[i].pos = 0;
            return &g_reads[i];
        }
    }
    return NULL;
}

static size_t readbuf_avail(const ReadBuf *rb) {
    return rb->len - rb->pos;
}

/* Refill an empty slot. Returns bytes read, 0 at end of file, -1 on error. */
static ssize_t readbuf_fill(ReadBuf *rb) {
    rb->len = rb->pos = 0;
    ssize_t n = read(rb->fd, rb->buf, IO_READ_BUF);
    if (n > 0) rb->len = (size_t)n;
    return n;
}

static void readbuf_drop(int fd) {
    ReadBuf *rb = readbuf_find(fd);
    if (rb) {
        rb->fd = -1;
        rb->len = rb->pos = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Buffered stdout                                                     */
/* ------------------------------------------------------------------ */

#define OUT_BUF 8192

static char g_out[OUT_BUF];
static size_t g_out_len = 0;
static int g_out_line_buffered = -1;   /* set on first use: isatty(1) */

void io_out_flush(void) {
    size_t off = 0;
    while (off < g_out_len) {
        ssize_t n = write(STDOUT_FILENO, g_out + off, g_out_len - off);
        if (n <= 0) break;   /* a broken pipe: drop the rest */
        off += (size_t)n;
    }
    g_out_len = 0;
}

static void out_write_direct(const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, buf + off, n - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

void io_out_write(const char *buf, size_t n) {
    if (g_out_line_buffered < 0) {
        g_out_line_buffered = isatty(STDOUT_FILENO) == 1;
        atexit(io_out_flush);   /* whatever was buffered must go out */
    }
    if (n >= OUT_BUF) {                  /* too big to buffer */
        io_out_flush();
        out_write_direct(buf, n);
        return;
    }
    if (g_out_len + n > OUT_BUF) io_out_flush();
    memcpy(g_out + g_out_len, buf, n);
    g_out_len += n;
    if (g_out_line_buffered && memchr(buf, '\n', n)) io_out_flush();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* A `byte[]` value arrives as a `VAL_ARRAY`; the raw bytes live in its buffer. */
static void *array_bytes(const Value *v) {
    Array *arr = v->as.arr;
    return arr ? arr->buffer : NULL;
}

/* A NUL-terminated copy of a String argument, or NULL. */
static char *path_copy(const Value *v) {
    if (v->tag != VAL_STRING) return NULL;
    const String *s = v->as.str;
    char *p = (char *)malloc(s->length + 1);
    if (!p) return NULL;
    memcpy(p, s->data, s->length);
    p[s->length] = '\0';
    return p;
}

/* Append to a growable buffer; returns 0 on success. */
static int buf_append(char **buf, size_t *len, size_t *cap, const void *src, size_t n) {
    if (*len + n > *cap) {
        size_t cap2 = *cap;
        while (cap2 < *len + n) cap2 *= 2;
        char *p = (char *)realloc(*buf, cap2);
        if (!p) return -1;
        *buf = p;
        *cap = cap2;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return 0;
}

/* `io_last_error`: the errno of the most recent io call that reported failure.
   Not cleared by later successes, like C's errno. */
static int g_last_error;

/* Report a failure: record errno for `io_last_error` and return -1 / null. */
static Value io_fail(void) {
    g_last_error = errno;
    return val_int(-1);
}

static Value io_fail_null(void) {
    g_last_error = errno;
    return val_null();
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Serve the buffered bytes first, then read(2) the rest. */
static size_t read_serve_buffered(int fd, void *buf, size_t count) {
    ReadBuf *rb = readbuf_find(fd);
    if (!rb || readbuf_avail(rb) == 0) return 0;
    size_t take = readbuf_avail(rb);
    if (take > count) take = count;
    memcpy(buf, rb->buf + rb->pos, take);
    rb->pos += take;
    return take;
}

/* read(int fd, byte[] buf, int count) -> int : read(2); may be a short read.
   Reading from stdin flushes buffered output first, so a prompt is on screen
   before the read blocks. */
static Value nat_read(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    void *buf = array_bytes(&args[1]);
    int64_t want = args[2].as.i;
    if (want < 0 || !buf) return val_int(-1);
    size_t count = (size_t)want;
    if (fd == STDIN_FILENO) io_out_flush();

    size_t got = read_serve_buffered(fd, buf, count);
    if (got < count) {
        ssize_t n = read(fd, (char *)buf + got, count - got);
        if (n < 0) return got > 0 ? val_int((int64_t)got) : io_fail();
        got += (size_t)n;
    }
    return val_int((int64_t)got);
}

/* io_read_full(int fd, byte[] buf, int count) -> int : fill `count` bytes,
   stopping early only at end of file or error. What `File::read` uses. */
static Value nat_read_full(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    void *buf = array_bytes(&args[1]);
    int64_t want = args[2].as.i;
    if (want < 0 || !buf) return val_int(-1);
    size_t count = (size_t)want;

    size_t got = read_serve_buffered(fd, buf, count);
    while (got < count) {
        ssize_t n = read(fd, (char *)buf + got, count - got);
        if (n < 0) {
            if (got > 0) break;
            return io_fail();
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    return val_int((int64_t)got);
}

/* A fresh zeroed byte[] of `count` bytes; null on failure. */
static Value new_bytes_value(size_t count) {
    Array *a = array_new(1);
    if (!a) return val_null();
    if (count > 0) {
        if (array_reserve(a, count) != 0) {
            array_free(a);
            return val_null();
        }
        memset(a->buffer, 0, count);
    }
    a->length = count;
    return val_array(a);
}

/* Read up to `count` bytes into a fresh buffer, filling it unless the file
   ends. Returns the byte count, or -1 when nothing could be read. */
static ssize_t read_new(int fd, void *buf, size_t count) {
    size_t got = read_serve_buffered(fd, buf, count);
    while (got < count) {
        ssize_t n = read(fd, (char *)buf + got, count - got);
        if (n < 0) {
            if (got == 0) return -1;
            break;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* io_read_string(int fd, int count) -> String : fill a fresh String; "" at end
   of file, null on error. */
static Value nat_read_string(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    int64_t want = args[1].as.i;
    if (want < 0) return val_null();
    size_t count = (size_t)want;
    char *buf = (char *)malloc(count ? count : 1);
    if (!buf) return val_null();
    ssize_t got = read_new(fd, buf, count);
    if (got < 0) {
        free(buf);
        return io_fail_null();
    }
    String *s = string_new_len(buf, (size_t)got);
    free(buf);
    return s ? val_string(s) : val_null();
}

/* io_read_bytes(int fd, int count) -> byte[] : fill a fresh byte[]; null at
   end of file or on error. */
static Value nat_read_bytes(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    int64_t want = args[1].as.i;
    if (want < 0) return val_null();
    size_t count = (size_t)want;
    Value v = new_bytes_value(count);
    if (v.tag != VAL_ARRAY) return val_null();
    Array *arr = v.as.arr;
    ssize_t got = read_new(fd, arr->buffer, count);
    if (got < 0) {
        array_free(arr);
        return io_fail_null();
    }
    if (got == 0) {
        array_free(arr);
        return val_null();
    }
    arr->length = (size_t)got;
    return v;
}

/* io_read_line(int fd) -> String : one line without its newline; null at end
   of file (an empty line is ""). */
static Value nat_read_line(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    ReadBuf *rb = readbuf_get(fd);
    size_t cap = 128, len = 0;
    char *line = (char *)malloc(cap);
    if (!line) return val_null();

    int eof = 0;
    for (;;) {
        if (rb) {
            if (readbuf_avail(rb) == 0) {
                ssize_t n = readbuf_fill(rb);
                if (n < 0) {
                    free(line);
                    return io_fail_null();
                }
                if (n == 0) {
                    eof = 1;
                    break;
                }
            }
            size_t i = rb->pos;
            while (i < rb->len && rb->buf[i] != '\n') i++;
            if (buf_append(&line, &len, &cap, rb->buf + rb->pos, i - rb->pos) != 0) {
                free(line);
                return val_null();
            }
            rb->pos = (i < rb->len) ? i + 1 : i;
            if (i < rb->len) break;   /* found the newline */
        } else {
            /* No buffer slot: fall back to one byte at a time. */
            char c;
            ssize_t n = read(fd, &c, 1);
            if (n < 0) {
                free(line);
                return io_fail_null();
            }
            if (n == 0) {
                eof = 1;
                break;
            }
            if (c == '\n') break;
            if (buf_append(&line, &len, &cap, &c, 1) != 0) {
                free(line);
                return val_null();
            }
        }
    }
    if (len == 0 && eof) {
        free(line);
        return val_null();
    }
    if (len > 0 && line[len - 1] == '\r') len--;
    String *s = string_new_len(line, len);
    free(line);
    return s ? val_string(s) : val_null();
}

/* io_read_char(int fd) -> int : one byte, or -1 at end of file/error. */
static Value nat_read_char(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    ReadBuf *rb = readbuf_get(fd);
    if (rb) {
        if (readbuf_avail(rb) == 0 && readbuf_fill(rb) <= 0) return val_int(-1);
        return val_int((int64_t)rb->buf[rb->pos++]);
    }
    unsigned char c;
    ssize_t n = read(fd, &c, 1);
    if (n <= 0) return val_int(-1);
    return val_int((int64_t)c);
}

/* io_read_all(int fd) -> String : the buffered bytes plus everything to the
   end of the file (null on failure). */
static Value nat_read_all(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return val_null();

    ReadBuf *rb = readbuf_find(fd);
    if (rb && readbuf_avail(rb) > 0) {
        if (buf_append(&buf, &len, &cap, rb->buf + rb->pos, readbuf_avail(rb)) != 0) {
            free(buf);
            return val_null();
        }
        rb->pos = rb->len;
    }
    for (;;) {
        if (len == cap) {
            char *p = (char *)realloc(buf, cap * 2);
            if (!p) {
                free(buf);
                return val_null();
            }
            buf = p;
            cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    String *s = string_new_len(buf, len);
    free(buf);
    return s ? val_string(s) : val_null();
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* write(int fd, byte[] buf, int count) -> int : write(2). A write to stdout
   goes out after whatever `print` has buffered, so order is kept. */
static Value nat_write(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    const void *buf = array_bytes(&args[1]);
    size_t count = (size_t)args[2].as.i;
    if (fd == STDOUT_FILENO) io_out_flush();
    ssize_t n = write(fd, buf, count);
    if (n < 0) return io_fail();
    return val_int((int64_t)n);
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

/* io_open(String path, io::Open flags) -> int : open(2) with the flag bits. */
static Value nat_open(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(-1);
    int64_t flags = args[1].as.i;
    int access = (int)(flags & OBY_OPEN_ACCESS_MASK);
    int oflags = (access == OBY_OPEN_WRITE) ? O_WRONLY
               : (access == OBY_OPEN_RDWR) ? O_RDWR
               : O_RDONLY;
    if (flags & OBY_OPEN_CREATE) oflags |= O_CREAT;
    if (flags & OBY_OPEN_TRUNC) oflags |= O_TRUNC;
    if (flags & OBY_OPEN_APPEND) oflags |= O_APPEND;
    if (flags & OBY_OPEN_EXCL) oflags |= O_EXCL;
    int fd = open(path, oflags, 0644);
    free(path);
    if (fd < 0) return io_fail();
    return val_int((int64_t)fd);
}

/* io_close(int fd) -> int : close(2), dropping any buffered input. */
static Value nat_close(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    readbuf_drop(fd);
    int r = close(fd);
    if (r != 0) return io_fail();
    return val_int(0);
}

/* io_seek(int fd, int64 offset, io::Seek whence) -> int64 */
static Value nat_seek(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    off_t off = (off_t)args[1].as.i;
    int whence = (int)args[2].as.i;
    ReadBuf *rb = readbuf_find(fd);
    if (rb && readbuf_avail(rb) > 0 && whence == SEEK_CUR)
        off -= (off_t)readbuf_avail(rb);
    readbuf_drop(fd);
    off_t r = lseek(fd, off, whence);
    if (r < 0) return io_fail();
    return val_int((int64_t)r);
}

/* io_tell(int fd) -> int64 : the logical position, buffered bytes included. */
static Value nat_tell(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) return io_fail();
    ReadBuf *rb = readbuf_find(fd);
    if (rb) cur -= (off_t)readbuf_avail(rb);
    return val_int((int64_t)cur);
}

/* io_is_tty(int fd) -> int */
static Value nat_is_tty(Value *args, int argc) {
    (void)argc;
    return val_int(isatty((int)args[0].as.i) == 1);
}

/* ------------------------------------------------------------------ */
/* Filesystem                                                          */
/* ------------------------------------------------------------------ */

/* io_size(String path) -> int64 : stat; -1 on failure. */
static Value nat_size(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(-1);
    struct stat st;
    int r = stat(path, &st);
    free(path);
    if (r != 0) return io_fail();
    return val_int((int64_t)st.st_size);
}

/* io_fd_size(int fd) -> int64 : fstat; -1 on failure. */
static Value nat_fd_size(Value *args, int argc) {
    (void)argc;
    struct stat st;
    if (fstat((int)args[0].as.i, &st) != 0) return io_fail();
    return val_int((int64_t)st.st_size);
}

/* io_exists(String path) -> int */
static Value nat_exists(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(0);
    int r = access(path, F_OK) == 0;
    free(path);
    return val_int(r);
}

/* io_is_dir(String path) -> int */
static Value nat_is_dir(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(0);
    struct stat st;
    int r = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    return val_int(r);
}

/* io_remove(String path) -> int : unlink(2). */
static Value nat_remove(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(-1);
    int r = unlink(path);
    free(path);
    if (r != 0) return io_fail();
    return val_int(0);
}

/* io_rename(String from, String to) -> int */
static Value nat_rename(Value *args, int argc) {
    (void)argc;
    char *from = path_copy(&args[0]);
    char *to = path_copy(&args[1]);
    if (!from || !to) {
        free(from);
        free(to);
        return val_int(-1);
    }
    int r = rename(from, to);
    free(from);
    free(to);
    if (r != 0) return io_fail();
    return val_int(0);
}

/* io_mkdir(String path) -> int : one directory, mode 0755. */
static Value nat_mkdir(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(-1);
    int r = mkdir(path, 0755);
    free(path);
    if (r != 0) return io_fail();
    return val_int(0);
}

/* io_rmdir(String path) -> int : removes an empty directory. */
static Value nat_rmdir(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_int(-1);
    int r = rmdir(path);
    free(path);
    if (r != 0) return io_fail();
    return val_int(0);
}

/* io_read_dir(String path) -> String[] : entry names, without "." and "..",
   in readdir order; null when the directory cannot be opened. */
static Value nat_read_dir(Value *args, int argc) {
    (void)argc;
    char *path = path_copy(&args[0]);
    if (!path) return val_null();
    DIR *d = opendir(path);
    free(path);
    if (!d) return io_fail_null();

    Array *out = array_new(sizeof(Value));
    if (!out) {
        closedir(d);
        return val_null();
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        String *s = string_new_len(e->d_name, strlen(e->d_name));
        if (!s) continue;
        Value v = val_string(s);
        array_push(out, &v);
    }
    closedir(d);
    return val_array(out);
}

/* ------------------------------------------------------------------ */
/* Process context                                                     */
/* ------------------------------------------------------------------ */

/* io_args() -> String[] : the arguments the program was given — everything
   after the script path, or after the executable itself when bundled. */
static Value nat_args(Value *args, int argc) {
    (void)args;
    (void)argc;
    Array *out = array_new(sizeof(Value));
    if (!out) return val_null();
    for (int i = 0; i < os_args_count(); i++) {
        const char *value = os_arg(i);
        if (!value) continue;
        String *s = string_new(value);
        if (!s) continue;
        Value v = val_string(s);
        array_push(out, &v);
    }
    return val_array(out);
}

/* io_cwd() -> String : the working directory, or null. */
static Value nat_cwd(Value *args, int argc) {
    (void)args;
    (void)argc;
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)malloc(cap);
        if (!buf) return val_null();
        if (getcwd(buf, cap)) {
            String *s = string_new(buf);
            free(buf);
            return s ? val_string(s) : val_null();
        }
        free(buf);
        if (errno != ERANGE) return io_fail_null();
        cap *= 2;
    }
}

/* io_getenv(String name) -> String : a variable's value, or null when it is
   not set (an empty value is ""). */
static Value nat_getenv(Value *args, int argc) {
    (void)argc;
    char *name = path_copy(&args[0]);
    if (!name) return val_null();
    char *v = getenv(name);
    free(name);
    if (!v) return val_null();
    String *s = string_new(v);
    return s ? val_string(s) : val_null();
}

/* io_last_error() -> int : the errno of the most recent io call that reported
   failure (0 before any failure; not cleared by later successes). */
static Value nat_last_error(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int((int64_t)g_last_error);
}

void io_register_natives(void) {
    native_add("read", nat_read);
    native_add("io_read_full", nat_read_full);
    native_add("io_read_string", nat_read_string);
    native_add("io_read_bytes", nat_read_bytes);
    native_add("io_read_line", nat_read_line);
    native_add("io_read_char", nat_read_char);
    native_add("write", nat_write);
    native_add("io_read_all", nat_read_all);
    native_add("io_open", nat_open);
    native_add("io_close", nat_close);
    native_add("io_seek", nat_seek);
    native_add("io_tell", nat_tell);
    native_add("io_is_tty", nat_is_tty);
    native_add("io_size", nat_size);
    native_add("io_fd_size", nat_fd_size);
    native_add("io_exists", nat_exists);
    native_add("io_is_dir", nat_is_dir);
    native_add("io_remove", nat_remove);
    native_add("io_rename", nat_rename);
    native_add("io_mkdir", nat_mkdir);
    native_add("io_rmdir", nat_rmdir);
    native_add("io_read_dir", nat_read_dir);
    native_add("io_cwd", nat_cwd);
    native_add("io_getenv", nat_getenv);
    native_add("io_last_error", nat_last_error);
    native_add("io_args", nat_args);
}
