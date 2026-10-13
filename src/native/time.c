#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "native/time.h"
#include "native/native.h"

/*
 * Time natives.
 *
 *   time_sleep_ms(int ms) -> void
 *   time_now_ms() -> int64
 *   time_now_us() -> int64
 *
 * `now_ms` and `now_us` read the same monotonic clock, so `now_us() / 1000`
 * is `now_ms()`: use milliseconds for pacing, microseconds for measuring.
 * The epoch is unspecified; only differences are meaningful.
 */

static Value nat_sleep_ms(Value *args, int argc) {
    (void)argc;
    int ms = (int)args[0].as.i;
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return val_null();
}

static Value nat_now_ms(Value *args, int argc) {
    (void)args;
    (void)argc;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return val_int(0);
    return val_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static Value nat_now_us(Value *args, int argc) {
    (void)args;
    (void)argc;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return val_int(0);
    return val_int((int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

void time_register_natives(void) {
    native_add("time_sleep_ms", nat_sleep_ms);
    native_add("time_now_ms", nat_now_ms);
    native_add("time_now_us", nat_now_us);
}
