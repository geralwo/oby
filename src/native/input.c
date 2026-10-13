#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include "native/input.h"
#include "native/native.h"
#include "native/io.h"
#include "core/string.h"

/*
 * Input natives.
 *
 *   input_poll() -> int
 *   input_wait(int timeout_ms) -> int
 *   input_is_key_pressed(int key) -> int
 *   input_mods(int key) -> int
 *   input_text() -> String
 *
 * Terminal modes (raw mode, mouse reporting) live in `src/native/terminal.c`.
 *
 * `poll` drains everything the terminal has buffered into a key set, a typed
 * text buffer, and mouse state, clearing them first: the queries therefore
 * answer "did this arrive since the last poll". The state lives here, not in
 * the script — the old project bound a script-visible `byte[64]` to the
 * engine's live bitmap, which coupled the language to the engine.
 *
 * Key codes match `Input::Key` in `src/prelude/Input.oby`: ASCII for printable
 * keys, 256+ for the special ones. Escape sequences are decoded incrementally,
 * so a sequence split across reads still parses; modifiers are kept per key
 * (`Input::Mod` bits) instead of being dropped.
 */

#define KEY_MAX   512
#define INPUT_BUF 64

/* Must match Input::Key in src/prelude/Input.oby. */
#define KEY_NONE      0
#define KEY_ENTER     13
#define KEY_ESCAPE    27
#define KEY_BACKSPACE 127
#define KEY_UP        256
#define KEY_DOWN      257
#define KEY_LEFT      258
#define KEY_RIGHT     259
#define KEY_INSERT    260
#define KEY_DELETE    261
#define KEY_HOME      262
#define KEY_END       263
#define KEY_PAGEUP    264
#define KEY_PAGEDOWN  265
#define KEY_F1        266

static uint64_t g_pressed[(KEY_MAX + 63) / 64];
static uint8_t  g_mods[KEY_MAX];

/* A partially-received escape sequence, kept across reads. */
static uint8_t g_buf[INPUT_BUF];
static int g_buf_len = 0;

/* Printable bytes typed since the last poll, in arrival order. The key set
   cannot express that order, which is what text entry needs. */
static char  *g_text = NULL;
static size_t g_text_len = 0;
static size_t g_text_cap = 0;

/* Mouse state (SGR reports). The position and held buttons persist across
   polls; clicks, releases, drags, and wheel ticks are per batch. */
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static int g_mouse_seen = 0;
static int g_mouse_held = 0;
static int g_mouse_clicked = 0;
static int g_mouse_released = 0;
static int g_mouse_dragged = 0;
static int g_mouse_wheel = 0;

static void text_push(unsigned char c) {
    if (g_text_len == g_text_cap) {
        size_t cap = g_text_cap ? g_text_cap * 2 : 256;
        char *p = (char *)realloc(g_text, cap);
        if (!p) return;   /* drop the byte rather than fail the poll */
        g_text = p;
        g_text_cap = cap;
    }
    g_text[g_text_len++] = (char)c;
}

static void pressed_set(int key, int mods) {
    unsigned u = (unsigned)key;
    if (u == 0 || u >= KEY_MAX) return;
    g_pressed[u / 64] |= (uint64_t)1 << (u % 64);
    g_mods[u] = (uint8_t)mods;
}

static int pressed_has(unsigned u) {
    if (u >= KEY_MAX) return 0;
    return (int)((g_pressed[u / 64] >> (u % 64)) & 1u);
}

/* A plain byte: control keys keep their value, letters fold to lowercase.
   Ctrl+letter bytes (1..26) become the letter plus the CTRL bit; an ASCII
   capital becomes the lowercase key plus SHIFT, so case stays queryable. */
static int map_byte(unsigned char ch, int *mods) {
    *mods = 0;
    if (ch == '\r' || ch == '\n') return KEY_ENTER;
    if (ch == '\t') return 9;
    if (ch == ' ') return 32;
    if (ch == 8 || ch == 127) return KEY_BACKSPACE;
    if (ch >= 'A' && ch <= 'Z') {
        *mods = INPUT_MOD_SHIFT;
        return ch - 'A' + 'a';
    }
    if (ch >= 1 && ch <= 26) {   /* Ctrl+A..Ctrl+Z, minus the aliases above */
        *mods = INPUT_MOD_CTRL;
        return 'a' + ch - 1;
    }
    return ch;
}

