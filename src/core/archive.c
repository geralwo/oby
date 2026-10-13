#include <stdlib.h>
#include <string.h>
#include "core/archive.h"
#include "core/bytes.h"

static void free_files(ArchiveFile *files, size_t count) {
    for (size_t i = 0; i < count; i++) free((char *)files[i].path);
    free(files);
}

bool archive_parse(const unsigned char *payload, size_t len, Archive *archive) {
    archive->files = NULL;
    archive->count = 0;
    if (len < 4) return false;

    uint32_t count = get_u32(payload);
    /* Each entry carries two 32-bit lengths, so this also bounds the loop. */
    if (count > (len - 4) / 8) return false;

    ArchiveFile *files = (ArchiveFile *)calloc(count ? count : 1, sizeof *files);
    if (!files) return false;

    size_t off = 4; /* always <= len */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t path_len = get_u32(payload + off);
        uint32_t data_len = get_u32(payload + off + 4);
        off += 8;

        if ((size_t)path_len > len - off) {
            free_files(files, i);
            return false;
        }
        char *path = (char *)malloc((size_t)path_len + 1);
        if (!path) {
            free_files(files, i);
            return false;
        }
        memcpy(path, payload + off, path_len);
        path[path_len] = '\0';
        off += path_len;

        if ((size_t)data_len > len - off) {
            free(path);
            free_files(files, i);
            return false;
        }
        files[i].path = path;
        files[i].data = (const char *)(payload + off);
        files[i].len = data_len;
        off += data_len;
    }

    /* The archive is exactly the payload: trailing bytes mean the lengths and
       the footer disagree. */
    if (off != len) {
        free_files(files, count);
        return false;
    }

    archive->files = files;
    archive->count = count;
    return true;
}

void archive_free(Archive *archive) {
    free_files(archive->files, archive->count);
    archive->files = NULL;
    archive->count = 0;
}

bool archive_write(FILE *out, const ArchiveFile *files, size_t count, uint64_t *len) {
    if (count > 0xffffffffu) return false;

    unsigned char head[8];
    put_u32(head, (uint32_t)count);
    if (fwrite(head, 1, 4, out) != 4) return false;

    uint64_t written = 4;
    for (size_t i = 0; i < count; i++) {
        size_t path_len = strlen(files[i].path);
        if (path_len > 0xffffffffu || files[i].len > 0xffffffffu) return false;
        put_u32(head, (uint32_t)path_len);
        put_u32(head + 4, (uint32_t)files[i].len);
        if (fwrite(head, 1, 8, out) != 8) return false;
        if (fwrite(files[i].path, 1, path_len, out) != path_len) return false;
        if (fwrite(files[i].data, 1, files[i].len, out) != files[i].len) return false;
        written += 8 + (uint64_t)path_len + files[i].len;
    }

    if (len) *len = written;
    return true;
}
