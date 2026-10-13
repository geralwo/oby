/* Self-path resolution and the bundle footer format. The footer is a file
   format, so the byte layout is checked directly, not just through the
   read/write pair. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/bundle.h"
#include "core/os.h"

#define SCRATCH "build/bundle_footer_scratch.bin"

static int fails = 0;

static void check(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

/* A file that is not a bundle: the honest answer is no footer. */
static void check_no_footer(const char *path, const char *what) {
    check(what, !bundle_has_footer(path));
}

int main(int argc, char **argv) {
    (void)argc;

    char exe[4096] = {0};
    check("self path resolves", os_get_executable_path(argv[0], exe, sizeof exe));
    check("self path names this binary",
          strcmp(base_name(argv[0]), base_name(exe)) == 0);

    FILE *self = fopen(exe, "rb");
    check("self path opens", self != NULL);
    if (self) {
        BundleFooter footer;
        check("self path has no footer", !bundle_read_footer(self, &footer));
        fclose(self);
    }

    char tiny[2];
    check("too-small buffer is refused",
          !os_get_executable_path(argv[0], tiny, sizeof tiny));

    check_no_footer("build/no_such_file.bin", "missing file has no footer");

    /* A payload and its footer, written the way the bundler will. */
    FILE *out = fopen(SCRATCH, "wb");
    if (!out) {
        printf("cannot write %s\n", SCRATCH);
        return 1;
    }
    fputs("HOST", out);
    fputs("PAYLOAD", out);
    check("write footer", bundle_write_footer(out, 7, OBY_BUNDLE_TYPE_SOURCE));
    fclose(out);

    check("bundled file has a footer", bundle_has_footer(SCRATCH));

    FILE *in = fopen(SCRATCH, "rb");
    BundleFooter footer = {0, 0, 0};
    check("read footer", in && bundle_read_footer(in, &footer));
    check("footer payload length", footer.payload_len == 7);
    check("footer payload type", footer.payload_type == OBY_BUNDLE_TYPE_SOURCE);
    check("footer reserved is zero", footer.reserved == 0);

    /* The bytes on disk: little-endian length, magic at offset 16. */
    unsigned char raw[OBY_BUNDLE_FOOTER_SIZE] = {0};
    if (in) {
        fseek(in, -OBY_BUNDLE_FOOTER_SIZE, SEEK_END);
        check("raw footer read",
              fread(raw, 1, sizeof raw, in) == sizeof raw);
        fclose(in);
    }
    check("footer length is little-endian", raw[0] == 7 && raw[1] == 0);
    check("footer magic",
          memcmp(raw + 16, OBY_BUNDLE_MAGIC, sizeof OBY_BUNDLE_MAGIC - 1) == 0);
    remove(SCRATCH);

    /* Shorter than a footer, and exactly a footer with the wrong bytes. */
    FILE *short_file = fopen(SCRATCH, "wb");
    if (short_file) {
        fputs("short", short_file);
        fclose(short_file);
    }
    check_no_footer(SCRATCH, "file shorter than a footer");

    unsigned char zeros[OBY_BUNDLE_FOOTER_SIZE] = {0};
    FILE *zero_file = fopen(SCRATCH, "wb");
    if (zero_file) {
        fwrite(zeros, 1, sizeof zeros, zero_file);
        fclose(zero_file);
    }
    check_no_footer(SCRATCH, "footer-sized file without the magic");
    remove(SCRATCH);

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
