#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "util.h"
#include "core/arena.h"
#include "core/bundle.h"
#include "core/os.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"
#include "compiler/ast_printer.h"
#include "compiler/sema.h"
#include "compiler/codegen.h"
#include "compiler/diag.h"
#include "vm/image.h"
#include "vm/vm.h"

const char *oby_version_str = "0.6.4";

/* Command-line options. */
typedef struct {
    const char *input;   /* script to compile, or NULL */
    const char *output;  /* --bundle: the executable to write */
    bool bundle;         /* --bundle: build a standalone executable */
    bool print_ast;      /* -printast: dump the typed AST */
    bool check_only;     /* -check: stop after diagnostics */
} Options;

static void print_usage(const char *prog) {
    printf("oby %s - compiler for the OBY scripting language\n\n", oby_version_str);
    printf("usage: %s [options] <script.oby> [arguments...]\n\n", prog);
    printf("options:\n");
    printf("  -check         check the script for errors (parse + typecheck)\n");
    printf("  -printast      print the parsed and typed AST\n");
    printf("  --bundle <script.oby> <output>\n");
    printf("                 write <output> as a standalone executable: this binary\n");
    printf("                 plus the compiled program (no files, no toolchain)\n");
    printf("  -h, --help     show this help\n");
    printf("  -v, --version  show the version\n");
}

/* Parse argv into `opts`. Returns false on a usage error. */
static bool parse_args(int argc, char **argv, Options *opts) {
    opts->input = NULL;
    opts->output = NULL;
    opts->bundle = false;
    opts->print_ast = false;
    opts->check_only = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* Everything after the script path belongs to the script. */
        if (opts->input != NULL) {
            if (!opts->bundle) break;
            if (opts->output == NULL) {
                opts->output = arg;
                continue;
            }
            fprintf(stderr,
                    "error: --bundle takes an entry script and an output path, not '%s'\n",
                    arg);
            return false;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("oby %s\n", oby_version_str);
            exit(0);
        } else if (strcmp(arg, "--bundle") == 0) {
            opts->bundle = true;
        } else if (strcmp(arg, "-check") == 0) {
            opts->check_only = true;
        } else if (strcmp(arg, "-printast") == 0) {
            opts->print_ast = true;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            print_usage(argv[0]);
            return false;
        } else {
            opts->input = arg;
        }
    }

    if (opts->bundle && (opts->check_only || opts->print_ast)) {
        fprintf(stderr, "error: --bundle cannot be combined with -check or -printast\n");
        return false;
    }
    if (opts->bundle && opts->output == NULL) {
        fprintf(stderr, "error: --bundle needs an entry script and an output path\n");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Compiling                                                          */
/* ------------------------------------------------------------------ */

/* One compilation in progress: the frontend's arena, diagnostics, and the
   checked program. Everything the frontend allocated lives in the arena. */
typedef struct {
    Diagnostics diag;
    Arena       arena;
    Sema        sema;
    ASTNode    *program;
    char       *source;  /* owned */
} Frontend;

/* Parse and typecheck `source`, which is named `label` in diagnostics and is
   the base `use` resolves against. Prints the AST and the summary; false when
   any diagnostic was an error. */
static bool frontend_run(Frontend *fe, const char *label, char *source, bool want_ast) {
    fe->source = source;
    diag_init(&fe->diag);
    diag_set_source(&fe->diag, label, source);

    arena_init(&fe->arena);

    Lexer lexer;
    lexer_init(&lexer, source);
    Parser parser;
    parser_init(&parser, &lexer, &fe->arena, &fe->diag);
    fe->program = parse_program(&parser);

    if (parser.had_error) {
        diag_summary(&fe->diag);
        return false;
    }

    sema_init(&fe->sema, &fe->arena, &fe->diag);
    sema_check(&fe->sema, fe->program, label);

    if (want_ast && fe->diag.error_count == 0) print_ast(fe->program, 0);
    diag_summary(&fe->diag);
    return fe->diag.error_count == 0;
}

static void frontend_free(Frontend *fe) {
    arena_free(&fe->arena);
    free(fe->source);
}

/* Compile an in-memory source buffer and, unless `run` is false, run it.
   Takes ownership of `source`. Returns the process status. */
static int compile_source(const char *label, char *source, bool run, bool want_ast) {
    Frontend fe;
    bool ok = frontend_run(&fe, label, source, want_ast);

    int status = 0;
    if (!ok) {
        status = 1;
    } else if (run) {
        CompiledProgram *prog = codegen_run(&fe.sema, &fe.arena);
        VM vm;
        vm_init(&vm, prog->global_size);
        status = vm_run(&vm, prog->main);
        vm_free(&vm);
        compiled_program_free(prog);
    }

    frontend_free(&fe);
    return status;
}

/* Read `path` and compile it. Returns the process status. */
static int compile_file(const char *path, bool run, bool want_ast) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 1;
    }
    return compile_source(path, source, run, want_ast);
}

/* ------------------------------------------------------------------ */
/* Bundling                                                           */
/* ------------------------------------------------------------------ */

