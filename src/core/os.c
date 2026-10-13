#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include "core/os.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <unistd.h>
#endif

/* Last resort: the name the program was started with. A bare `oby` resolves
   against the working directory, which is why the platform APIs come first. */
static bool from_argv0(const char *argv0, char *buf, size_t size) {
    if (!argv0 || !*argv0) return false;
    size_t len = strlen(argv0);
    if (len >= size) return false;
    memcpy(buf, argv0, len + 1);
    return true;
}

bool os_get_executable_path(const char *argv0, char *buf, size_t size) {
    if (size == 0) return false;
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (n > 0 && (size_t)n < size) return true;
#elif defined(__APPLE__)
    uint32_t cap = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &cap) == 0 && strlen(buf) < size) return true;
#else
    /* `readlink` truncates silently, so a return of `size - 1` may be one. */
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n > 0 && (size_t)n < size - 1) {
        buf[n] = '\0';
        return true;
    }
#endif
    return from_argv0(argv0, buf, size);
}

/* ------------------------------------------------------------------ */
/* Command line                                                       */
/* ------------------------------------------------------------------ */

static int    g_arg_count = 0;
static char **g_args = NULL;

void os_args_init(int count, char **args) {
    g_arg_count = count;
    g_args = args;
}

int os_args_count(void) {
    return g_arg_count;
}

const char *os_arg(int index) {
    if (!g_args || index < 0 || index >= g_arg_count) return NULL;
    return g_args[index];
}
