#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include "util.h"

bool digit_in_base(int base, char c) {
    if (base == 16) return isxdigit((unsigned char)c) != 0;
    if (base == 2) return c == '0' || c == '1';
    return isdigit((unsigned char)c) != 0;
}

bool strip_digit_separators(char *text, int base) {
    char *dst = text;
    bool ok = true;
    for (const char *p = text; *p; p++) {
        if (*p == '_') {
            if (base && (p == text || !digit_in_base(base, p[-1]) ||
                         !digit_in_base(base, p[1])))
                ok = false;
            continue;
        }
        *dst++ = *p;
    }
    *dst = '\0';
    return ok;
}

bool parse_int_literal(const char *text, unsigned long long *out) {
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text += 2;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(text, &end, base);
    if (!end || end == text || *end != '\0') return false;
    *out = v;
    return errno != ERANGE;
}

char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read = fread(buffer, 1, size, file);
    buffer[read] = '\0';

    fclose(file);
    return buffer;
}
