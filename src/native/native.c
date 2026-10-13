#include <string.h>
#include "native/native.h"
#include "native/io.h"
#include "native/strings.h"
#include "native/memory.h"
#include "native/terminal.h"
#include "native/input.h"
#include "native/random.h"
#include "native/time.h"
#include "core/numbers.h"
#include "core/array.h"

typedef struct {
    const char *name;
    NativeFn fn;
} NativeEntry;

/* The registry is a growable array of entries; the index is the native id.
   It is process-global, matching how the VM will look natives up by id. */
static Array *g_natives = NULL;

void native_init(void) {
    if (!g_natives) g_natives = array_new(sizeof(NativeEntry));
}

int native_find(const char *name) {
    if (!g_natives || !name) return -1;
    for (size_t i = 0; i < array_length(g_natives); i++) {
        NativeEntry *e = (NativeEntry *)array_get(g_natives, i);
        if (strcmp(e->name, name) == 0) return (int)i;
    }
    return -1;
}

bool native_add(const char *name, NativeFn fn) {
    native_init();
    if (native_find(name) >= 0) return false;
    NativeEntry e = { name, fn };
    array_push(g_natives, &e);
    return true;
}

const char *native_name(int id) {
    if (!g_natives || id < 0 || (size_t)id >= array_length(g_natives)) return NULL;
    return ((NativeEntry *)array_get(g_natives, (size_t)id))->name;
}

Value native_call(int id, Value *args, int argc) {
    if (!g_natives || id < 0 || (size_t)id >= array_length(g_natives)) return val_null();
    NativeEntry *e = (NativeEntry *)array_get(g_natives, (size_t)id);
    return e->fn(args, argc);
}

/* ------------------------------------------------------------------ */
/* Built-in natives                                                   */
/* ------------------------------------------------------------------ */

/* Register the built-in natives the prelude depends on. Idempotent.
   Each domain file exposes a `*_register_natives()` hook; add new ones here. */
void native_register_std(void) {
    native_init();
    io_register_natives();
    numbers_register_natives();
    strings_register_natives();
    memory_register_natives();
    terminal_register_natives();
    input_register_natives();
    random_register_natives();
    time_register_natives();
}

uint64_t native_fingerprint(void) {
    native_register_std();

    /* FNV-1a over each name including its terminator, in id order. */
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int id = 0; ; id++) {
        const char *name = native_name(id);
        if (!name) break;
        for (const char *p = name; ; p++) {
            hash ^= (unsigned char)*p;
            hash *= 0x100000001b3ull;
            if (*p == '\0') break;
        }
    }
    return hash;
}