/* Read a whole file into memory; `read_file` does not report the size. */
static unsigned char *read_sized(const char *path, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *len = (size_t)size;
    return buf;
}

/* Write `output` as a standalone executable: a copy of this binary, a bytecode
   image of `entry` (prelude and imports included), and the footer. Nothing
   but `oby` itself is needed at bundle time; the artifact carries the compiler
   so that it needs no toolchain and no files. */
static int bundle_command(const char *argv0, const char *entry, const char *output) {
    char self[4096];
    if (!os_get_executable_path(argv0, self, sizeof self)) {
        fprintf(stderr, "error: cannot find this binary's own path\n");
        return 1;
    }
    size_t self_len = 0;
    unsigned char *self_bytes = read_sized(self, &self_len);
    if (!self_bytes) {
        fprintf(stderr, "error: cannot read '%s'\n", self);
        return 1;
    }

    char *source = read_file(entry);
    if (!source) {
        fprintf(stderr, "error: cannot read '%s'\n", entry);
        free(self_bytes);
        return 1;
    }

    Frontend fe;
    if (!frontend_run(&fe, entry, source, false)) {
        frontend_free(&fe);
        free(self_bytes);
        return 1;
    }

    CompiledProgram *prog = codegen_run(&fe.sema, &fe.arena);
    Function **functions = (Function **)array_data(prog->functions);
    size_t count = array_length(prog->functions);
    size_t entry_index = count;
    for (size_t i = 0; i < count; i++) {
        if (functions[i] == prog->main) {
            entry_index = i;
            break;
        }
    }
    if (entry_index == count) {
        fprintf(stderr, "error: internal: the entry function is missing\n");
        frontend_free(&fe);
        free(self_bytes);
        return 1;
    }

    size_t image_len = 0;
    unsigned char *image = image_write(functions, count, entry_index,
                                       prog->global_size, &image_len);
    frontend_free(&fe);

    if (!image) {
        fprintf(stderr, "error: cannot serialize '%s'\n", entry);
        free(self_bytes);
        return 1;
    }

    FILE *out = fopen(output, "wb");
    bool ok = out != NULL;
    if (ok) {
        ok = fwrite(self_bytes, 1, self_len, out) == self_len &&
             fwrite(image, 1, image_len, out) == image_len &&
             bundle_write_footer(out, image_len, OBY_BUNDLE_TYPE_IMAGE);
        if (fclose(out) != 0) ok = false;
    }
    free(self_bytes);
    free(image);

    if (!ok) {
        fprintf(stderr, "error: cannot write '%s'\n", output);
        remove(output);
        return 1;
    }

#if !defined(_WIN32)
    if (chmod(output, 0755) != 0)
        fprintf(stderr, "warning: cannot make '%s' executable\n", output);
#endif
    printf("%s: %zu bytes (oby %zu, image %zu)\n", output,
           self_len + image_len + OBY_BUNDLE_FOOTER_SIZE, self_len, image_len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bundled binaries                                                   */
/* ------------------------------------------------------------------ */

/* Run the payload of a bundled bytecode image. */
static int run_image_payload(FILE *file, const BundleFooter *footer) {
    unsigned char *payload = bundle_read_payload(file, footer);
    if (!payload) {
        fprintf(stderr, "error: cannot read the bundle payload\n");
        return 1;
    }
    int status = image_run(payload, (size_t)footer->payload_len);
    free(payload);
    if (status < 0) {
        fprintf(stderr,
                "error: cannot load the bundle image; was it built by a different\n"
                "       build of oby?\n");
        return 1;
    }
    return status;
}

/* A bundled binary carries its own script: when this executable has a payload,
   run it and report the status through `*status`. False means there is no
   bundle and the command line applies. */
static bool try_run_bundle(const char *argv0, int *status) {
    char path[4096];
    if (!os_get_executable_path(argv0, path, sizeof path)) return false;

    FILE *file = fopen(path, "rb");
    if (!file) return false;

    BundleFooter footer;
    bool bundled = bundle_read_footer(file, &footer);
    if (bundled) {
        if (footer.payload_type == OBY_BUNDLE_TYPE_IMAGE) {
            *status = run_image_payload(file, &footer);
        } else {
            fprintf(stderr, "error: unknown bundle payload type %u\n",
                    footer.payload_type);
            *status = 1;
        }
    }

    fclose(file);
    return bundled;
}

int main(int argc, char *argv[]) {
    /* The command line as the process saw it, for `args()`: how this binary was
       invoked — the interpreter, or the executable itself when bundled — then
       the script (if any) and its arguments. Nothing here interprets them. */
    os_args_init(argc, argv);

    int bundle_status = 0;
    if (try_run_bundle(argv[0], &bundle_status)) return bundle_status;

    Options opts;
    if (!parse_args(argc, argv, &opts)) {
        return 1;
    }

    if (opts.input == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts.bundle) return bundle_command(argv[0], opts.input, opts.output);

    /* `-check` and `-printast` stop after the frontend; the default mode runs
       the compiled bytecode. */
    bool run = !opts.check_only && !opts.print_ast;
    return compile_file(opts.input, run, opts.print_ast);
}
