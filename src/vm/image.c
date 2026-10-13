#include <stdlib.h>
#include <string.h>
#include "core/array.h"
#include "core/bytes.h"
#include "native/native.h"
#include "vm/image.h"
#include "vm/vm.h"

/*
 * Program images: see include/vm/image.h and
 * plans/done/standalone_executable_bundling.md. Writer and reader share this file so
 * the layout cannot drift.
 *
 * Header:
 *    8  magic "OBYIMAGE"
 *    4  format_version
 *    8  native_hash
 *    8  global_size
 *    4  function_count
 *    4  entry_index
 * Per function:
 *    4  name_len, name bytes
 *    4  code_len, code_len * 8 instruction bytes
 *    4  const_count, then per constant one tag byte and its payload
 *    4  param_count
 *    4  frame_size
 *    4  up_count
 *    4  up_total
 *    4*up_count  up_off
 *    4*up_count  up_size
 *
 * All little-endian. The loader is two-pass over the constants because a
 * function constant can name a function that comes later in the file.
 */

#define IMAGE_MAGIC "OBYIMAGE"
#define IMAGE_VERSION 1u
#define IMAGE_HEADER_SIZE 36

_Static_assert(sizeof(IMAGE_MAGIC) - 1 == 8, "image magic must be 8 bytes");

enum {
    CONST_INT    = 0,
    CONST_FLOAT  = 1,
    CONST_STRING = 2,
    CONST_FUNC   = 3,
};

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

static bool const_size(const Value *v, size_t *size) {
    switch (v->tag) {
    case VAL_INT:
    case VAL_FLOAT:
        *size = 1 + 8;
        return true;
    case VAL_STRING: {
        size_t len = v->as.str ? v->as.str->length : 0;
        if (len > 0xffffffffu) return false;
        *size = 1 + 4 + len;
        return true;
    }
    case VAL_FUNC:
        *size = 1 + 4;
        return true;
    default:
        return false; /* a constant kind the format cannot carry */
    }
}

static bool function_size(const Function *fn, size_t *size) {
    size_t name_len = fn->name ? strlen(fn->name) : 0;
    if (name_len > 0xffffffffu || fn->code_len > 0xffffffffu ||
        fn->const_count > 0xffffffffu || fn->up_count > 0xffffffffu ||
        fn->frame_size > 0xffffffffu || fn->up_total > 0xffffffffu)
        return false;

    size_t total = 4 + name_len + 4 + fn->code_len * 8 + 4;
    for (size_t i = 0; i < fn->const_count; i++) {
        size_t one = 0;
        if (!const_size(&fn->consts[i], &one)) return false;
        total += one;
    }
    total += 4 * 4 + fn->up_count * 4 * 2;
    *size = total;
    return true;
}

static bool function_index(Function *const *functions, size_t count,
                           const Function *target, uint32_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (functions[i] == target) {
            *out = (uint32_t)i;
            return true;
        }
    }
    return false;
}

