#ifndef OBY_ARCHIVE_H
#define OBY_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A source archive: the `payload_type 0` payload. It is what the tests and the
   toolchain use (a bundle that carries its own compiler), never what ships —
   a distributed bundle carries a bytecode image, see `vm/image.h`.
 *
 *   4  file_count
 *   repeated file_count times:
 *     4  path_len
 *     4  data_len
 *        path      (entry-directory-relative, `/` separators)
 *        data
 *
 * The entry file is stored first. Counts and lengths are 32-bit
 * little-endian, like the footer. */

typedef struct {
    const char *path;  /* NUL-terminated */
    const char *data;
    size_t      len;
} ArchiveFile;

/* In a parsed archive, paths are owned by the archive and data points into the
   payload buffer, which must outlive it. */
typedef struct {
    ArchiveFile *files;
    size_t       count;
} Archive;

/* Split an archive payload. False on a malformed archive (a length pointing
   past the payload, or trailing bytes). */
bool archive_parse(const unsigned char *payload, size_t len, Archive *archive);

void archive_free(Archive *archive);

/* Write `count` files as an archive at the current position of `out` and put
   the written byte count into `*len`. False when a count or length does not
   fit 32 bits, or the write fails. */
bool archive_write(FILE *out, const ArchiveFile *files, size_t count, uint64_t *len);

#endif // OBY_ARCHIVE_H
