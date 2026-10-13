#include <stdlib.h>
#include "core/value.h"

/* A closure owns its captured bytes; freeing it does not free heap values the
   captures point at (same shallow rule as `object::free` for objects). */

Closure *closure_new(Function *fn, size_t up_total) {
    Closure *c = (Closure *)malloc(sizeof(Closure));
    if (!c) return NULL;
    c->fn = fn;
    c->up_total = up_total;
    c->up = up_total ? (uint8_t *)malloc(up_total) : NULL;
    if (up_total && !c->up) { free(c); return NULL; }
    return c;
}

void closure_free(Closure *c) {
    if (!c) return;
    free(c->up);
    free(c);
}