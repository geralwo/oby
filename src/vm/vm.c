#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm/vm.h"
#include "vm/opcode.h"
#include "native/native.h"
#include "core/array.h"
#include "core/object.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* `need` is a byte count. */
static bool stack_ensure(VM *vm, size_t need) {
    if (need <= vm->stack_cap) return true;
    size_t cap = vm->stack_cap ? vm->stack_cap : VM_STACK_INIT;
    while (cap < need) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(vm->stack, cap);
    if (!p) return false;
    vm->stack = p;
    vm->stack_cap = cap;
    return true;
}

static bool truthy(Value v) {
    switch (v.tag) {
        case VAL_NULL:  return false;
        case VAL_BOOL:  return v.as.b;
        case VAL_INT:   return v.as.i != 0;
        case VAL_FLOAT: return v.as.f != 0.0;
        default:        return true;
    }
}

static bool values_equal(Value a, Value b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case VAL_NULL:  return true;
        case VAL_BOOL:  return a.as.b == b.as.b;
        case VAL_INT:   return a.as.i == b.as.i;
        case VAL_FLOAT: return a.as.f == b.as.f;
        case VAL_STRING: return string_equal(a.as.str, b.as.str);
        default:        return a.as.obj == b.as.obj;
    }
}

/* ------------------------------------------------------------------ */
/* Packed values                                                      */
/* ------------------------------------------------------------------ */

/* Bytes a packed kind occupies. */
static size_t kind_size(int kind) {
    switch (kind) {
        case KIND_BYTE:    return 1;
        case KIND_INT32:
        case KIND_UINT32:
        case KIND_FLOAT32: return 4;
        case KIND_INT64:
        case KIND_UINT64:
        case KIND_FLOAT64: return 8;
        default:           return sizeof(Value);
    }
}

/* Read a packed value of `kind` from `p` and box it into a Value. Signed kinds
   sign-extend and unsigned ones zero-extend, so the boxed value is the number. */
static Value box_value(int kind, const uint8_t *p) {
    switch (kind) {
        case KIND_BYTE:    return val_int(*(const uint8_t *)p);
        case KIND_INT32:   return val_int(*(const int32_t *)p);
        case KIND_UINT32:  return val_int(*(const uint32_t *)p);
        case KIND_FLOAT32: return val_float(*(const float *)p);
        case KIND_INT64:   return val_int(*(const int64_t *)p);
        case KIND_UINT64:  return val_int((int64_t)*(const uint64_t *)p);
        case KIND_FLOAT64: return val_float(*(const double *)p);
        default:           return val_null();
    }
}

/* Write `v` into `p` as a packed value of `kind`. */
static void unbox_value(int kind, uint8_t *p, Value v) {
    switch (kind) {
        case KIND_BYTE:    *(uint8_t *)p = (uint8_t)v.as.i; break;
        case KIND_INT32:   *(int32_t *)p = (int32_t)v.as.i; break;
        case KIND_UINT32:  *(uint32_t *)p = (uint32_t)v.as.i; break;
        case KIND_FLOAT32: *(float *)p = (float)v.as.f; break;
        case KIND_INT64:   *(int64_t *)p = v.as.i; break;
        case KIND_UINT64:  *(uint64_t *)p = (uint64_t)v.as.i; break;
        case KIND_FLOAT64: *(double *)p = v.as.f; break;
        default: break;
    }
}

/* Convert a packed value at `src` (kind `from`) to kind `to` at `dst`. */
static void convert_value(int from, int to, uint8_t *dst, const uint8_t *src) {
    if (from == to) {
        memcpy(dst, src, kind_size(from));
        return;
    }
    bool from_f = (from == KIND_FLOAT32 || from == KIND_FLOAT64);
    bool to_f = (to == KIND_FLOAT32 || to == KIND_FLOAT64);
    if (from_f || to_f) {
        double d = from_f ? box_value(from, src).as.f : (double)box_value(from, src).as.i;
        if (to == KIND_FLOAT32) *(float *)dst = (float)d;
        else if (to == KIND_FLOAT64) *(double *)dst = d;
        else unbox_value(to, dst, val_int((int64_t)d));
    } else {
        unbox_value(to, dst, val_int(box_value(from, src).as.i));
    }
}

