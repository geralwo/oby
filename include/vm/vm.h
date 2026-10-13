#ifndef OBY_VM_H
#define OBY_VM_H

#include <stddef.h>
#include "core/value.h"
#include "vm/function.h"

#define VM_FRAMES_MAX 256
#define VM_STACK_INIT 4096   /* bytes */

/* The execution state of one executing function. Registers live at byte
   offsets from `base` into the VM's byte-addressed stack. */
typedef struct {
    Function *fn;
    Closure  *closure;   /* captured state, when executing a closure; else NULL */
    uint32_t  ip;
    size_t    base;      /* byte offset of R[0] */
    size_t    ret_base;  /* caller's byte offset for results */
} Frame;

typedef struct {
    uint8_t *globals;     /* byte-addressed */
    size_t   global_size; /* bytes */
    uint8_t *stack;       /* byte-addressed */
    size_t   stack_cap;   /* bytes */
    Frame    frames[VM_FRAMES_MAX];
    int      frame_count;
} VM;

void vm_init(VM *vm, size_t global_size);
void vm_free(VM *vm);

/* Execute the top-level program (a compiled `Function`). Returns 0 on success,
   non-zero on a VM error. */
int vm_run(VM *vm, Function *program);

#endif // OBY_VM_H
