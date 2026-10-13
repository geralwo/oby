/* The payload archive and the bundled-binary startup path. Wraps this
   project's `oby` around an entry plus a helper, then runs the result from a
   directory that holds a decoy `helper.oby`: the payload must win, because a
   bundled binary reads its script from itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/archive.h"
#include "core/bundle.h"

#define DIR     "build/bundle_run"
#define BINARY  DIR "/game"
#define DECOY   DIR "/helper.oby"
#define OUTPUT  DIR "/out.txt"
#define ARCHIVE DIR "/archive.bin"

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
static char *read_all(const char *path, size_t *len) {
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
    if (len) *len = got;
    return buf;
}

int main(void) {
    const char *entry_src  =
        "use \"helper.oby\"\n"
        "l = List<String>()\n"
        "l.push(where())\n"
        "print(l.at(0))\n";
    const char *helper_src = "where = () -> String { return \"payload\\n\" }\n";
    const char *decoy_src  = "where = () -> String { return \"disk\\n\" }\n";

    check("scratch directory", system("mkdir -p " DIR) == 0);
    check("decoy helper written", write_file(DECOY, decoy_src));

    ArchiveFile files[2];
    files[0].path = "main.oby";
    files[0].data = entry_src;
    files[0].len = strlen(entry_src);
    files[1].path = "helper.oby";
    files[1].data = helper_src;
    files[1].len = strlen(helper_src);

    /* Round trip: write a standalone archive and parse it back. */
    FILE *arc = fopen(ARCHIVE, "wb");
    uint64_t arc_len = 0;
    check("archive written", arc && archive_write(arc, files, 2, &arc_len));
    if (arc) fclose(arc);

    size_t blob_len = 0;
    char *blob = read_all(ARCHIVE, &blob_len);
    Archive parsed;
    check("archive parses", blob && archive_parse((unsigned char *)blob, blob_len, &parsed));
    check("archive file count", blob && parsed.count == 2);
    check("archive keeps the entry first",
          blob && parsed.count == 2 && strcmp(parsed.files[0].path, "main.oby") == 0 &&
          parsed.files[0].len == strlen(entry_src) &&
          memcmp(parsed.files[0].data, entry_src, parsed.files[0].len) == 0);
    check("archive keeps the helper",
          blob && parsed.count == 2 && strcmp(parsed.files[1].path, "helper.oby") == 0 &&
          memcmp(parsed.files[1].data, helper_src, strlen(helper_src)) == 0);
    if (blob) archive_free(&parsed);
    free(blob);

    /* Malformed archives are refused, not trusted. */
    Archive bad;
    unsigned char short_count[8] = {1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    check("archive with a bogus length refused",
          !archive_parse(short_count, sizeof short_count, &bad));
    unsigned char trailing[5] = {0, 0, 0, 0, 0xaa};
    check("archive with trailing bytes refused",
          !archive_parse(trailing, sizeof trailing, &bad));

    /* A bundle: host binary + archive + footer. */
    check("host binary copied", system("cp build/oby " BINARY) == 0);
    FILE *out = fopen(BINARY, "ab");
    bool built = false;
    if (out) {
        uint64_t payload_len = 0;
        built = archive_write(out, files, 2, &payload_len) &&
                bundle_write_footer(out, payload_len, OBY_BUNDLE_TYPE_SOURCE);
        fclose(out);
    }
    check("bundle built", built);
    check("bundle has a footer", bundle_has_footer(BINARY));
    system("chmod +x " BINARY);

    /* Run it from DIR, where `helper.oby` says something else. */
    check("bundled binary runs", system("cd " DIR " && ./game > out.txt") == 0);
    size_t out_len = 0;
    char *got = read_all(OUTPUT, &out_len);
    check("bundle runs its own payload", got && strcmp(got, "payload\n") == 0);
    if (got && strcmp(got, "payload\n") != 0) printf("  got: %s", got);
    free(got);

    /* A payload type this build does not know is refused, not ignored. */
    check("host binary re-copied", system("cp build/oby " BINARY) == 0);
    FILE *weird = fopen(BINARY, "ab");
    bool footer_written = weird && bundle_write_footer(weird, 0, 9);
    if (weird) fclose(weird);
    check("unknown payload type footer", footer_written);
    check("unknown payload type refused",
          system("cd " DIR " && ./game > /dev/null 2>&1") != 0);

    /* Diagnostics in a bundle name the payload's file, and the exit status
       comes from the script. */
    const char *bad_entry = "print(nope())\n";
    ArchiveFile bad_files[1];
    bad_files[0].path = "main.oby";
    bad_files[0].data = bad_entry;
    bad_files[0].len = strlen(bad_entry);
    check("host binary re-copied for the failing bundle",
          system("cp build/oby " BINARY) == 0);
    FILE *bad_out = fopen(BINARY, "ab");
    bool bad_built = false;
    if (bad_out) {
        uint64_t payload_len = 0;
        bad_built = archive_write(bad_out, bad_files, 1, &payload_len) &&
                    bundle_write_footer(bad_out, payload_len, OBY_BUNDLE_TYPE_SOURCE);
        fclose(bad_out);
    }
    check("failing bundle built", bad_built);
    check("bundled error exits non-zero",
          system("cd " DIR " && ./game > /dev/null 2> err.txt") != 0);
    size_t err_len = 0;
    char *err = read_all(DIR "/err.txt", &err_len);
    check("bundled error names the payload file",
          err != NULL && strstr(err, "main.oby") != NULL);
    free(err);

    system("rm -rf " DIR);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
