#ifndef OBY_LSP_URI_H
#define OBY_LSP_URI_H

#include <stdbool.h>
#include <stddef.h>

/* A `file://` URI to a filesystem path: `%XX` escapes decoded, a query or
   fragment refused. False for another scheme or a path that does not fit
   `cap`. */
bool uri_to_path(const char *uri, char *buf, size_t cap);

/* A filesystem path to a `file://` URI: forward slashes and `%XX` for the
   bytes that need it. False when it does not fit `cap`. */
bool path_to_uri(const char *path, char *buf, size_t cap);

#endif // OBY_LSP_URI_H
