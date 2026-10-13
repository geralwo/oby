#include <string.h>

#include "lsp/uri.h"

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

bool uri_to_path(const char *uri, char *buf, size_t cap) {
    static const char scheme[] = "file://";
    size_t i = 0;
    for (; scheme[i]; i++) {
        char c = uri[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != scheme[i]) return false;
    }

    size_t o = 0;
    for (const char *p = uri + i; *p; p++) {
        char c = *p;
        if (c == '#' || c == '?') break;   /* a fragment or query names no file */
        if (c == '%' && is_hex(p[1]) && is_hex(p[2])) {
            c = (char)(hex_val(p[1]) * 16 + hex_val(p[2]));
            p += 2;
        }
        if (o + 1 >= cap) return false;
        buf[o++] = c;
    }
    buf[o] = '\0';

#if defined(_WIN32)
    /* `file:///C:/x` decodes to `/C:/x`; drop the extra slash, since the drive
       letter is the path's root. (Separators stay `/`: Windows accepts them.) */
    if (o >= 3 && buf[0] == '/' && buf[2] == ':' &&
        ((buf[1] >= 'A' && buf[1] <= 'Z') || (buf[1] >= 'a' && buf[1] <= 'z'))) {
        memmove(buf, buf + 1, o);
    }
#endif
    return true;
}

bool path_to_uri(const char *path, char *buf, size_t cap) {
    size_t o = 0;
    for (const char *p = "file://"; *p; p++) {
        if (o + 1 >= cap) return false;
        buf[o++] = *p;
    }

#if defined(_WIN32)
    /* `C:\x` -> `file:///C:/x`, with the drive letter uppercased (clients
       differ on the casing; leading with the canonical form is safest). */
    if (path[0] && path[1] == ':') {
        char drive = path[0];
        if (drive >= 'a' && drive <= 'z') drive = (char)(drive - 'a' + 'A');
        if (o + 3 >= cap) return false;
        buf[o++] = '/';
        buf[o++] = drive;
        buf[o++] = ':';
        path += 2;
    } else if (path[0] != '/') {
        if (o + 1 >= cap) return false;
        buf[o++] = '/';
    }
#endif

    static const char digits[] = "0123456789ABCDEF";
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') c = '/';
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~' ||
                    c == '/' || c == ':';
        if (keep) {
            if (o + 1 >= cap) return false;
            buf[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return false;
            buf[o++] = '%';
            buf[o++] = digits[c >> 4];
            buf[o++] = digits[c & 0xF];
        }
    }
    buf[o] = '\0';
    return true;
}