unsigned char *image_write(Function *const *functions, size_t count,
                           size_t entry_index, size_t global_size, size_t *len) {
    if (!functions || count == 0 || count > 0xffffffffu || entry_index >= count)
        return NULL;

    size_t total = IMAGE_HEADER_SIZE;
    size_t *sizes = (size_t *)malloc(count * sizeof *sizes);
    if (!sizes) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (!function_size(functions[i], &sizes[i])) {
            free(sizes);
            return NULL;
        }
        total += sizes[i];
    }

    unsigned char *buf = (unsigned char *)malloc(total);
    if (!buf) {
        free(sizes);
        return NULL;
    }

    size_t off = 0;
    memcpy(buf, IMAGE_MAGIC, 8);
    off = 8;
    put_u32(buf + off, IMAGE_VERSION);
    off += 4;
    put_u64(buf + off, native_fingerprint());
    off += 8;
    put_u64(buf + off, (uint64_t)global_size);
    off += 8;
    put_u32(buf + off, (uint32_t)count);
    off += 4;
    put_u32(buf + off, (uint32_t)entry_index);
    off += 4;

    for (size_t i = 0; i < count; i++) {
        const Function *fn = functions[i];

        size_t name_len = fn->name ? strlen(fn->name) : 0;
        put_u32(buf + off, (uint32_t)name_len);
        off += 4;
        if (name_len) {
            memcpy(buf + off, fn->name, name_len);
            off += name_len;
        }

        put_u32(buf + off, (uint32_t)fn->code_len);
        off += 4;
        for (size_t k = 0; k < fn->code_len; k++) {
            put_u64(buf + off, fn->code[k]);
            off += 8;
        }

        put_u32(buf + off, (uint32_t)fn->const_count);
        off += 4;
        for (size_t k = 0; k < fn->const_count; k++) {
            const Value *v = &fn->consts[k];
            switch (v->tag) {
            case VAL_INT:
                buf[off] = CONST_INT;
                put_u64(buf + off + 1, (uint64_t)v->as.i);
                off += 9;
                break;
            case VAL_FLOAT:
                buf[off] = CONST_FLOAT;
                put_f64(buf + off + 1, v->as.f);
                off += 9;
                break;
            case VAL_STRING: {
                size_t str_len = v->as.str ? v->as.str->length : 0;
                buf[off] = CONST_STRING;
                put_u32(buf + off + 1, (uint32_t)str_len);
                off += 5;
                if (str_len) {
                    memcpy(buf + off, v->as.str->data, str_len);
                    off += str_len;
                }
                break;
            }
            case VAL_FUNC: {
                uint32_t index = 0;
                if (!function_index(functions, count, v->as.fn, &index)) {
                    free(buf);
                    free(sizes);
                    return NULL;
                }
                buf[off] = CONST_FUNC;
                put_u32(buf + off + 1, index);
                off += 5;
                break;
            }
            default:
                free(buf);
                free(sizes);
                return NULL;
            }
        }

        put_u32(buf + off, (uint32_t)fn->param_count);
        off += 4;
        put_u32(buf + off, (uint32_t)fn->frame_size);
        off += 4;
        put_u32(buf + off, (uint32_t)fn->up_count);
        off += 4;
        put_u32(buf + off, (uint32_t)fn->up_total);
        off += 4;
        for (size_t k = 0; k < fn->up_count; k++) {
            put_u32(buf + off, (uint32_t)fn->up_off[k]);
            off += 4;
        }
        for (size_t k = 0; k < fn->up_count; k++) {
            put_u32(buf + off, (uint32_t)fn->up_size[k]);
            off += 4;
        }
    }

    free(sizes);
    if (off != total) { /* the sizing and emit passes disagree: a bug */
        free(buf);
        return NULL;
    }
    if (len) *len = off;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Loading                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t off; /* always <= len */
} Reader;

static bool rd(Reader *r, size_t n, const unsigned char **out) {
    if (r->len - r->off < n) return false;
    *out = r->data + r->off;
    r->off += n;
    return true;
}

static bool rd_u32(Reader *r, uint32_t *out) {
    const unsigned char *p;
    if (!rd(r, 4, &p)) return false;
    *out = get_u32(p);
    return true;
}

static bool rd_u64(Reader *r, uint64_t *out) {
    const unsigned char *p;
    if (!rd(r, 8, &p)) return false;
    *out = get_u64(p);
    return true;
}

/* A function constant, to patch once every function exists. */
typedef struct {
    uint32_t function;
    uint32_t const_index;
    uint32_t target;
} FuncFixup;

/* Free a function the loader allocated: its constant strings, its name copy,
   its code and up tables, and the struct itself. */
static void free_function(Function *fn) {
    for (size_t i = 0; i < fn->const_count; i++) {
        if (fn->consts[i].tag == VAL_STRING) string_free(fn->consts[i].as.str);
    }
    free((char *)fn->name);
    function_free(fn);
    free(fn);
}

