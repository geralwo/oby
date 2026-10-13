#ifndef OBY_CODEGEN_H
#define OBY_CODEGEN_H

#include "compiler/sema.h"
#include "vm/function.h"
#include "core/array.h"

/* A compiled program: the top-level function plus every function it defines. */
typedef struct {
    Function *main;
    size_t    global_size; /* bytes of global storage */
    Array    *functions;   /* Array of Function* */
} CompiledProgram;

/* Compile the checked program in `sema` into bytecode. */
CompiledProgram *codegen_run(Sema *sema, Arena *arena);
void compiled_program_free(CompiledProgram *p);

#endif // OBY_CODEGEN_H
