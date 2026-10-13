#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "core/embed.h"

/* Files registered at runtime (a bundle's payload). The list grows on demand
   and lives for the process's lifetime. */
static EmbeddedFile *added_files = NULL;
static size_t added_count = 0;
static size_t added_capacity = 0;

bool embed_add(const char *path, const char *data, size_t len) {
    if (added_count == added_capacity) {
        size_t capacity = added_capacity ? added_capacity * 2 : 8;
        EmbeddedFile *grown =
            (EmbeddedFile *)realloc(added_files, capacity * sizeof *grown);
        if (!grown) return false;
        added_files = grown;
        added_capacity = capacity;
    }
    added_files[added_count].path = path;
    added_files[added_count].data = data;
    added_files[added_count].len = len;
    added_count++;
    return true;
}

const EmbeddedFile *embed_lookup(const char *path) {
    /* Runtime registrations win over the build-time table. */
    for (size_t i = 0; i < added_count; i++) {
        if (strcmp(added_files[i].path, path) == 0) return &added_files[i];
    }

    const EmbeddedFile *files = NULL;
    size_t count = embed_static_files(&files);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(files[i].path, path) == 0) return &files[i];
    }
    return NULL;
}
