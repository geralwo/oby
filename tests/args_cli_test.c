/* Command-line arguments: `args()[0]` is the program (the script path) and the
   rest are its arguments. Everything after the script path is the script's, so
   `oby` is driven through the shell here — the harness itself runs tests without
   arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR    "build/args_cli"
#define SCRIPT DIR "/echo.oby"
#define OUT    DIR "/out.txt"

static int fails = 0;

static void check(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static int write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    fputs(text, file);
    return fclose(file) == 0;
}

/* Read a whole file; NULL on failure. Caller frees. */
static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    buf[got] = '\0';
    return buf;
}

/* Run `command` (which must write its stdout to OUT) and compare the result. */
static void check_output(const char *what, const char *command, const char *want) {
    if (system(command) != 0) {
        check(what, 0);
        return;
    }
    char *got = read_all(OUT);
    int ok = got != NULL && strcmp(got, want) == 0;
    check(what, ok);
    if (!ok && got) printf("  got: %s", got);
    free(got);
}

int main(void) {
    check("scratch directory", system("mkdir -p " DIR) == 0);
    check("echo script written",
          write_file(SCRIPT, "print(\"${args().length}\\n\")\n"
                             "for (local i = 0; i < args().length; i += 1) {\n"
                             "\tprint(\"${args()[i]}\\n\")\n"
                             "}\n"));

    check_output("no arguments", "build/oby " SCRIPT " > " OUT,
                 "1\n" SCRIPT "\n");
    check_output("one argument", "build/oby " SCRIPT " one > " OUT,
                 "2\n" SCRIPT "\none\n");
    check_output("arguments are passed verbatim",
                 "build/oby " SCRIPT " alpha --level 3 -x > " OUT,
                 "5\n" SCRIPT "\nalpha\n--level\n3\n-x\n");
    check_output("a quoted argument survives",
                 "build/oby " SCRIPT " 'two words' > " OUT,
                 "2\n" SCRIPT "\ntwo words\n");

    check("-check accepts trailing arguments",
          system("build/oby -check " SCRIPT " alpha > /dev/null 2>&1") == 0);

    system("rm -rf " DIR);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
