/*
 * The entry file may be one of the prelude's own files: the language server
 * checks `src/prelude/File.oby` while it is open in the editor, under an
 * absolute path. The embedded copy must not be loaded alongside it, or every
 * definition in it reports "cannot redefine".
 *
 * Built and run by `./compile.sh test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/frontend.h"
#include "core/embed.h"

static int failures = 0;

/* Check the embedded copy of `key` as though it were the file open in the
   editor, labelled `label`. A file with no `use`s of its own works under any
   spelling; the prelude's entry point needs a real one, because its own imports
   resolve relative to it. */
static void check_as_entry(const char *key, const char *label) {
    const EmbeddedFile *file = embed_lookup(key);
    if (!file) {
        printf("  FAIL %s is not embedded\n", key);
        failures++;
        return;
    }

    char *source = (char *)malloc(file->len + 1);
    if (!source) exit(1);
    memcpy(source, file->data, file->len);
    source[file->len] = '\0';

    Frontend fe;
    frontend_init(&fe);
    bool ok = frontend_run(&fe, label, source);
    if (!ok) {
        printf("  FAIL %s reports %d error(s) as the entry\n", label, fe.diag.error_count);
        failures++;
    } else {
        printf("  ok   %s checks as its own entry\n", label);
    }
    frontend_free(&fe);
}

int main(void) {
    /* A file with no `use`s of its own (the rest of the prelude comes from the
       injected entry point), under a relative spelling and a foreign absolute
       one — a client elsewhere labels the file with its own path. */
    check_as_entry("src/prelude/File.oby", "src/prelude/File.oby");
    check_as_entry("src/prelude/File.oby", "/home/someone/oby/src/prelude/File.oby");
    /* The entry point itself: its `use`s load everything else. Its directory
       exists here; the foreign spelling does not, and its imports then come
       from the prelude's own (embedded) directory. */
    check_as_entry("src/prelude/core.oby", "src/prelude/core.oby");
    check_as_entry("src/prelude/core.oby", "/C:/Users/someone/oby/src/prelude/core.oby");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("a prelude file stands in for its embedded copy\n");
    return 0;
}
