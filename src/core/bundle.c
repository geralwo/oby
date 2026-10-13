#include <stdlib.h>
#include <string.h>
#include "core/bundle.h"
#include "core/bytes.h"

/* Footer field offsets; the format is little-endian, so it is assembled and
   taken apart byte by byte rather than by copying a struct. */
enum {
    OFF_LEN      = 0,  /* uint64 */
    OFF_TYPE     = 8,  /* uint32 */
    OFF_RESERVED = 12, /* uint32 */
    OFF_MAGIC    = 16, /* 9 bytes */
};

bool bundle_read_footer(FILE *file, BundleFooter *footer) {
    if (fseek(file, 0, SEEK_END) != 0) return false;
    long size = ftell(file);
    if (size < OBY_BUNDLE_FOOTER_SIZE) return false;
    if (fseek(file, size - OBY_BUNDLE_FOOTER_SIZE, SEEK_SET) != 0) return false;

    unsigned char buf[OBY_BUNDLE_FOOTER_SIZE];
    if (fread(buf, 1, sizeof buf, file) != sizeof buf) return false;
    if (memcmp(buf + OFF_MAGIC, OBY_BUNDLE_MAGIC, sizeof OBY_BUNDLE_MAGIC - 1) != 0)
        return false;

    uint64_t payload_len = get_u64(buf + OFF_LEN);
    if (payload_len > (uint64_t)(size - OBY_BUNDLE_FOOTER_SIZE)) return false;

    if (footer) {
        footer->payload_len = payload_len;
        footer->payload_type = get_u32(buf + OFF_TYPE);
        footer->reserved = get_u32(buf + OFF_RESERVED);
    }
    return true;
}

bool bundle_has_footer(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool has = bundle_read_footer(file, NULL);
    fclose(file);
    return has;
}

bool bundle_write_footer(FILE *file, uint64_t payload_len, uint32_t payload_type) {
    unsigned char buf[OBY_BUNDLE_FOOTER_SIZE];
    memset(buf, 0, sizeof buf);
    put_u64(buf + OFF_LEN, payload_len);
    put_u32(buf + OFF_TYPE, payload_type);
    put_u32(buf + OFF_RESERVED, 0);
    memcpy(buf + OFF_MAGIC, OBY_BUNDLE_MAGIC, sizeof OBY_BUNDLE_MAGIC - 1);

    if (fwrite(buf, 1, sizeof buf, file) != sizeof buf) return false;
    return fflush(file) == 0;
}

unsigned char *bundle_read_payload(FILE *file, const BundleFooter *footer) {
    if (fseek(file, 0, SEEK_END) != 0) return NULL;
    long size = ftell(file);
    if (size < 0 || (uint64_t)size < footer->payload_len) return NULL;
    uint64_t offset = (uint64_t)size - footer->payload_len;
    if (offset < OBY_BUNDLE_FOOTER_SIZE) return NULL;
    offset -= OBY_BUNDLE_FOOTER_SIZE;

    if (fseek(file, (long)offset, SEEK_SET) != 0) return NULL;
    unsigned char *payload = (unsigned char *)malloc((size_t)footer->payload_len + 1);
    if (!payload) return NULL;
    if (fread(payload, 1, (size_t)footer->payload_len, file) !=
        (size_t)footer->payload_len) {
        free(payload);
        return NULL;
    }
    payload[footer->payload_len] = '\0';
    return payload;
}
