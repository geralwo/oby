#ifndef OBY_SEMA_H
#define OBY_SEMA_H

#include "compiler/ast.h"
#include "compiler/type.h"
#include "compiler/diag.h"
#include "core/arena.h"

/* A parsed source file plus the info needed to report errors in it. */
typedef struct {
    ASTNode *program;
    const char *file;
    const char *source;
} ProgramUnit;

/* The semantic analysis (typecheck) pass.
 *
 * It resolves `use` imports (pragma-once), registers type and function
 * definitions, then walks every body to resolve paths and annotate each
 * AST node with its `Type`. Errors are reported to stderr and recorded in
 * `had_error`. */
typedef struct {
    Arena *arena;
    TypeContext types;
    Scope *global;
    Scope *current;         /* scope stack top while checking */
    Type *current_ret;      /* return type of the enclosing function */
    bool had_error;
    Diagnostics *diag;
    Array *programs;        /* Array of ProgramUnit* (incl. imports) */
    Array *loaded;          /* Array of char* (resolved import paths) */
    const char *main_dir;   /* directory of the entry file */
    size_t global_size;     /* bytes of global storage assigned */
    int func_depth;         /* >0 while checking a function body */
    int loop_depth;         /* >0 while checking a loop body */
    int cur_func;           /* nesting id of the function body being checked (0 = top level) */
    int func_seq;           /* source of `cur_func` ids */
    Array *func_stack;      /* Array of ASTNode* (function literals being checked, innermost last) */
    int allow_local;        /* >0 while a `local` is permitted even outside a function */
    int in_default;         /* >0 while checking an instance member's initializer */
    Scope *field_enclosing; /* scope around a `{}` literal whose members shadow it */
    Type *cur_owner;        /* type whose member function body is being checked */
    Symbol *cur_receiver;   /* that method's receiver parameter, or NULL */
    int inst_depth;         /* >0 while checking an instantiated generic body */
    bool defer_inst;        /* while registering: instances queue for checking */
    Array *pending_insts;   /* Array of Type* whose body has not been checked */
} Sema;

void sema_init(Sema *s, Arena *arena, Diagnostics *diag);
/* `path` is the entry file path; used to resolve relative `use` imports. */
void sema_check(Sema *s, ASTNode *program, const char *path);

#endif // OBY_SEMA_H
