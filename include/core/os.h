#ifndef OBY_OS_H
#define OBY_OS_H

#include <stdbool.h>
#include <stddef.h>

/* Fill `buf` with the path of the running executable (`/proc/self/exe`,
   `GetModuleFileNameA`, `_NSGetExecutablePath`). Falls back to `argv0` as
   given — which may be relative to the working directory — when the platform
   cannot tell. Returns false when nothing fits `size`. */
bool os_get_executable_path(const char *argv0, char *buf, size_t size);

/* The command line a program sees, for `io_args`: exactly the process's argv,
   so `args[0]` is how the binary was invoked — the interpreter, or the
   executable when bundled — then the script and its arguments. The strings are
   borrowed and live as long as the process. */
void os_args_init(int count, char **args);

/* How many arguments there are, and one of them (NULL when out of range). */
int os_args_count(void);
const char *os_arg(int index);

#endif // OBY_OS_H
