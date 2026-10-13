/* --bundle end to end: an OBY script becomes a standalone executable that
   needs nothing else. The source run is the oracle; the artifact is then run
   from a foreign working directory, and again after its sources are deleted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR    "build/bundle_image"
#define SCRIPT DIR "/app.oby"
#define HELPER DIR "/helper.oby"
#define GOOD   DIR "/good.oby"
#define BROKEN DIR "/broken.oby"
#define APP    DIR "/app"
#define OUT    DIR "/actual.txt"

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

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
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

int main(void) {
    check("scratch directory", system("mkdir -p " DIR) == 0);
    check("helper written",
          write_file(HELPER, "score = (int n) -> int { return n * n }\n"));
    check("script written",
          write_file(SCRIPT, "use \"helper.oby\"\n"
                             "l = List<int>()\n"
                             "l.push(score(6))\n"
                             "l.push(score(7))\n"
                             "print(\"${l.at(0) + l.at(1)}\\n\")\n"));

    /* The source run is the oracle: 36 + 49. */
    check("source run", system("build/oby " SCRIPT " > " DIR "/expected.txt") == 0);
    char *expected = read_all(DIR "/expected.txt");
    check("oracle output", expected && strcmp(expected, "85\n") == 0);
    free(expected);

    /* Bundle, with nothing but `oby` on hand. */
    check("bundle", system("build/oby --bundle " SCRIPT " " APP " > /dev/null") == 0);
    check("output written", file_exists(APP));

    /* Run the artifact from a foreign working directory. */
    check("bundled run", system("cd tests && ../" APP " > ../" OUT) == 0);
    char *actual = read_all(OUT);
    check("bundled output matches the oracle", actual && strcmp(actual, "85\n") == 0);
    free(actual);

    /* Its sources are not needed: delete them and run again. */
    check("sources deleted", system("rm -f " SCRIPT " " HELPER) == 0);
    check("bundled run without sources", system("cd tests && ../" APP " > ../" OUT) == 0);
    actual = read_all(OUT);
    check("output still correct", actual && strcmp(actual, "85\n") == 0);
    free(actual);

    /* A second script, for the runner-override checks. */
    check("good script written", write_file(GOOD, "print(\"ok\\n\")\n"));

    /* Failures: usage, a missing script, a script with errors, a runner that is
       not there or already carries a bundle. None may leave an output behind. */
    check("--bundle without an output is a usage error",
          system("build/oby --bundle x.oby > /dev/null 2>&1") != 0);
    check("--bundle with -check is a usage error",
          system("build/oby --bundle -check x.oby y > /dev/null 2>&1") != 0);

    check("missing script fails",
          system("build/oby --bundle " DIR "/nope.oby " DIR "/broken > /dev/null 2>&1") != 0);
    check("no output for a missing script", !file_exists(DIR "/broken"));

    check("broken script written", write_file(BROKEN, "print(nope())\n"));
    check("broken script fails the bundle",
          system("build/oby --bundle " BROKEN " " DIR "/broken > /dev/null 2>&1") != 0);
    check("no output for a broken script", !file_exists(DIR "/broken"));

    check("OBY_RUNNER pointing nowhere fails",
          system("OBY_RUNNER=" DIR "/nope build/oby --bundle " GOOD " " DIR "/broken > /dev/null 2>&1") != 0);
    check("no output when the runner is missing", !file_exists(DIR "/broken"));

    check("OBY_RUNNER that is already bundled is refused",
          system("OBY_RUNNER=" APP " build/oby --bundle " GOOD " " DIR "/broken > /dev/null 2>&1") != 0);
    check("no output for a bundled runner", !file_exists(DIR "/broken"));

    /* An override runner is used when it is a real one. */
    check("OBY_RUNNER override bundles",
          system("OBY_RUNNER=build/obyrun build/oby --bundle " GOOD " " DIR "/override > /dev/null") == 0);
    check("override artifact runs", system("cd tests && ../" DIR "/override > ../" OUT) == 0);
    actual = read_all(OUT);
    check("override artifact output", actual && strcmp(actual, "ok\n") == 0);
    free(actual);

    system("rm -rf " DIR);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
