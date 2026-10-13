#ifndef OBY_INPUT_H
#define OBY_INPUT_H

#include <stdint.h>

/* Modifier bits, matching `Input::Mod` in src/prelude/Input.oby. */
#define INPUT_MOD_SHIFT 1
#define INPUT_MOD_CTRL  2
#define INPUT_MOD_ALT   4

/* A decoded SGR mouse report (`ESC [ < b ; x ; y M|m`). */
typedef struct {
    int is_mouse;   /* 1 when the decoded sequence was a mouse report */
    int code;       /* the SGR `b` value: button, modifiers, and kind */
    int x, y;       /* 1-based, as reported */
    int release;    /* 1 for the `m` terminator */
} InputMouseEvent;

/* Register the input natives. */
void input_register_natives(void);

/* Test hook: decode one key from `len` bytes of a terminal stream. Returns the
   bytes consumed (0 = incomplete sequence); writes the key code, the modifier
   bits, and any mouse report. A complete but unknown sequence reports key 0
   with `is_mouse` clear. */
int input_decode_test(const uint8_t *bytes, int len, int *key, int *mods,
                      InputMouseEvent *mouse);

#endif // OBY_INPUT_H