/* `ESC [ <num> ~` */
static int csi_tilde(int num) {
    switch (num) {
        case 1:  return KEY_HOME;
        case 2:  return KEY_INSERT;
        case 3:  return KEY_DELETE;
        case 4:  return KEY_END;
        case 5:  return KEY_PAGEUP;
        case 6:  return KEY_PAGEDOWN;
        case 11: return KEY_F1;
        case 12: return KEY_F1 + 1;
        case 13: return KEY_F1 + 2;
        case 14: return KEY_F1 + 3;
        case 15: return KEY_F1 + 4;
        case 17: return KEY_F1 + 5;
        case 18: return KEY_F1 + 6;
        case 19: return KEY_F1 + 7;
        case 20: return KEY_F1 + 8;
        case 21: return KEY_F1 + 9;
        case 23: return KEY_F1 + 10;
        case 24: return KEY_F1 + 11;
        default: return KEY_NONE;
    }
}

/* `ESC [ <final>` */
static int csi_final(char c) {
    switch (c) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default:  return KEY_NONE;
    }
}

/* The xterm modifier parameter (`ESC [ 1;5A` = Ctrl+Up) carries a one-based
   number: 1 + shift(1) + alt(2) + ctrl(4). */
static int csi_mods(int param) {
    int m = param - 1;
    int mods = 0;
    if (m & 1) mods |= INPUT_MOD_SHIFT;
    if (m & 2) mods |= INPUT_MOD_ALT;
    if (m & 4) mods |= INPUT_MOD_CTRL;
    return mods;
}

/* `ESC [ < b ; x ; y (M|m)` : an SGR mouse report. `b` packs the button, the
   modifiers, and the kind of event; `M` is a press or motion, `m` a release.
   Returns the bytes consumed (0 = incomplete; a malformed report is consumed
   and dropped). */
static int parse_sgr_mouse(const uint8_t *b, int n, InputMouseEvent *out) {
    int vals[3] = { 0, 0, 0 };
    int idx = 0;
    for (int i = 3; i < n; i++) {   /* b[0]=ESC, b[1]='[', b[2]='<' */
        char c = (char)b[i];
        if (c >= '0' && c <= '9') { vals[idx] = vals[idx] * 10 + (c - '0'); continue; }
        if (c == ';') { if (idx < 2) idx++; continue; }
        if (c == 'M' || c == 'm') {
            out->is_mouse = 1;
            out->code = vals[0];
            out->x = vals[1];
            out->y = vals[2];
            out->release = (c == 'm');
            return i + 1;
        }
        return i + 1;   /* malformed: drop it */
    }
    return 0;
}

/* Bytes consumed from `b`, or 0 if the sequence is incomplete. `*out` is
   KEY_NONE for a complete but unknown sequence (dropped); `*out_mods` gets
   the modifier bits, and `mouse` the decoded mouse report when there is one. */
static int parse_key(const uint8_t *b, int n, int *out, int *out_mods,
                     InputMouseEvent *mouse) {
    *out = KEY_NONE;
    *out_mods = 0;
    mouse->is_mouse = 0;
    if (n <= 0) return 0;

    if (b[0] != 0x1b) {
        *out = map_byte(b[0], out_mods);
        return 1;
    }
    if (n == 1) return 0;   /* a lone ESC: wait for more */

    if (b[1] == '[' && n >= 3 && b[2] == '<')
        return parse_sgr_mouse(b, n, mouse);

    if (b[1] == '[') {
        int num = 0, first = -1;
        for (int i = 2; i < n; i++) {
            char c = (char)b[i];
            if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); continue; }
            if (c == ';') { first = num; num = 0; continue; }   /* key; modifier */
            *out = (c == '~') ? csi_tilde(first >= 0 ? first : num) : csi_final(c);
            *out_mods = (first >= 0) ? csi_mods(num) : 0;
            return i + 1;
        }
        return 0;
    }

    if (b[1] == 'O') {   /* application-mode F1-F4 */
        if (n < 3) return 0;
        switch ((char)b[2]) {
            case 'P': *out = KEY_F1; break;
            case 'Q': *out = KEY_F1 + 1; break;
            case 'R': *out = KEY_F1 + 2; break;
            case 'S': *out = KEY_F1 + 3; break;
            default:  *out = KEY_NONE; break;
        }
        return 3;
    }

    *out = KEY_ESCAPE;
    return 1;
}

