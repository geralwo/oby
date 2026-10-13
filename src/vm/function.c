#include <stdlib.h>
#include "vm/function.h"

void function_init(Function *f, const char *name) {
    f->name = name;
    f->code = NULL;
    f->code_len = 0;
    f->code_cap = 0;
    f->consts = NULL;
    f->const_count = 0;
    f->const_cap = 0;
    f->param_count = 0;
    f->frame_size = 0;
    f->up_count = 0;
    f->up_off = NULL;
    f->up_size = NULL;
    f->up_total = 0;
}

void function_free(Function *f) {
    free(f->code);
    free(f->consts);
    free(f->up_off);
    free(f->up_size);
    f->code = NULL;
    f->consts = NULL;
    f->up_off = NULL;
    f->up_size = NULL;
    f->code_len = f->code_cap = 0;
    f->const_count = f->const_cap = 0;
    f->up_count = f->up_total = 0;
}

void function_emit(Function *f, Instruction ins) {
    if (f->code_len == f->code_cap) {
        size_t cap = f->code_cap ? f->code_cap * 2 : 16;
        Instruction *p = (Instruction *)realloc(f->code, cap * sizeof(Instruction));
        if (!p) return;
        f->code = p;
        f->code_cap = cap;
    }
    f->code[f->code_len++] = ins;
}

int function_add_const(Function *f, Value v) {
    if (f->const_count == f->const_cap) {
        size_t cap = f->const_cap ? f->const_cap * 2 : 8;
        Value *p = (Value *)realloc(f->consts, cap * sizeof(Value));
        if (!p) return -1;
        f->consts = p;
        f->const_cap = cap;
    }
    f->consts[f->const_count] = v;
    return (int)f->const_count++;
}
