#ifndef OBY_EMBED_H
#define OBY_EMBED_H

#include <stddef.h>

/* A source file baked into the binary at build time. `data` holds `len` bytes
   of the file's exact contents; it is not NUL-terminated. */
typedef struct {
    const char *path; /* normalized, as the loader asks for it */
    const char *data;
    size_t      len;
} EmbeddedFile;

/* The build-time table: `compile.sh` runs `tools/embed_gen.c` over the
   prelude sources into `build/prelude_embed.c`, which defines this. Puts the
   prelude's file list into `*files` and returns its length. */
size_t embed_static_files(const EmbeddedFile **files);

/* Look up a source file by normalized path. Embedded files shadow the disk,
   so the compiler behaves the same from any working directory. NULL when the
   file is not embedded; the caller then reads it from disk. */
const EmbeddedFile *embed_lookup(const char *path);

#endif // OBY_EMBED_H