static void mouse_apply(const InputMouseEvent *e) {
    g_mouse_seen = 1;
    g_mouse_x = e->x - 1;   /* SGR coordinates are 1-based */
    g_mouse_y = e->y - 1;
    int b = e->code;
    if (b & 64) {           /* wheel: 64 is up, 65 down */
        g_mouse_wheel += (b & 1) ? -1 : 1;
        return;
    }
    if (b & 32) {           /* motion (reported while a button is held) */
        g_mouse_dragged = 1;
        return;
    }
    int button = b & 3;
    if (button > 2) return;   /* extra buttons: no state for them */
    if (e->release) {
        g_mouse_held &= ~(1 << button);
        g_mouse_released |= 1 << button;
    } else {
        g_mouse_held |= 1 << button;
        g_mouse_clicked |= 1 << button;
    }
}

/* Decode as much of the buffer as possible. With `flush`, an incomplete
   sequence is resolved (a lone ESC becomes KEY_ESCAPE) and dropped. */
static void decode_buf(int flush) {
    int used = 0;
    while (used < g_buf_len) {
        int key, mods;
        InputMouseEvent mouse;
        int n = parse_key(g_buf + used, g_buf_len - used, &key, &mods, &mouse);
        if (n == 0) {
            if (!flush) break;
            if (g_buf[used] == 0x1b) pressed_set(KEY_ESCAPE, 0);
            used = g_buf_len;
            break;
        }
        if (mouse.is_mouse) {
            mouse_apply(&mouse);
        } else if (key != KEY_NONE) {
            pressed_set(key, mods);
            unsigned char c = g_buf[used];
            if (c != 0x1b && c >= 32 && c != 127) text_push(c);
        }
        used += n;
    }
    if (used > 0 && used < g_buf_len)
        memmove(g_buf, g_buf + used, (size_t)(g_buf_len - used));
    g_buf_len -= used;
    if (g_buf_len < 0) g_buf_len = 0;
}

static int anything_arrived(void) {
    if (g_text_len > 0 || g_mouse_seen) return 1;
    for (size_t i = 0; i < sizeof g_pressed / sizeof g_pressed[0]; i++) {
        if (g_pressed[i] != 0) return 1;
    }
    return 0;
}

/* Start a new batch: drain stdin (waiting up to `timeout_ms`); -1 waits
   forever. Returns 1 when anything arrived. */
static int drain(int timeout_ms) {
    /* Show anything `print` has buffered before we block. */
    io_out_flush();

    memset(g_pressed, 0, sizeof g_pressed);
    memset(g_mods, 0, sizeof g_mods);
    g_text_len = 0;
    g_mouse_seen = g_mouse_clicked = g_mouse_released = 0;
    g_mouse_dragged = g_mouse_wheel = 0;

    int t = timeout_ms;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, t) <= 0) break;

        uint8_t chunk[32];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) break;

        if (g_buf_len + (int)n > INPUT_BUF) n = INPUT_BUF - g_buf_len;
        if (n <= 0) { decode_buf(1); g_buf_len = 0; continue; }
        memcpy(g_buf + g_buf_len, chunk, (size_t)n);
        g_buf_len += (int)n;
        decode_buf(0);
        t = 0;   /* drain what else is ready, then stop */
    }

    decode_buf(1);
    return anything_arrived();
}

/* Letters match either case; ENTER covers \r and \n (decoded to KEY_ENTER). */
static unsigned normalize_key(int64_t key) {
    if (key == KEY_ENTER) return KEY_ENTER;
    unsigned u = (unsigned)key;
    if (u < 256 && isalpha((unsigned char)u)) u = (unsigned)tolower((unsigned char)u);
    return u;
}

