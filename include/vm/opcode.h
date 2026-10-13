#ifndef OBY_OPCODE_H
#define OBY_OPCODE_H

#include <stdint.h>

/*
 * A 64-bit fixed-width instruction. Byte offsets need 16 bits, so operands are
 * packed as
 *   op:8 | A:16 | B:16 | C:16 | N:8
 * where N is a size class / auxiliary field. The `ABx` form puts a 32-bit
 * operand in B+C.
 *
 * Packed values (byte, int, float, ...) live on the stack as raw bytes; tagged
 * values (String, Array, {}, function, ...) are 16-byte `Value`s. Instructions
 * that touch packed data carry a `ValueKind` in N (or C, for OP_CONV).
 */
typedef uint64_t Instruction;

/* How a stack value is stored. */
typedef enum {
    KIND_TAGGED  = 0,   /* a 16-byte Value */
    KIND_BYTE    = 1,   /* 1 byte  */
    KIND_INT32   = 2,   /* 4 bytes, signed integer */
    KIND_FLOAT32 = 3,   /* 4 bytes, IEEE-754 */
    KIND_INT64   = 4,   /* 8 bytes, signed integer */
    KIND_FLOAT64 = 5,   /* 8 bytes, IEEE-754 */
    KIND_UINT32  = 6,   /* 4 bytes, unsigned integer */
    KIND_UINT64  = 7,   /* 8 bytes, unsigned integer */
} ValueKind;

typedef enum {
    OP_HALT = 0,
    OP_LOADK,       /* A Bx N: R[A] = K[Bx] as kind N */
    OP_LOADNIL,     /* A    : R[A] = null */
    OP_ZERO,        /* A B  : zero B bytes at R[A] */
    OP_MOVE,        /* A B  : R[A] = R[B] (one tagged value) */
    OP_MOVEN,       /* A B N: copy N bytes from R[B] to R[A] */
    OP_GETGLOBAL,   /* A Bx N: copy N bytes from G[Bx] to R[A] */
    OP_SETGLOBAL,   /* A Bx N: copy N bytes from R[A] to G[Bx] */
    OP_ADD_INT,     /* A B C N: R[A] = R[B] + R[C], kind N */
    OP_SUB_INT,     /* A B C N */
    OP_MUL_INT,     /* A B C N */
    OP_DIV_INT,     /* A B C N: integer division; zero divisor is a runtime error */
    OP_MOD_INT,     /* A B C N: integer remainder; zero divisor is a runtime error */
    OP_ADD_FLOAT,   /* A B C N */
    OP_SUB_FLOAT,   /* A B C N */
    OP_MUL_FLOAT,   /* A B C N */
    OP_DIV_FLOAT,   /* A B C N */
    OP_NEG_INT,     /* A B N: R[A] = -R[B] */
    OP_NEG_FLOAT,   /* A B N */
    OP_BAND,        /* A B C N: R[A] = R[B] & R[C], kind N */
    OP_BOR,         /* A B C N */
    OP_BXOR,        /* A B C N */
    OP_SHL,         /* A B C N: R[A] = R[B] << R[C]; C is a 64-bit count */
    OP_SHRU,        /* A B C N: logical (unsigned) right shift */
    OP_SAR,         /* A B C N: arithmetic (signed) right shift */
    OP_BNOT,        /* A B N: R[A] = ~R[B] */
    OP_EQ,          /* A B C N: R[A] = (R[B] == R[C]) as int32, kind N */
    OP_NE,          /* A B C N */
    OP_LT,          /* A B C N */
    OP_GT,          /* A B C N */
    OP_LE,          /* A B C N */
    OP_GE,          /* A B C N */
    OP_EQPACKED,    /* A B C N: R[A] = (R[B] == R[C]) as int32, N bytes */
    OP_NEPACKED,    /* A B C N */
    OP_NOT,         /* A B N: R[A] = !R[B] as int32 */
    OP_JMP,         /* Bx   : pc += sBx */
    OP_TEST,        /* A N  : if R[A] is truthy, skip the next instruction */
    OP_GETFIELD,    /* A B C N: copy N bytes from R[B].field[C] to R[A] */
    OP_SETFIELD,    /* A B C N: copy N bytes from R[C] to R[A].field[B] */
    OP_NEWOBJ,      /* A Bx : R[A] = new object with Bx fields */
    OP_NEWARRAY,    /* A B C: R[A] = new array, element size B, capacity C */
    OP_APPEND,      /* A B N: append N bytes from R[B] to the array at R[A] */
    OP_GETINDEX,    /* A B C: R[A] = arr(R[B])[R[C]]; C is an int32 index */
    OP_SETINDEX,    /* A B C: arr(R[A])[R[B]] = R[C]; B is an int32 index */
    OP_ARRAY_POP,    /* A B  : R[B] = pop(arr(R[A])); heap arrays only */
    OP_ARRAY_INSERT, /* A B C: insert R[C] into arr(R[A]) at int32 index R[B] */
    OP_ARRAY_REMOVE, /* A B C: R[C] = remove(arr(R[A]), int32 index R[B]) */
    OP_ARRAY_FIXED,  /* A: mark arr(R[A]) as fixed (cannot be resized) */
    OP_ARRAY_REPEAT, /* A B C: append R[C] to arr(R[A]), int32 R[B] times */
    OP_CALL,        /* A B C: call R[A] with B args at R[A+16..]; C reserved */
    OP_CALLNATIVE,  /* A B C: R[A] = native[B](R[A..A+C-1]); args are 16-byte slots */
    OP_RETURN,      /* A B  : return R[A..A+B-1] */
    OP_BOX,         /* A B N: R[A] = box(kind N, R[B]) */
    OP_UNBOX,       /* A B N: R[A] = unbox(kind N, R[B]) */
    OP_CONV,        /* A B C N: R[A] = convert R[B] from kind C to kind N */
    OP_GETUPVAL,    /* A B C: copy C bytes from the closure's upvalue B to R[A] */
    OP_CLOSURE,     /* A B C: R[A] = closure(consts[B].fn), captures from R[C..] */
    OP_COUNT
} OpCode;

