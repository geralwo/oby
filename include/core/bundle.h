#ifndef OBY_BUNDLE_H
#define OBY_BUNDLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Standalone executables: a payload appended to a runner binary, followed by a
   fixed footer. The OS loader ignores trailing bytes, so a bundled binary is
   still a valid executable and an unbundled one behaves as the plain tool.
   See plans/done/standalone_executable_bundling.md for the payload formats. */

#define OBY_BUNDLE_MAGIC "OBYBUNDLE"  /* 9 bytes, not NUL-terminated in the file */
#define OBY_BUNDLE_FOOTER_SIZE 25

_Static_assert(sizeof(OBY_BUNDLE_MAGIC) - 1 == 9, "bundle magic must be 9 bytes");

enum {
    OBY_BUNDLE_TYPE_SOURCE = 0, /* source archive; toolchain and tests only */
    OBY_BUNDLE_TYPE_IMAGE  = 1, /* bytecode image; what ships */
};

/* The 25 bytes at the end of a bundled binary, in order:
       uint64 payload_len   (payload bytes before the footer)
       uint32 payload_type  (OBY_BUNDLE_TYPE_*)
       uint32 reserved      (0; future flags)
       char   magic[9]      ("OBYBUNDLE")
   little-endian, magic last so it ends the file. */
typedef struct {
    uint64_t payload_len;
    uint32_t payload_type;
    uint32_t reserved;
} BundleFooter;

/* Read the footer that ends `file` (seeks to the end). False when the magic is
   missing, the file is shorter than a footer, or the payload length does not
   fit before the footer. */
bool bundle_read_footer(FILE *file, BundleFooter *footer);

/* True when `path` ends in a bundle footer. */
bool bundle_has_footer(const char *path);

/* Append the footer for a payload of `payload_len` bytes that `file` is
   already positioned after (at the end of the file). */
bool bundle_write_footer(FILE *file, uint64_t payload_len, uint32_t payload_type);

/* Read the payload that `footer` describes (malloc'd, caller frees, NULL on
   failure). */
unsigned char *bundle_read_payload(FILE *file, const BundleFooter *footer);

/* The runner's bytes, baked into the toolchain at build time (`tools/blob_gen.c`
   over the linked `obyrun`). `--bundle` writes them before the payload, so a
   standalone executable can be produced without a second file on hand. */
const unsigned char *runner_bytes(size_t *len);

#endif // OBY_BUNDLE_H