/* Truthiness of a raw packed value (or a tagged Value when kind is TAGGED). */
static bool raw_truthy(int kind, const uint8_t *p) {
    switch (kind) {
        case KIND_BYTE:    return *(const uint8_t *)p != 0;
        case KIND_INT32:   return *(const int32_t *)p != 0;
        case KIND_UINT32:  return *(const uint32_t *)p != 0;
        case KIND_FLOAT32: return *(const float *)p != 0.0f;
        case KIND_INT64:   return *(const int64_t *)p != 0;
        case KIND_UINT64:  return *(const uint64_t *)p != 0;
        case KIND_FLOAT64: return *(const double *)p != 0.0;
        default:           return truthy(*(const Value *)p);
    }
}

/* The result of a comparison opcode on two raw operands. */
static bool cmp_int(OpCode op, int64_t x, int64_t y) {
    switch (op) {
        case OP_EQ: return x == y;
        case OP_NE: return x != y;
        case OP_LT: return x < y;
        case OP_GT: return x > y;
        case OP_LE: return x <= y;
        default:    return x >= y;
    }
}

/* Unsigned comparison, for the unsigned kinds. */
static bool cmp_uint(OpCode op, uint64_t x, uint64_t y) {
    switch (op) {
        case OP_EQ: return x == y;
        case OP_NE: return x != y;
        case OP_LT: return x < y;
        case OP_GT: return x > y;
        case OP_LE: return x <= y;
        default:    return x >= y;
    }
}

static bool cmp_float(OpCode op, double x, double y) {
    switch (op) {
        case OP_EQ: return x == y;
        case OP_NE: return x != y;
        case OP_LT: return x < y;
        case OP_GT: return x > y;
        case OP_LE: return x <= y;
        default:    return x >= y;
    }
}

/* A non-owning `byte[]` view over a String's bytes, so `io::write` can read it.
   The view struct leaks for now (no GC); the bytes are not owned. */
/* Write an intrinsic field value (String/Array) into `dst`, which holds `n`
   bytes: a tagged field takes the whole `Value`, a packed one is unboxed. */
static void store_intrinsic(uint8_t *dst, size_t n, Value v) {
    if (n >= sizeof(Value)) { *(Value *)dst = v; return; }
    switch (n) {
        case 1: *(uint8_t *)dst = (uint8_t)v.as.i; break;
        case 4: *(int32_t *)dst = (int32_t)v.as.i; break;
        case 8: *(int64_t *)dst = v.as.i; break;
        default: break;
    }
}

/* A `byte[]` view of the string's bytes. Created once and cached on the String;
   the view shares `data` and is not owned by the caller. */
static Value string_buffer_view(String *s) {
    if (!s->view) {
        Array *arr = (Array *)malloc(sizeof(Array));
        if (!arr) return val_null();
        array_init(arr, 1);
        arr->buffer = s->data;
        arr->capacity = s->length;
        array_set_length(arr, s->length);
        s->view = arr;
    }
    return val_array(s->view);
}

/* ------------------------------------------------------------------ */
/* VM lifecycle                                                       */
/* ------------------------------------------------------------------ */

void vm_init(VM *vm, size_t global_size) {
    vm->global_size = global_size;
    vm->globals = (uint8_t *)calloc(1, global_size ? global_size : 1);
    vm->stack = NULL;
    vm->stack_cap = 0;
    vm->frame_count = 0;
}

void vm_free(VM *vm) {
    free(vm->globals);
    free(vm->stack);
    vm->globals = NULL;
    vm->stack = NULL;
    vm->stack_cap = 0;
    vm->frame_count = 0;
}

/* ------------------------------------------------------------------ */
/* Execution                                                          */
/* ------------------------------------------------------------------ */

