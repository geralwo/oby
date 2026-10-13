#ifndef OBY_IMAGE_H
#define OBY_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include "vm/function.h"

/* A compiled program image: the `payload_type 1` payload, what `--bundle`
   appends to a copy of `oby`. It is build-coupled — the image records a
   fingerprint of the native table — and the loader refuses anything that does
   not match. See plans/done/standalone_executable_bundling.md for the byte
   layout. */

typedef struct {
    Function  *entry;
    size_t     global_size;
    Function **functions;  /* every function the image holds, for freeing */
    size_t     count;
} Image;

/* Serialize `functions` (`entry_index` selects the entry) into a malloc'd
   buffer; the caller frees. NULL when a value cannot be represented (an
   unexpected constant kind, a function constant that is not in the table) or
   the arguments do not add up. */
unsigned char *image_write(Function *const *functions, size_t count,
                           size_t entry_index, size_t global_size, size_t *len);

/* Load an image into freshly allocated functions. False on bad magic, an
   unknown format version, a native fingerprint mismatch, a length running past
   the buffer, or trailing bytes. */
bool image_load(const unsigned char *data, size_t len, Image *image);

/* Free everything a loaded image owns: the functions, their code and up
   tables, and the strings among their constants. */
void image_free(Image *image);

/* Load `data` and run it to completion. Returns the VM status (0 on success),
   or -1 when the image cannot be loaded — the caller reports that. */
int image_run(const unsigned char *data, size_t len);

#endif // OBY_IMAGE_H
