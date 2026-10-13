#include <string.h>
#include "core/embed.h"

const EmbeddedFile *embed_lookup(const char *path) {
    const EmbeddedFile *files = NULL;
    size_t count = embed_static_files(&files);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(files[i].path, path) == 0) return &files[i];
    }
    return NULL;
}
