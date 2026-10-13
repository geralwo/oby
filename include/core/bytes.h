#ifndef OBY_BYTES_H
#define OBY_BYTES_H

#include <stdint.h>
#include <string.h>

/* Little-endian field packing, shared by the file formats (footer, archive,
   image). Fields are assembled and taken apart byte by byte, so the layout
   does not depend on the machine. */

static inline void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static inline void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static inline uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t get_u64(const unsigned char *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* Doubles travel as their IEEE-754 bits, memcpy'd rather than reinterpreted. */
static inline void put_f64(unsigned char *p, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    put_u64(p, bits);
}

static inline double get_f64(const unsigned char *p) {
    uint64_t bits = get_u64(p);
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

#endif // OBY_BYTES_H
