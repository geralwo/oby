#ifndef OBY_FUNCTION_H
#define OBY_FUNCTION_H

#include <stddef.h>
#include "core/value.h"
#include "vm/opcode.h"

/* A compiled function: an instruction stream plus a constant pool.
   `Function` itself is typedef'd in `core/value.h` (forward declaration). */
struct Function {
    const char  *name;       /* for debugging */
    Instruction *code;
    size_t       code_len;
    size_t       code_cap;
    Value       *consts;
    size_t       const_count;
    size_t       const_cap;
    int          param_count;
    size_t       frame_size; /* bytes of stack the frame needs */
    /* Captured state, when this function is a closure body. */
    size_t       up_count;   /* number of captured values */
    size_t      *up_off;     /* byte offset of each captured value */
    size_t      *up_size;    /* byte size of each captured value */
    size_t       up_total;   /* total bytes: the size of a closure's `up` */
};

void function_init(Function *f, const char *name);
void function_free(Function *f);
void function_emit(Function *f, Instruction ins);
int  function_add_const(Function *f, Value v);

#endif // OBY_FUNCTION_H
