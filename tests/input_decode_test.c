// The escape-sequence and modifier decoder, without a terminal: feed byte
// streams to the test hook and check key codes and modifier bits.

#include <stdio.h>
#include "native/input.h"

/* Key codes, matching Input::Key in src/prelude/Input.oby. */
#define K_ENTER     13
#define K_ESCAPE    27
#define K_BACKSPACE 127
#define K_UP        256
#define K_LEFT      258
#define K_DELETE    261
#define K_PAGEUP    264
#define K_F1        266
#define K_F5        270

static int failures = 0;

/* `used` is how many bytes the decoder should consume; a complete sequence
   consumes all of them. Non-mouse sequences must not report a mouse event. */
static void expect_n(const char *name, const char *bytes, int len, int used,
                     int key, int mods) {
    int k = -1, m = -1;
    InputMouseEvent me;
    int got = input_decode_test((const unsigned char *)bytes, len, &k, &m, &me);
    if (got != used || k != key || m != mods || me.is_mouse) {
        printf("FAIL %s: used=%d (want %d) key=%d (want %d) mods=%d (want %d) mouse=%d\n",
               name, got, used, k, key, m, mods, me.is_mouse);
        failures++;
    }
}

static void expect(const char *name, const char *bytes, int len, int key, int mods) {
    expect_n(name, bytes, len, len, key, mods);
}

/* `code` is the raw SGR b value; `x`/`y` are 1-based, as reported. */
static void expect_mouse(const char *name, const char *bytes, int len,
                         int code, int x, int y, int release) {
    int k = -1, m = -1;
    InputMouseEvent me;
    int got = input_decode_test((const unsigned char *)bytes, len, &k, &m, &me);
    if (got != len || !me.is_mouse || me.code != code || me.x != x ||
        me.y != y || me.release != release) {
        printf("FAIL %s: used=%d (want %d) mouse=%d code=%d (want %d) x=%d y=%d rel=%d (want %d)\n",
               name, got, len, me.is_mouse, me.code, code, me.x, me.y, me.release, release);
        failures++;
    }
}

int main(void) {
    expect("plain letter", "a", 1, 'a', 0);
    expect("capital letter", "A", 1, 'a', INPUT_MOD_SHIFT);
    expect("ctrl+a", "\x01", 1, 'a', INPUT_MOD_CTRL);
    expect("ctrl+z", "\x1a", 1, 'z', INPUT_MOD_CTRL);
    expect("enter cr", "\r", 1, K_ENTER, 0);
    expect("enter lf", "\n", 1, K_ENTER, 0);
    expect("tab", "\t", 1, 9, 0);
    expect("backspace", "\x7f", 1, K_BACKSPACE, 0);
    expect("space", " ", 1, 32, 0);

    expect("up", "\x1b[A", 3, K_UP, 0);
    expect("left", "\x1b[D", 3, K_LEFT, 0);
    expect("delete", "\x1b[3~", 4, K_DELETE, 0);
    expect("pageup", "\x1b[5~", 4, K_PAGEUP, 0);
    expect("f5", "\x1b[15~", 5, K_F5, 0);
    expect("application f1", "\x1bOP", 3, K_F1, 0);

    expect("ctrl+up", "\x1b[1;5A", 6, K_UP, INPUT_MOD_CTRL);
    expect("shift+up", "\x1b[1;2A", 6, K_UP, INPUT_MOD_SHIFT);
    expect("alt+up", "\x1b[1;3A", 6, K_UP, INPUT_MOD_ALT);
    expect("ctrl+shift+up", "\x1b[1;6A", 6, K_UP, INPUT_MOD_CTRL | INPUT_MOD_SHIFT);
    expect("ctrl+delete", "\x1b[3;5~", 6, K_DELETE, INPUT_MOD_CTRL);
    expect("ctrl+pageup", "\x1b[5;5~", 6, K_PAGEUP, INPUT_MOD_CTRL);

    expect_mouse("left press", "\x1b[<0;10;5M", 10, 0, 10, 5, 0);
    expect_mouse("left release", "\x1b[<0;10;5m", 10, 0, 10, 5, 1);
    expect_mouse("motion while held", "\x1b[<32;12;6M", 11, 32, 12, 6, 0);
    expect_mouse("wheel up", "\x1b[<64;3;4M", 10, 64, 3, 4, 0);
    expect_mouse("ctrl+left press", "\x1b[<16;1;1M", 10, 16, 1, 1, 0);
    expect_n("malformed mouse report", "\x1b[<0;1X", 7, 7, 0, 0);

    /* Incomplete sequences consume nothing; `poll`'s flush decides them. */
    expect_n("lone esc", "\x1b", 1, 0, 0, 0);
    expect_n("partial csi", "\x1b[1;", 4, 0, 0, 0);
    expect_n("esc then key", "\x1bx", 2, 1, K_ESCAPE, 0);

    if (failures) {
        printf("%d decode test(s) failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
