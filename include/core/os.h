#ifndef OBY_OS_H
#define OBY_OS_H

#include <stdbool.h>
#include <stddef.h>

/* Fill `buf` with the path of the running executable (`/proc/self/exe`,
   `GetModuleFileNameA`, `_NSGetExecutablePath`). Falls back to `argv0` as
   given — which may be relative to the working directory — when the platform
   cannot tell. Returns false when nothing fits `size`. */
bool os_get_executable_path(const char *argv0, char *buf, size_t size);

#endif // OBY_OS_H