static bool load_function(Reader *r, Function *fn, uint32_t function_index,
                          uint32_t count, Array *fixups) {
    uint32_t name_len = 0;
    const unsigned char *name = NULL;
    if (!rd_u32(r, &name_len) || !rd(r, name_len, &name)) return false;
    char *copy = (char *)malloc((size_t)name_len + 1);
    if (!copy) return false;
    memcpy(copy, name, name_len);
    copy[name_len] = '\0';
    fn->name = copy;

    uint32_t code_len = 0;
    if (!rd_u32(r, &code_len)) return false;
    if (code_len) {
        const unsigned char *bytes = NULL;
        if (!rd(r, (size_t)code_len * 8, &bytes)) return false;
        fn->code = (Instruction *)malloc((size_t)code_len * sizeof(Instruction));
        if (!fn->code) return false;
        for (uint32_t k = 0; k < code_len; k++)
            fn->code[k] = get_u64(bytes + (size_t)k * 8);
        fn->code_len = fn->code_cap = code_len;
    }

    uint32_t const_count = 0;
    if (!rd_u32(r, &const_count)) return false;
    for (uint32_t k = 0; k < const_count; k++) {
        const unsigned char *tag_bytes = NULL;
        if (!rd(r, 1, &tag_bytes)) return false;
        const unsigned char *p = NULL;
        switch (tag_bytes[0]) {
        case CONST_INT: {
            if (!rd(r, 8, &p)) return false;
            if (function_add_const(fn, val_int((int64_t)get_u64(p))) < 0) return false;
            break;
        }
        case CONST_FLOAT: {
            if (!rd(r, 8, &p)) return false;
            if (function_add_const(fn, val_float(get_f64(p))) < 0) return false;
            break;
        }
        case CONST_STRING: {
            uint32_t str_len = 0;
            if (!rd_u32(r, &str_len) || !rd(r, str_len, &p)) return false;
            String *s = string_new_len((const char *)p, str_len);
            if (!s) return false;
            if (function_add_const(fn, val_string(s)) < 0) {
                string_free(s);
                return false;
            }
            break;
        }
        case CONST_FUNC: {
            uint32_t target = 0;
            if (!rd_u32(r, &target) || target >= count) return false;
            int index = function_add_const(fn, val_null());
            if (index < 0) return false;
            FuncFixup fix = { function_index, (uint32_t)index, target };
            array_push(fixups, &fix);
            break;
        }
        default:
            return false;
        }
    }

    uint32_t param_count = 0, frame_size = 0, up_count = 0, up_total = 0;
    if (!rd_u32(r, &param_count) || !rd_u32(r, &frame_size) ||
        !rd_u32(r, &up_count) || !rd_u32(r, &up_total))
        return false;
    fn->param_count = (int)param_count;
    fn->frame_size = frame_size;
    fn->up_count = up_count;
    fn->up_total = up_total;

    if (up_count) {
        fn->up_off = (size_t *)malloc((size_t)up_count * sizeof(size_t));
        fn->up_size = (size_t *)malloc((size_t)up_count * sizeof(size_t));
        if (!fn->up_off || !fn->up_size) return false;
        for (uint32_t k = 0; k < up_count; k++) {
            uint32_t v = 0;
            if (!rd_u32(r, &v)) return false;
            fn->up_off[k] = v;
        }
        for (uint32_t k = 0; k < up_count; k++) {
            uint32_t v = 0;
            if (!rd_u32(r, &v)) return false;
            fn->up_size[k] = v;
        }
    }
    return true;
}

bool image_load(const unsigned char *data, size_t len, Image *image) {
    memset(image, 0, sizeof *image);

    Reader r = { data, len, 0 };
    const unsigned char *magic = NULL;
    uint32_t version = 0, count = 0, entry_index = 0;
    uint64_t native_hash = 0, global_size = 0;
    if (!rd(&r, 8, &magic) || memcmp(magic, IMAGE_MAGIC, 8) != 0) return false;
    if (!rd_u32(&r, &version) || version != IMAGE_VERSION) return false;
    if (!rd_u64(&r, &native_hash) || native_hash != native_fingerprint()) return false;
    if (!rd_u64(&r, &global_size)) return false;
    if (!rd_u32(&r, &count) || !rd_u32(&r, &entry_index)) return false;
    if (count == 0 || entry_index >= count) return false;

    Function **functions = (Function **)calloc(count, sizeof *functions);
    if (!functions) return false;
    Array *fixups = array_new(sizeof(FuncFixup));
    if (!fixups) {
        free(functions);
        return false;
    }

    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++) {
        functions[i] = (Function *)malloc(sizeof(Function));
        if (!functions[i]) {
            ok = false;
            break;
        }
        function_init(functions[i], NULL);
        ok = load_function(&r, functions[i], i, count, fixups);
    }
    if (ok && r.off != len) ok = false; /* trailing bytes */

    if (ok) {
        for (size_t i = 0; i < array_length(fixups); i++) {
            FuncFixup *fix = (FuncFixup *)array_get(fixups, i);
            functions[fix->function]->consts[fix->const_index] =
                val_func(functions[fix->target]);
        }
    }
    array_destroy(fixups);

    if (!ok) {
        for (uint32_t i = 0; i < count; i++) {
            if (!functions[i]) break;
            free_function(functions[i]);
        }
        free(functions);
        return false;
    }

    image->entry = functions[entry_index];
    image->global_size = (size_t)global_size;
    image->functions = functions;
    image->count = count;
    return true;
}

void image_free(Image *image) {
    for (size_t i = 0; i < image->count; i++) free_function(image->functions[i]);
    free(image->functions);
    image->entry = NULL;
    image->functions = NULL;
    image->count = 0;
    image->global_size = 0;
}

int image_run(const unsigned char *data, size_t len) {
    Image image;
    if (!image_load(data, len, &image)) return -1;

    VM vm;
    vm_init(&vm, image.global_size);
    int status = vm_run(&vm, image.entry);
    vm_free(&vm);
    image_free(&image);
    return status;
}
