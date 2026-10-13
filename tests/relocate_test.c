/* The prelude is baked into the binary, so the compiler must work from any
   working directory. `tests/` has no `src/prelude` below it — running from
   there used to fail with `cannot read import "src/prelude/core.oby"`.
   (The harness runs tests from the repository root.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCH "build/relocate_scratch.oby"
#define ACTUAL  "build/relocate_actual.out"

static int fails = 0;

static void check(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* A script whose value comes from the prelude: typecheck and run it with
       `tests/` as the working directory. */
    FILE *f = fopen(SCRATCH, "wb");
    if (!f) {
        printf("cannot write %s\n", SCRATCH);
        return 1;
    }
    fputs("l = List<int>()\n"
          "l.push(20)\n"
          "l.push(22)\n"
          "print(\"${l.at(0) + l.at(1)}\\n\")\n", f);
    fclose(f);

    check("check from foreign cwd",
          system("cd tests && ../build/oby -check ../" SCRATCH) == 0);
    check("run from foreign cwd",
          system("cd tests && ../build/oby ../" SCRATCH " > ../" ACTUAL) == 0);

    char out[64] = {0};
    FILE *in = fopen(ACTUAL, "rb");
    if (in) {
        size_t n = fread(out, 1, sizeof out - 1, in);
        out[n] = '\0';
        fclose(in);
    }
    check("output survives relocation", strcmp(out, "42\n") == 0);

    remove(SCRATCH);
    remove(ACTUAL);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