int vm_run(VM *vm, Function *program) {
    if (!stack_ensure(vm, program->frame_size + sizeof(Value))) return 1;

    Frame *f = &vm->frames[0];
    f->fn = program;
    f->closure = NULL;
    f->ip = 0;
    f->base = 0;
    f->ret_base = 0;
    vm->frame_count = 1;

/* Registers are byte offsets from the frame base. */
#define R(i) (*(Value *)(vm->stack + f->base + (i)))

    for (;;) {
        Instruction ins = f->fn->code[f->ip++];

        switch (OP_GET(ins)) {
            case OP_HALT:
                return 0;

            case OP_LOADK: {
                size_t a = (size_t)OP_A(ins);
                int kind = (int)OP_N(ins);
                Value k = f->fn->consts[OP_BX(ins)];
                if (kind == KIND_TAGGED) *(Value *)(vm->stack + f->base + a) = k;
                else unbox_value(kind, vm->stack + f->base + a, k);
                break;
            }

            case OP_LOADNIL:
                R(OP_A(ins)) = val_null();
                break;

            case OP_ZERO:
                memset(vm->stack + f->base + OP_A(ins), 0, (size_t)OP_B(ins));
                break;

            case OP_MOVE:
                R(OP_A(ins)) = R(OP_B(ins));
                break;

            case OP_MOVEN: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t n = (size_t)OP_N(ins);
                if (n) memcpy(vm->stack + f->base + a, vm->stack + f->base + b, n);
                break;
            }

            case OP_GETGLOBAL: {
                size_t a = (size_t)OP_A(ins);
                size_t bx = (size_t)OP_BX(ins);
                size_t n = (size_t)OP_N(ins);
                if (n) memcpy(vm->stack + f->base + a, vm->globals + bx, n);
                break;
            }

            case OP_SETGLOBAL: {
                size_t a = (size_t)OP_A(ins);
                size_t bx = (size_t)OP_BX(ins);
                size_t n = (size_t)OP_N(ins);
                if (n) memcpy(vm->globals + bx, vm->stack + f->base + a, n);
                break;
            }

            case OP_ADD_INT:
            case OP_SUB_INT:
            case OP_MUL_INT:
            case OP_DIV_INT:
            case OP_MOD_INT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int kind = (int)OP_N(ins);
                OpCode op = OP_GET(ins);
                bool div = (op == OP_DIV_INT || op == OP_MOD_INT);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                const uint8_t *pc = vm->stack + f->base + c;
                bool zero = false;
                switch (kind) {
                    case KIND_BYTE: {
                        uint8_t x = *(const uint8_t *)pb, y = *(const uint8_t *)pc;
                        if (div && y == 0) { zero = true; break; }
                        *(uint8_t *)pa = op == OP_ADD_INT ? (uint8_t)(x + y)
                                       : op == OP_SUB_INT ? (uint8_t)(x - y)
                                       : op == OP_MUL_INT ? (uint8_t)(x * y)
                                       : op == OP_DIV_INT ? (uint8_t)(x / y)
                                       : (uint8_t)(x % y);
                        break;
                    }
                    case KIND_INT64: {
                        int64_t x = *(const int64_t *)pb, y = *(const int64_t *)pc;
                        if (div && y == 0) { zero = true; break; }
                        *(int64_t *)pa = op == OP_ADD_INT ? x + y
                                       : op == OP_SUB_INT ? x - y
                                       : op == OP_MUL_INT ? x * y
                                       : op == OP_DIV_INT ? x / y
                                       : x % y;
                        break;
                    }
                    case KIND_UINT32: {
                        uint32_t x = *(const uint32_t *)pb, y = *(const uint32_t *)pc;
                        if (div && y == 0) { zero = true; break; }
                        *(uint32_t *)pa = op == OP_ADD_INT ? x + y
                                        : op == OP_SUB_INT ? x - y
                                        : op == OP_MUL_INT ? x * y
                                        : op == OP_DIV_INT ? x / y
                                        : x % y;
                        break;
                    }
                    case KIND_UINT64: {
                        uint64_t x = *(const uint64_t *)pb, y = *(const uint64_t *)pc;
                        if (div && y == 0) { zero = true; break; }
                        *(uint64_t *)pa = op == OP_ADD_INT ? x + y
                                        : op == OP_SUB_INT ? x - y
                                        : op == OP_MUL_INT ? x * y
                                        : op == OP_DIV_INT ? x / y
                                        : x % y;
                        break;
                    }
                    default: {
                        int32_t x = *(const int32_t *)pb, y = *(const int32_t *)pc;
                        if (div && y == 0) { zero = true; break; }
                        *(int32_t *)pa = op == OP_ADD_INT ? x + y
                                       : op == OP_SUB_INT ? x - y
                                       : op == OP_MUL_INT ? x * y
                                       : op == OP_DIV_INT ? x / y
                                       : x % y;
                        break;
                    }
                }
                if (zero) {
                    fprintf(stderr, "vm error: division by zero\n");
                    return 1;
                }
                break;
            }

            case OP_ADD_FLOAT:
            case OP_SUB_FLOAT:
            case OP_MUL_FLOAT:
            case OP_DIV_FLOAT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int kind = (int)OP_N(ins);
                OpCode op = OP_GET(ins);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                const uint8_t *pc = vm->stack + f->base + c;
                if (kind == KIND_FLOAT64) {
                    double x = *(const double *)pb, y = *(const double *)pc;
                    *(double *)pa = op == OP_ADD_FLOAT ? x + y
                                  : op == OP_SUB_FLOAT ? x - y
                                  : op == OP_MUL_FLOAT ? x * y
                                  : x / y;
                } else {
                    float x = *(const float *)pb, y = *(const float *)pc;
                    *(float *)pa = op == OP_ADD_FLOAT ? x + y
                                 : op == OP_SUB_FLOAT ? x - y
                                 : op == OP_MUL_FLOAT ? x * y
                                 : x / y;
                }
                break;
            }

            case OP_NEG_INT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                int kind = (int)OP_N(ins);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                switch (kind) {
                    case KIND_BYTE:  *(uint8_t *)pa = (uint8_t)(-*(const uint8_t *)pb); break;
                    case KIND_INT64: *(int64_t *)pa = -*(const int64_t *)pb; break;
                    case KIND_UINT32: *(uint32_t *)pa = (uint32_t)(-*(const uint32_t *)pb); break;
                    case KIND_UINT64: *(uint64_t *)pa = (uint64_t)(-*(const uint64_t *)pb); break;
                    default:         *(int32_t *)pa = -*(const int32_t *)pb; break;
                }
                break;
            }

            case OP_NEG_FLOAT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                int kind = (int)OP_N(ins);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                if (kind == KIND_FLOAT64) *(double *)pa = -*(const double *)pb;
                else *(float *)pa = -*(const float *)pb;
                break;
            }

            case OP_BAND:
            case OP_BOR:
            case OP_BXOR: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int kind = (int)OP_N(ins);
                OpCode op = OP_GET(ins);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                const uint8_t *pc = vm->stack + f->base + c;
                switch (kind) {
                    case KIND_BYTE: {
                        uint8_t x = *(const uint8_t *)pb, y = *(const uint8_t *)pc;
                        *(uint8_t *)pa = op == OP_BAND ? (uint8_t)(x & y)
                                       : op == OP_BOR ? (uint8_t)(x | y)
                                       : (uint8_t)(x ^ y);
                        break;
                    }
                    case KIND_INT32:
                    case KIND_UINT32: {
                        uint32_t x = *(const uint32_t *)pb, y = *(const uint32_t *)pc;
                        *(uint32_t *)pa = op == OP_BAND ? (x & y)
                                        : op == OP_BOR ? (x | y)
                                        : (x ^ y);
                        break;
                    }
                    case KIND_INT64:
                    case KIND_UINT64: {
                        uint64_t x = *(const uint64_t *)pb, y = *(const uint64_t *)pc;
                        *(uint64_t *)pa = op == OP_BAND ? (x & y)
                                        : op == OP_BOR ? (x | y)
                                        : (x ^ y);
                        break;
                    }
                    default: break;
                }
                break;
            }

            case OP_SHL:
            case OP_SHRU:
            case OP_SAR: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int kind = (int)OP_N(ins);
                OpCode op = OP_GET(ins);
                int64_t count = *(const int64_t *)(vm->stack + f->base + c);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                unsigned long long bits = 0;
                switch (kind) {
                    case KIND_BYTE:   bits = 8;  break;
                    case KIND_INT32:
                    case KIND_UINT32: bits = 32; break;
                    case KIND_INT64:
                    case KIND_UINT64: bits = 64; break;
                    default: break;
                }
                if (bits == 0 || count < 0 || (unsigned long long)count >= bits) {
                    fprintf(stderr, "vm error: shift count out of range\n");
                    return 1;
                }
                switch (kind) {
                    case KIND_BYTE: {
                        uint8_t x = *(const uint8_t *)pb;
                        *(uint8_t *)pa = op == OP_SHL ? (uint8_t)(x << count)
                                       : (uint8_t)(x >> count);
                        break;
                    }
                    case KIND_INT32: {
                        int32_t x = *(const int32_t *)pb;
                        *(int32_t *)pa = op == OP_SHL ? (int32_t)((uint32_t)x << count)
                                       : op == OP_SAR ? (x >> count)
                                       : (int32_t)((uint32_t)x >> count);
                        break;
                    }
                    case KIND_UINT32: {
                        uint32_t x = *(const uint32_t *)pb;
                        *(uint32_t *)pa = op == OP_SHL ? (x << count) : (x >> count);
                        break;
                    }
                    case KIND_INT64: {
                        int64_t x = *(const int64_t *)pb;
                        *(int64_t *)pa = op == OP_SHL ? (int64_t)((uint64_t)x << count)
                                       : op == OP_SAR ? (x >> count)
                                       : (int64_t)((uint64_t)x >> count);
                        break;
                    }
                    case KIND_UINT64: {
                        uint64_t x = *(const uint64_t *)pb;
                        *(uint64_t *)pa = op == OP_SHL ? (x << count) : (x >> count);
                        break;
                    }
                    default: break;
                }
                break;
            }

            case OP_BNOT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                int kind = (int)OP_N(ins);
                uint8_t *pa = vm->stack + f->base + a;
                const uint8_t *pb = vm->stack + f->base + b;
                switch (kind) {
                    case KIND_BYTE:   *(uint8_t *)pa = (uint8_t)~*(const uint8_t *)pb; break;
                    case KIND_INT32:
                    case KIND_UINT32: *(uint32_t *)pa = ~*(const uint32_t *)pb; break;
                    case KIND_INT64:
                    case KIND_UINT64: *(uint64_t *)pa = ~*(const uint64_t *)pb; break;
                    default: break;
                }
                break;
            }

            case OP_EQ:
            case OP_NE:
            case OP_LT:
            case OP_GT:
            case OP_LE:
            case OP_GE: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int kind = (int)OP_N(ins);
                OpCode op = OP_GET(ins);
                const uint8_t *pb = vm->stack + f->base + b;
                const uint8_t *pc = vm->stack + f->base + c;
                bool res;
                switch (kind) {
                    case KIND_BYTE:
                        res = cmp_int(op, *(const uint8_t *)pb, *(const uint8_t *)pc);
                        break;
                    case KIND_INT32:
                        res = cmp_int(op, *(const int32_t *)pb, *(const int32_t *)pc);
                        break;
                    case KIND_UINT32:
                        res = cmp_uint(op, *(const uint32_t *)pb, *(const uint32_t *)pc);
                        break;
                    case KIND_INT64:
                        res = cmp_int(op, *(const int64_t *)pb, *(const int64_t *)pc);
                        break;
                    case KIND_UINT64:
                        res = cmp_uint(op, *(const uint64_t *)pb, *(const uint64_t *)pc);
                        break;
                    case KIND_FLOAT32:
                        res = cmp_float(op, *(const float *)pb, *(const float *)pc);
                        break;
                    case KIND_FLOAT64:
                        res = cmp_float(op, *(const double *)pb, *(const double *)pc);
                        break;
                    default: {
                        Value x = *(const Value *)pb, y = *(const Value *)pc;
                        if (op == OP_EQ) res = values_equal(x, y);
                        else if (op == OP_NE) res = !values_equal(x, y);
                        else if (x.tag == VAL_STRING && y.tag == VAL_STRING) {
                            size_t n = x.as.str->length < y.as.str->length
                                           ? x.as.str->length : y.as.str->length;
                            int cmp = memcmp(x.as.str->data, y.as.str->data, n);
                            if (cmp == 0) cmp = (x.as.str->length > y.as.str->length)
                                                    - (x.as.str->length < y.as.str->length);
                            res = op == OP_LT ? cmp < 0
                                : op == OP_GT ? cmp > 0
                                : op == OP_LE ? cmp <= 0 : cmp >= 0;
                        } else {
                            res = false;
                        }
                        break;
                    }
                }
                *(int32_t *)(vm->stack + f->base + a) = res ? 1 : 0;
                break;
            }

            case OP_EQPACKED:
            case OP_NEPACKED: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                size_t n = (size_t)OP_N(ins);
                bool eq = memcmp(vm->stack + f->base + b,
                                 vm->stack + f->base + c, n) == 0;
                if (OP_GET(ins) == OP_NEPACKED) eq = !eq;
                *(int32_t *)(vm->stack + f->base + a) = eq ? 1 : 0;
                break;
            }

            case OP_NOT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                int kind = (int)OP_N(ins);
                bool t = raw_truthy(kind, vm->stack + f->base + b);
                if (kind == KIND_TAGGED) R(a) = val_bool(!t);
                else *(int32_t *)(vm->stack + f->base + a) = t ? 0 : 1;
                break;
            }

            case OP_JMP:
                f->ip = (uint32_t)((int32_t)f->ip + OP_SBX(ins));
                break;

            case OP_TEST: {
                size_t a = (size_t)OP_A(ins);
                int kind = (int)OP_N(ins);
                if (raw_truthy(kind, vm->stack + f->base + a)) f->ip++;
                break;
            }

            case OP_GETFIELD: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                size_t n = (size_t)OP_N(ins);
                Value obj = R(b);
                if (obj.tag == VAL_STRING) {
                    /* String's intrinsic fields, by index. */
                    String *s = obj.as.str;
                    Value v = (c == 0) ? string_buffer_view(s)
                            : (c == 1) ? val_int((int64_t)s->length) : val_null();
                    store_intrinsic(vm->stack + f->base + a, n, v);
                } else if (obj.tag == VAL_ARRAY) {
                    Value v = (c == 0) ? val_int((int64_t)obj.as.arr->length) : val_null();
                    store_intrinsic(vm->stack + f->base + a, n, v);
                } else if (obj.tag == VAL_OBJ && obj.as.obj->kind == OBJ_OBJECT) {
                    Object *o = (Object *)obj.as.obj;
                    if (c + n > o->size) {
                        fprintf(stderr,
                                "vm error: member access out of range (offset %zu, size %zu, object %zu)\n",
                                c, n, o->size);
                        return 1;
                    }
                    memcpy(vm->stack + f->base + a, o->data + c, n);
                } else {
                    /* Silence here would leave R[A] holding stale bytes. */
                    fprintf(stderr, "vm error: member access on %s\n",
                            obj.tag == VAL_NULL ? "null" : "a non-object");
                    return 1;
                }
                break;
            }

            case OP_SETFIELD: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                size_t n = (size_t)OP_N(ins);
                Value obj = R(a);
                if (obj.tag == VAL_OBJ && obj.as.obj->kind == OBJ_OBJECT) {
                    Object *o = (Object *)obj.as.obj;
                    if (b + n > o->size) {
                        fprintf(stderr,
                                "vm error: member store out of range (offset %zu, size %zu, object %zu)\n",
                                b, n, o->size);
                        return 1;
                    }
                    memcpy(o->data + b, vm->stack + f->base + c, n);
                } else {
                    /* Silence here would silently discard the write. */
                    fprintf(stderr, "vm error: cannot set a member on %s\n",
                            obj.tag == VAL_NULL ? "null" : "a non-object");
                    return 1;
                }
                break;
            }

            case OP_BOX: {
                size_t a = (size_t)OP_A(ins);
                int kind = (int)OP_N(ins);
                *(Value *)(vm->stack + f->base + a) =
                    box_value(kind, vm->stack + f->base + OP_B(ins));
                break;
            }

            case OP_UNBOX: {
                size_t a = (size_t)OP_A(ins);
                int kind = (int)OP_N(ins);
                unbox_value(kind, vm->stack + f->base + a,
                            *(const Value *)(vm->stack + f->base + OP_B(ins)));
                break;
            }

            case OP_CONV: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                convert_value((int)OP_C(ins), (int)OP_N(ins),
                              vm->stack + f->base + a, vm->stack + f->base + b);
                break;
            }

            case OP_NEWOBJ:
                R(OP_A(ins)) = val_obj((Obj *)object_new((size_t)OP_BX(ins)));
                break;

            case OP_NEWARRAY: {
                size_t a = (size_t)OP_A(ins);
                size_t esize = (size_t)OP_B(ins);
                size_t cap = (size_t)OP_C(ins);
                Array *arr = array_new(esize);
                if (!arr || (cap && array_reserve(arr, cap) != 0)) {
                    fprintf(stderr, "vm error: out of memory\n");
                    return 1;
                }
                R(a) = val_array(arr);
                break;
            }

            case OP_APPEND: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                Value v = R(a);
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: append to a non-array\n");
                    return 1;
                }
                if (!(v.as.arr->flags & ARRAY_F_OWNED)) {
                    fprintf(stderr, "vm error: cannot grow a non-heap array\n");
                    return 1;
                }
                if (v.as.arr->flags & ARRAY_F_FIXED) {
                    fprintf(stderr, "vm error: cannot resize a fixed-size array\n");
                    return 1;
                }
                if (array_push(v.as.arr, vm->stack + f->base + b) != 0) {
                    fprintf(stderr, "vm error: array append failed\n");
                    return 1;
                }
                break;
            }

            case OP_GETINDEX: {
                size_t a = (size_t)OP_A(ins);
                Value v = R(OP_B(ins));
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: index into a non-array\n");
                    return 1;
                }
                Array *arr = v.as.arr;
                int32_t i = *(const int32_t *)(vm->stack + f->base + OP_C(ins));
                if (i < 0 || (size_t)i >= arr->length) {
                    fprintf(stderr, "vm error: index %d out of bounds (length %zu)\n",
                            (int)i, arr->length);
                    return 1;
                }
                memcpy(vm->stack + f->base + a,
                       (const uint8_t *)arr->buffer + (size_t)i * arr->size_of_element,
                       arr->size_of_element);
                break;
            }

            case OP_SETINDEX: {
                Value v = R(OP_A(ins));
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: index into a non-array\n");
                    return 1;
                }
                Array *arr = v.as.arr;
                int32_t i = *(const int32_t *)(vm->stack + f->base + OP_B(ins));
                if (i < 0 || (size_t)i >= arr->length) {
                    fprintf(stderr, "vm error: index %d out of bounds (length %zu)\n",
                            (int)i, arr->length);
                    return 1;
                }
                memcpy((uint8_t *)arr->buffer + (size_t)i * arr->size_of_element,
                       vm->stack + f->base + OP_C(ins), arr->size_of_element);
                break;
            }

            case OP_ARRAY_POP: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                Value v = R(a);
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: pop from a non-array\n");
                    return 1;
                }
                if (!(v.as.arr->flags & ARRAY_F_OWNED)) {
                    fprintf(stderr, "vm error: cannot grow a non-heap array\n");
                    return 1;
                }
                if (v.as.arr->flags & ARRAY_F_FIXED) {
                    fprintf(stderr, "vm error: cannot resize a fixed-size array\n");
                    return 1;
                }
                if (!array_pop(v.as.arr, vm->stack + f->base + b)) {
                    fprintf(stderr, "vm error: pop from an empty array\n");
                    return 1;
                }
                break;
            }

            case OP_ARRAY_INSERT: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                Value v = R(a);
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: insert into a non-array\n");
                    return 1;
                }
                Array *arr = v.as.arr;
                if (!(arr->flags & ARRAY_F_OWNED)) {
                    fprintf(stderr, "vm error: cannot grow a non-heap array\n");
                    return 1;
                }
                if (arr->flags & ARRAY_F_FIXED) {
                    fprintf(stderr, "vm error: cannot resize a fixed-size array\n");
                    return 1;
                }
                int32_t i = *(const int32_t *)(vm->stack + f->base + b);
                if (i < 0 || (size_t)i > arr->length) {
                    fprintf(stderr, "vm error: insert index %d out of bounds (length %zu)\n",
                            (int)i, arr->length);
                    return 1;
                }
                if (array_insert(arr, (size_t)i, vm->stack + f->base + c) != 0) {
                    fprintf(stderr, "vm error: array insert failed\n");
                    return 1;
                }
                break;
            }

            case OP_ARRAY_REMOVE: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                Value v = R(a);
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: remove from a non-array\n");
                    return 1;
                }
                Array *arr = v.as.arr;
                if (!(arr->flags & ARRAY_F_OWNED)) {
                    fprintf(stderr, "vm error: cannot grow a non-heap array\n");
                    return 1;
                }
                if (arr->flags & ARRAY_F_FIXED) {
                    fprintf(stderr, "vm error: cannot resize a fixed-size array\n");
                    return 1;
                }
                int32_t i = *(const int32_t *)(vm->stack + f->base + b);
                if (i < 0 || (size_t)i >= arr->length) {
                    fprintf(stderr, "vm error: remove index %d out of bounds (length %zu)\n",
                            (int)i, arr->length);
                    return 1;
                }
                array_remove(arr, (size_t)i, vm->stack + f->base + c);
                break;
            }

            case OP_ARRAY_FIXED: {
                Value v = R(OP_A(ins));
                if (v.tag == VAL_ARRAY) v.as.arr->flags |= ARRAY_F_FIXED;
                break;
            }

            case OP_ARRAY_REPEAT: {
                /* `[expr; n]` with a runtime count: append R[C] to arr(R[A]),
                   R[B] (int32) times. */
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                int64_t count = *(const int32_t *)(vm->stack + f->base + b);
                if (count < 0) {
                    fprintf(stderr, "vm error: repeat count is negative\n");
                    return 1;
                }
                Value v = R(a);
                if (v.tag != VAL_ARRAY) {
                    fprintf(stderr, "vm error: append to a non-array\n");
                    return 1;
                }
                if (!(v.as.arr->flags & ARRAY_F_OWNED)) {
                    fprintf(stderr, "vm error: cannot grow a non-heap array\n");
                    return 1;
                }
                if (v.as.arr->flags & ARRAY_F_FIXED) {
                    fprintf(stderr, "vm error: cannot resize a fixed-size array\n");
                    return 1;
                }
                if (count > 0 && array_reserve(v.as.arr, (size_t)count) != 0) {
                    fprintf(stderr, "vm error: out of memory\n");
                    return 1;
                }
                for (int64_t i = 0; i < count; i++) {
                    if (array_push(v.as.arr, vm->stack + f->base + c) != 0) {
                        fprintf(stderr, "vm error: array append failed\n");
                        return 1;
                    }
                }
                break;
            }

            case OP_CALL: {
                size_t a = (size_t)OP_A(ins);
                Value callee = R(a);
                Function *fn;
                Closure *cl = NULL;
                if (callee.tag == VAL_CLOSURE) {
                    cl = callee.as.closure;
                    fn = cl->fn;
                } else if (callee.tag == VAL_FUNC) {
                    fn = callee.as.fn;
                } else {
                    fprintf(stderr, "vm error: attempt to call a non-function\n");
                    return 1;
                }
                if (vm->frame_count >= VM_FRAMES_MAX) {
                    fprintf(stderr, "vm error: call stack overflow\n");
                    return 1;
                }
                size_t base = f->base + a + sizeof(Value);
                if (!stack_ensure(vm, base + fn->frame_size + sizeof(Value))) return 1;
                Frame *nf = &vm->frames[vm->frame_count++];
                nf->fn = fn;
                nf->closure = cl;
                nf->ip = 0;
                nf->base = base;
                nf->ret_base = f->base + a;
                f = nf;
                break;
            }

            case OP_CLOSURE: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                if (b >= f->fn->const_count || f->fn->consts[b].tag != VAL_FUNC) {
                    fprintf(stderr, "vm error: bad closure constant\n");
                    return 1;
                }
                Function *cfn = f->fn->consts[b].as.fn;
                Closure *cl = closure_new(cfn, cfn->up_total);
                if (cfn->up_total && !cl) {
                    fprintf(stderr, "vm error: out of memory\n");
                    return 1;
                }
                /* The captured values sit in R[C..] in the function's own
                   upvalue layout, so the copy is offset-for-offset. */
                for (size_t i = 0; i < cfn->up_count; i++)
                    memcpy(cl->up + cfn->up_off[i],
                           vm->stack + f->base + c + cfn->up_off[i],
                           cfn->up_size[i]);
                R(a) = val_closure(cl);
                break;
            }

            case OP_GETUPVAL: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);
                size_t c = (size_t)OP_C(ins);
                if (!f->closure || b >= f->fn->up_count) {
                    fprintf(stderr, "vm error: no such upvalue\n");
                    return 1;
                }
                memcpy(vm->stack + f->base + a,
                       f->closure->up + f->fn->up_off[b], c);
                break;
            }

            case OP_CALLNATIVE: {
                size_t a = (size_t)OP_A(ins);
                int bx = (int)OP_B(ins);
                int c = (int)OP_C(ins);
                Value *args = (Value *)(vm->stack + f->base + a);
                R(a) = native_call(bx, args, c);
                break;
            }

            case OP_RETURN: {
                size_t a = (size_t)OP_A(ins);
                size_t b = (size_t)OP_B(ins);   /* bytes */
                size_t ret_base = f->ret_base;
                uint8_t *results = vm->stack + f->base + a;
                vm->frame_count--;
                if (vm->frame_count == 0) return 0;
                if (b) memcpy(vm->stack + ret_base, results, b);
                f = &vm->frames[vm->frame_count - 1];
                break;
            }

            default:
                fprintf(stderr, "vm error: bad opcode %d\n", (int)OP_GET(ins));
                return 1;
        }
    }

#undef R
}
