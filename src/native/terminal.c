#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "native/terminal.h"
#include "native/native.h"
#include "native/io.h"
#include "core/string.h"

/*
 * Terminal natives: modes, cursor, colours, size, and buffered output.
 *
 *   term_raw_mode(int on) -> void       keys arrive immediately, no echo
 *   term_mouse_mode(int on) -> void     SGR mouse reporting on/off
 *   term_restore() -> void              undo all of it; also runs at exit
 *   io_print is in io.c: the buffered stdout belongs there
 *   (the flush is `io_flush`, in io.c: the buffer belongs to io)
 *   term_move_to(int x, int y) -> void
 *   term_set_color(int fg, int bg) -> void
 *   term_width() -> int                 columns, 0 when not a terminal
 *   term_height() -> int                rows, 0 when not a terminal
 *
 * Cursor positioning and colours are natives so a redraw loop does not build an
 * interpolated string per cell. Everything here appends to the stdout buffer in
 * `src/native/io.c` (so a cell costs a memcpy, not a syscall) — except
 * `term_restore`, which flushes before the program exits.
 *
 * Input decoding lives in `src/native/input.c`; the clock lives in
 * `src/native/time.c`.
 */

static struct termios g_saved;
static int g_raw = 0;
static int g_mouse = 0;
static int g_atexit = 0;

static void emit(const char *buf, int n) {
    if (n > 0) io_out_write(buf, (size_t)n);
}

static void emit_str(const char *s) {
    emit(s, (int)strlen(s));
}

/* Undo every mode this program may have set. Safe to call twice, and safe
   when nothing was set. */
static void terminal_restore(void) {
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = 0;
    }
    if (g_mouse) {
        emit_str("\x1b[?1006l\x1b[?1002l");
        g_mouse = 0;
    }
    /* Leave the terminal usable even if the program died mid-frame. */
    emit_str("\x1b[0m\x1b[?25h");
    io_out_flush();
}

static void ensure_restore_at_exit(void) {
    if (!g_atexit) {
        atexit(terminal_restore);
        g_atexit = 1;
    }
}

static Value nat_raw_mode(Value *args, int argc) {
    (void)argc;
    int on = (int)args[0].as.i;
    if (on && !g_raw) {
        if (tcgetattr(STDIN_FILENO, &g_saved) == 0) {
            struct termios raw = g_saved;
            raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
                g_raw = 1;
                ensure_restore_at_exit();
            }
        }
    } else if (!on && g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = 0;
    }
    return val_null();
}

static Value nat_mouse_mode(Value *args, int argc) {
    (void)argc;
    int on = (int)args[0].as.i;
    if (on && !g_mouse) {
        /* 1002: report presses, releases, and motion while a button is held.
           1006: SGR coordinates (no 223-column limit). */
        emit_str("\x1b[?1002h\x1b[?1006h");
        g_mouse = 1;
        ensure_restore_at_exit();
    } else if (!on && g_mouse) {
        emit_str("\x1b[?1006l\x1b[?1002l");
        g_mouse = 0;
    }
    return val_null();
}

static Value nat_restore(Value *args, int argc) {
    (void)args;
    (void)argc;
    terminal_restore();
    return val_null();
}

static Value nat_width(Value *args, int argc) {
    (void)args;
    (void)argc;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return val_int(0);
    return val_int(ws.ws_col);
}

static Value nat_height(Value *args, int argc) {
    (void)args;
    (void)argc;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return val_int(0);
    return val_int(ws.ws_row);
}

/* 0-based: move_to(0, 0) is the top-left cell. */
static Value nat_move_to(Value *args, int argc) {
    (void)argc;
    int x = (int)args[0].as.i + 1;
    int y = (int)args[1].as.i + 1;
    char buf[32];
    emit(buf, snprintf(buf, sizeof buf, "\x1b[%d;%dH", y, x));
    return val_null();
}

static Value nat_set_color(Value *args, int argc) {
    (void)argc;
    int fg = (int)args[0].as.i;
    int bg = (int)args[1].as.i;
    char buf[32];
    emit(buf, snprintf(buf, sizeof buf, "\x1b[%d;%dm", fg, bg));
    return val_null();
}

void terminal_register_natives(void) {
    native_add("term_raw_mode", nat_raw_mode);
    native_add("term_mouse_mode", nat_mouse_mode);
    native_add("term_restore", nat_restore);
    native_add("term_width", nat_width);
    native_add("term_height", nat_height);
    native_add("term_move_to", nat_move_to);
    native_add("term_set_color", nat_set_color);
}
