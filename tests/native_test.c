/*
 * Standalone test for the native registry. Built and run by `./compile.sh test`
 * (every C file under tests/ becomes its own binary and must exit 0).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "native/native.h"

static int failures = 0;

static void check(const char *what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL %s: got %lld, want %lld\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %s = %lld\n", what, got);
    }
}

static Value dummy_add(Value *args, int argc) {
    int64_t sum = 0;
    for (int i = 0; i < argc; i++) sum += args[i].as.i;
    return val_int(sum);
}

int main(void) {
    native_init();

    check("native_add", native_add("dummy_add", dummy_add) ? 1 : 0, 1);
    int id = native_find("dummy_add");
    check("native_find(\"dummy_add\")", id, 0);
    check("native_find(\"missing\")", native_find("missing"), -1);
    check("native_add rejects a duplicate", native_add("dummy_add", dummy_add) ? 0 : 1, 1);

    Value args[3] = { val_int(1), val_int(2), val_int(39) };
    Value r = native_call(id, args, 3);
    check("native_call(dummy_add, 1, 2, 39)", r.as.i, 42);

    const char *name = native_name(id);
    if (!name || strcmp(name, "dummy_add") != 0) {
        printf("  FAIL native_name\n");
        failures++;
    } else {
        printf("  ok   native_name = %s\n", name);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all ok\n");
    return 0;
}
