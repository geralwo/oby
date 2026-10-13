#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
#include "native/random.h"
#include "native/native.h"

/*
 * ob_rand(int n) -> int
 *
 * A pseudo-random value in `0..n-1`, seeded from the clock. `n <= 0` yields 0.
 */
static Value nat_rand(Value *args, int argc) {
    (void)argc;
    int64_t n = args[0].as.i;
    if (n <= 0) return val_int(0);
    return val_int((int64_t)(rand() % n));
}

void random_register_natives(void) {
    srand((unsigned)time(NULL));
    native_add("ob_rand", nat_rand);
}
