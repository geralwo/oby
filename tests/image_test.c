/* The bytecode image: an image_write/image_load round trip (code, every
   constant kind, closure metadata), the refusals, and a smoke test that
   `obyrun` executes a hand-built image and rejects what it cannot run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/archive.h"
#include "core/bundle.h"
#include "vm/image.h"
#include "vm/opcode.h"

#define DIR    "build/image_scratch"
#define BINARY DIR "/app"

static int fails = 0;

static void check(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* Free a function the test built by hand: its name is a string literal, so
   only the constant strings, the arrays and the struct go. */
static void free_built(Function *fn) {
    for (size_t i = 0; i < fn->const_count; i++) {
        if (fn->consts[i].tag == VAL_STRING) string_free(fn->consts[i].as.str);
    }
    function_free(fn);
    free(fn);
}

static Function *built(const char *name, size_t up_count) {
    Function *fn = (Function *)malloc(sizeof *fn);
    function_init(fn, name);
    if (up_count) {
        fn->up_count = up_count;
        fn->up_off = (size_t *)malloc(up_count * sizeof(size_t));
        fn->up_size = (size_t *)malloc(up_count * sizeof(size_t));
        for (size_t i = 0; i < up_count; i++) {
            fn->up_off[i] = i * 4;
            fn->up_size[i] = 4;
        }
        fn->up_total = up_count * 4;
    }
    return fn;
}

/* A copy of `len` bytes with `at` bumped, for the refusal cases. */
static unsigned char *mutated(const unsigned char *image, size_t len, size_t at,
                              unsigned char bump) {
    unsigned char *copy = (unsigned char *)malloc(len);
    memcpy(copy, image, len);
    copy[at] += bump;
    return copy;
}

int main(void) {
    /* helper first: main holds a constant that names it, which is the loader's
       two-pass case. */
    Function *helper = built("helper", 0);
    function_emit(helper, INSTR_ABC(OP_HALT, 0, 0, 0));
    helper->param_count = 1;
    helper->frame_size = 16;
    function_add_const(helper, val_int(-1));
    function_add_const(helper, val_string(string_new("")));

    Function *main_fn = built("main", 2);
    function_emit(main_fn, INSTR_ABXN(OP_LOADK, 0, 0, KIND_INT64));
    function_emit(main_fn, INSTR_ABC(OP_HALT, 0, 0, 0));
    main_fn->frame_size = 32;
    function_add_const(main_fn, val_int(42));
    function_add_const(main_fn, val_float(2.5));
    function_add_const(main_fn, val_string(string_new("hi")));
    function_add_const(main_fn, val_func(helper));

    Function *both[2] = {main_fn, helper};
    size_t len = 0;
    unsigned char *image = image_write(both, 2, 0, 1234, &len);
    check("image written", image != NULL && len > 0);

    Image loaded = {0};
    bool loads = image != NULL && image_load(image, len, &loaded);
    check("image loads", loads);
    if (loads) {
        check("entry is the first function", loaded.entry == loaded.functions[0]);
        check("function count", loaded.count == 2);
        check("global size", loaded.global_size == 1234);

        Function *m = loaded.functions[0];
        Function *h = loaded.functions[1];
        check("names", strcmp(m->name, "main") == 0 && strcmp(h->name, "helper") == 0);
        check("code", m->code_len == 2 && m->code[0] == main_fn->code[0] &&
                          m->code[1] == main_fn->code[1]);
        check("frame size", m->frame_size == 32 && h->frame_size == 16);
        check("param count", m->param_count == 0 && h->param_count == 1);
        check("closure metadata",
              m->up_count == 2 && m->up_total == 8 && m->up_off[1] == 4 &&
                  m->up_size[0] == 4 && h->up_count == 0);

        check("const count", m->const_count == 4 && h->const_count == 2);
        check("int const", m->consts[0].tag == VAL_INT && m->consts[0].as.i == 42);
        check("float const", m->consts[1].tag == VAL_FLOAT && m->consts[1].as.f == 2.5);
        check("string const",
              m->consts[2].tag == VAL_STRING && m->consts[2].as.str->length == 2 &&
                  memcmp(m->consts[2].as.str->data, "hi", 2) == 0);
        check("function const resolves forward",
              m->consts[3].tag == VAL_FUNC && m->consts[3].as.fn == h);
        check("empty string const",
              h->consts[1].tag == VAL_STRING && h->consts[1].as.str->length == 0);
        image_free(&loaded);
    }

    /* Refusals; every mutation is a copy, so the good image stays good. */
    if (image) {
        Image tmp = {0};
        unsigned char *bad;
        bad = mutated(image, len, 0, 1); /* magic */
        check("bad magic refused", !image_load(bad, len, &tmp));
        free(bad);
        bad = mutated(image, len, 8, 1); /* format_version */
        check("unknown version refused", !image_load(bad, len, &tmp));
        free(bad);
        bad = mutated(image, len, 12, 1); /* native_hash */
        check("native fingerprint mismatch refused", !image_load(bad, len, &tmp));
        free(bad);
        check("truncated image refused", !image_load(image, len - 1, &tmp));
        bad = (unsigned char *)malloc(len + 1);
        memcpy(bad, image, len);
        bad[len] = 0xaa;
        check("trailing bytes refused", !image_load(bad, len + 1, &tmp));
        free(bad);
    }

    free_built(main_fn);
    free_built(helper);

    /* obyrun: a minimal image (just HALT) must run, from any directory. */
    check("scratch directory", system("mkdir -p " DIR) == 0);

    Function *tiny = built("main", 0);
    function_emit(tiny, INSTR_ABC(OP_HALT, 0, 0, 0));
    Function *one[1] = {tiny};
    size_t tiny_len = 0;
    unsigned char *tiny_image = image_write(one, 1, 0, 0, &tiny_len);
    check("minimal image written", tiny_image && tiny_len > 0);

    check("runner copied", system("cp build/obyrun " BINARY) == 0);
    FILE *out = fopen(BINARY, "ab");
    bool bundled = false;
    if (out) {
        bundled = tiny_image &&
                  fwrite(tiny_image, 1, tiny_len, out) == tiny_len &&
                  bundle_write_footer(out, tiny_len, OBY_BUNDLE_TYPE_IMAGE);
        fclose(out);
    }
    check("image appended", bundled);
    system("chmod +x " BINARY);

    check("obyrun runs the image from a foreign cwd",
          system("cd tests && ../" BINARY) == 0);
    check("plain obyrun refuses to run",
          system("build/obyrun > /dev/null 2>&1") != 0);

    /* A runner handed a source archive says so instead of guessing. */
    const char *text = "print(1)\n";
    ArchiveFile src_file = {"main.oby", text, strlen(text)};
    check("runner copied for the source archive", system("cp build/obyrun " BINARY) == 0);
    FILE *src_out = fopen(BINARY, "ab");
    bool src_bundled = false;
    if (src_out) {
        uint64_t archive_len = 0;
        src_bundled = archive_write(src_out, &src_file, 1, &archive_len) &&
                      bundle_write_footer(src_out, archive_len, OBY_BUNDLE_TYPE_SOURCE);
        fclose(src_out);
    }
    check("source archive appended", src_bundled);
    check("obyrun refuses a source archive",
          system(BINARY " > /dev/null 2>&1") != 0);

    free(tiny_image);
    free_built(tiny);
    free(image);
    system("rm -rf " DIR);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