/* input_poll() -> int : start a new input batch (the frame tick). */
static Value nat_poll(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int(drain(0));
}

/* input_wait(int timeout_ms) -> int : the same, but block first. */
static Value nat_wait(Value *args, int argc) {
    (void)argc;
    return val_int(drain((int)args[0].as.i));
}

/* input_is_key_pressed(int key) -> int */
static Value nat_is_key_pressed(Value *args, int argc) {
    (void)argc;
    unsigned u = normalize_key(args[0].as.i);
    if (u == 0 || u >= KEY_MAX) return val_int(0);
    return val_int(pressed_has(u));
}

/* input_mods(int key) -> int : the modifier bits `key` arrived with, or 0. */
static Value nat_mods(Value *args, int argc) {
    (void)argc;
    unsigned u = normalize_key(args[0].as.i);
    if (u == 0 || u >= KEY_MAX || !pressed_has(u)) return val_int(0);
    return val_int((int64_t)g_mods[u]);
}

/* input_text() -> String : printable bytes since the last poll, in arrival
   order; null when none. */
static Value nat_text(Value *args, int argc) {
    (void)args;
    (void)argc;
    if (g_text_len == 0) return val_null();
    String *s = string_new_len(g_text, g_text_len);
    return s ? val_string(s) : val_null();
}

/* input_mouse_x() -> int : the last event's column (0-based). */
static Value nat_mouse_x(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int(g_mouse_x);
}

/* input_mouse_y() -> int : the last event's row (0-based). */
static Value nat_mouse_y(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int(g_mouse_y);
}

static int mouse_button(const Value *v, int *bit) {
    int64_t b = v->as.i;
    if (b < 0 || b > 2) return 0;
    *bit = 1 << (int)b;
    return 1;
}

/* input_mouse_clicked(button) -> int : pressed since the last poll. */
static Value nat_mouse_clicked(Value *args, int argc) {
    (void)argc;
    int bit;
    if (!mouse_button(&args[0], &bit)) return val_int(0);
    return val_int((g_mouse_clicked & bit) != 0);
}

/* input_mouse_released(button) -> int : released since the last poll. */
static Value nat_mouse_released(Value *args, int argc) {
    (void)argc;
    int bit;
    if (!mouse_button(&args[0], &bit)) return val_int(0);
    return val_int((g_mouse_released & bit) != 0);
}

/* input_mouse_down(button) -> int : held right now. */
static Value nat_mouse_down(Value *args, int argc) {
    (void)argc;
    int bit;
    if (!mouse_button(&args[0], &bit)) return val_int(0);
    return val_int((g_mouse_held & bit) != 0);
}

/* input_mouse_dragged() -> int : moved while a button was held since poll. */
static Value nat_mouse_dragged(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int(g_mouse_dragged);
}

/* input_mouse_wheel() -> int : net ticks since the last poll, + is up. */
static Value nat_mouse_wheel(Value *args, int argc) {
    (void)args;
    (void)argc;
    return val_int(g_mouse_wheel);
}

/* Test hook: decode one key from `bytes`, so the escape-sequence and modifier
   parsing can be exercised without a terminal. */
int input_decode_test(const uint8_t *bytes, int len, int *key, int *mods,
                      InputMouseEvent *mouse) {
    return parse_key(bytes, len, key, mods, mouse);
}

void input_register_natives(void) {
    native_add("input_poll", nat_poll);
    native_add("input_wait", nat_wait);
    native_add("input_is_key_pressed", nat_is_key_pressed);
    native_add("input_mods", nat_mods);
    native_add("input_text", nat_text);
    native_add("input_mouse_x", nat_mouse_x);
    native_add("input_mouse_y", nat_mouse_y);
    native_add("input_mouse_clicked", nat_mouse_clicked);
    native_add("input_mouse_released", nat_mouse_released);
    native_add("input_mouse_down", nat_mouse_down);
    native_add("input_mouse_dragged", nat_mouse_dragged);
    native_add("input_mouse_wheel", nat_mouse_wheel);
}