#define OP_GET(i)  ((OpCode)((i) & 0xFFu))
#define OP_A(i)    (((i) >> 8)  & 0xFFFFu)
#define OP_B(i)    (((i) >> 24) & 0xFFFFu)
#define OP_C(i)    (((i) >> 40) & 0xFFFFu)
#define OP_N(i)    (((i) >> 56) & 0xFFu)
#define OP_BX(i)   (((i) >> 24) & 0xFFFFFFFFu)
#define OP_SBX(i)  ((int32_t)((i) >> 24))   /* sign-extended 32-bit */

#define INSTR_ABC(op, a, b, c) \
    ((Instruction)(op) | \
     ((Instruction)((uint32_t)(a) & 0xFFFFu) << 8) | \
     ((Instruction)((uint32_t)(b) & 0xFFFFu) << 24) | \
     ((Instruction)((uint32_t)(c) & 0xFFFFu) << 40))
#define INSTR_ABX(op, a, bx) \
    ((Instruction)(op) | \
     ((Instruction)((uint32_t)(a) & 0xFFFFu) << 8) | \
     ((Instruction)((uint32_t)(bx) & 0xFFFFFFFFu) << 24))
#define INSTR_ABCN(op, a, b, c, n) \
    (INSTR_ABC(op, a, b, c) | ((Instruction)((uint32_t)(n) & 0xFFu) << 56))
#define INSTR_ABXN(op, a, bx, n) \
    (INSTR_ABX(op, a, bx) | ((Instruction)((uint32_t)(n) & 0xFFu) << 56))

#endif // OBY_OPCODE_H
