/* embed_gen — bake source files into a C table (see `include/core/embed.h`).
 *
 *   embed_gen <prefix> <out.c> <file>...
 *
 * Each file is registered under `<prefix>/<basename>`, folded the same way the
 * compiler folds the paths it asks for (see `path_normalize` in
 * src/compiler/sema.c — keep the two in step). The generated file defines
 * `embed_static_files`; `src/core/embed.c` does the lookup.
 *
 * Run by `compile.sh` into `build/prelude_embed.c`, so the compiler carries the
 * prelude inside the binary and works from any working directory.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "embed_gen: out of memory\n");
        exit(1);
    }
    return p;
}

static bool is_sep(char c) { return c == '/' || c == '\\'; }

/* ---- key folding ------------------------------------------------------- */

/* Fold `path` to its canonical spelling: `\` -> `/`, drop `.` and empty
   segments, cancel `segment/..`. Mirrors path_normalize in sema.c. */
static char *folded(const char *path) {
    size_t len = strlen(path);
    bool absolute = len > 0 && is_sep(path[0]);
    size_t *start = (size_t *)xmalloc((len + 1) * sizeof(size_t));
    size_t *slen = (size_t *)xmalloc((len + 1) * sizeof(size_t));
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && is_sep(path[i])) i++;
        size_t seg = i;
        while (i < len && !is_sep(path[i])) i++;
        size_t seg_len = i - seg;
        if (seg_len == 0) continue;                        /* "//" */
        if (seg_len == 1 && path[seg] == '.') continue;    /* "." */
        if (seg_len == 2 && path[seg] == '.' && path[seg + 1] == '.') {
            bool prev_is_dotdot = n > 0 && slen[n - 1] == 2 &&
                                  path[start[n - 1]] == '.' && path[start[n - 1] + 1] == '.';
            if (n > 0 && !prev_is_dotdot) {                /* `a/..` cancels */
                n--;
                continue;
            }
        }
        start[n] = seg;
        slen[n] = seg_len;
        n++;
    }

    size_t out_len = absolute ? 1 : 0;
    for (size_t k = 0; k < n; k++) out_len += slen[k] + (k > 0 ? 1 : 0);
    char *out = (char *)xmalloc(out_len + 2);
    size_t o = 0;
    if (absolute) out[o++] = '/';
    for (size_t k = 0; k < n; k++) {
        if (k > 0) out[o++] = '/';
        memcpy(out + o, path + start[k], slen[k]);
        o += slen[k];
    }
    if (o == 0) out[o++] = '.';
    out[o] = '\0';
    free(start);
    free(slen);
    return out;
}

/* `<prefix>/<basename(file)>`, folded: the path the compiler will ask for. */
static char *key_for(const char *prefix, const char *file) {
    const char *base = file + strlen(file);
    while (base > file && !is_sep(base[-1])) base--;
    size_t plen = strlen(prefix), blen = strlen(file) - (size_t)(base - file);
    char *joined = (char *)xmalloc(plen + 1 + blen + 1);
    memcpy(joined, prefix, plen);
    joined[plen] = '/';
    memcpy(joined + plen + 1, base, blen + 1);
    char *key = folded(joined);
    free(joined);
    return key;
}

/* A C identifier for a file name (the key's basename): `File.oby` ->
   `File_oby`. */
static void ident_for(const char *name, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = name; *p && o + 1 < cap; p++) {
        char c = *p;
        bool ok = c == '_' || (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
}

/* ---- input ------------------------------------------------------------- */

typedef struct {
    char *key;
    char ident[64];
    unsigned char *data;
    size_t len;
} Input;

static bool read_bytes(const char *path, unsigned char **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    unsigned char *buf = (unsigned char *)xmalloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *data = buf;
    *len = got;
    return true;
}

/* ---- output ------------------------------------------------------------ */

static void emit_array(FILE *out, const Input *in) {
    fprintf(out, "static const unsigned char %s[] = {\n", in->ident);
    if (in->len == 0) fputs("    0\n", out);
    for (size_t i = 0; i < in->len; i++) {
        if (i % 12 == 0) fputs("    ", out);
        fprintf(out, "0x%02x,", in->data[i]);
        if (i % 12 == 11 || i + 1 == in->len) fputc('\n', out);
        else fputc(' ', out);
    }
    fputs("};\n\n", out);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <prefix> <out.c> <file>...\n", argv[0]);
        return 1;
    }
    const char *prefix = argv[1];
    const char *out_path = argv[2];
    size_t count = (size_t)(argc - 3);
    Input *inputs = (Input *)xmalloc(count * sizeof(Input));

    for (size_t i = 0; i < count; i++) {
        const char *path = argv[3 + i];
        inputs[i].key = key_for(prefix, path);
        ident_for(inputs[i].key + (strlen(prefix) + 1), inputs[i].ident,
                  sizeof inputs[i].ident);
        if (!read_bytes(path, &inputs[i].data, &inputs[i].len)) {
            fprintf(stderr, "embed_gen: cannot read '%s'\n", path);
            return 1;
        }
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "embed_gen: cannot write '%s'\n", out_path);
        return 1;
    }

    fputs("/* Generated by tools/embed_gen.c — do not edit.\n"
          " *\n"
          " * The prelude, baked into the binary so the compiler finds it from\n"
          " * any working directory. See include/core/embed.h. */\n"
          "#include <stddef.h>\n"
          "#include \"core/embed.h\"\n\n", out);

    for (size_t i = 0; i < count; i++) emit_array(out, &inputs[i]);

    fputs("static const EmbeddedFile embedded_files[] = {\n", out);
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "    { \"%s\", (const char *)%s, %zu },\n",
                inputs[i].key, inputs[i].ident, inputs[i].len);
    }
    fputs("};\n\n"
          "size_t embed_static_files(const EmbeddedFile **files) {\n"
          "    *files = embedded_files;\n"
          "    return sizeof embedded_files / sizeof embedded_files[0];\n"
          "}\n", out);

    if (fclose(out) != 0) {
        fprintf(stderr, "embed_gen: cannot write '%s'\n", out_path);
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        free(inputs[i].key);
        free(inputs[i].data);
    }
    free(inputs);
    return 0;
}
