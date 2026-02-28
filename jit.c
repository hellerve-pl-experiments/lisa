#include "jit.h"
#include "chunk.h"
#include "vm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wc23-extensions"
#include "ctx.h"
#include "op.h"
#include "register.h"
#pragma GCC diagnostic pop

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef lisa_value (*lisa_jit_fn)(lisa_vm *vm, lisa_obj_closure *closure,
                                  lisa_value *slots);

/* --- Platform-specific register definitions --- */

#if defined(__x86_64__) || defined(_M_X64)

/* Callee-saved registers holding persistent state */
#define REG_VM       "rbx"
#define REG_SLOTS    "r12"
#define REG_CLOSURE  "r13"
#define REG_STKTOP   "r14"
#define REG_CONSTS   "r15"

/* Temporaries (caller-saved) */
#define REG_TMP1     "rax"
#define REG_TMP2     "rcx"
#define REG_TMP3     "rdx"
#define REG_TMP4     "rsi"
#define REG_TMP5     "rdi"
#define REG_TMP6     "r8"
#define REG_TMP7     "r9"
#define REG_CALLADDR "r10"

/* ABI argument registers */
#define REG_ARG0     "rdi"
#define REG_ARG1     "rsi"
#define REG_ARG2     "rdx"
#define REG_ARG3     "rcx"
#define REG_ARG4     "r8"
#define REG_ARG5     "r9"
#define REG_RET      "rax"

#elif defined(__aarch64__) || defined(_M_ARM64)

#define REG_VM       "x19"
#define REG_SLOTS    "x20"
#define REG_CLOSURE  "x21"
#define REG_STKTOP   "x22"
#define REG_CONSTS   "x23"

#define REG_TMP1     "x0"
#define REG_TMP2     "x1"
#define REG_TMP3     "x2"
#define REG_TMP4     "x3"
#define REG_TMP5     "x4"
#define REG_TMP6     "x5"
#define REG_TMP7     "x6"
#define REG_CALLADDR "x9"

#define REG_ARG0     "x0"
#define REG_ARG1     "x1"
#define REG_ARG2     "x2"
#define REG_ARG3     "x3"
#define REG_ARG4     "x4"
#define REG_ARG5     "x5"
#define REG_RET      "x0"

#endif

/* --- Operand helpers --- */

static cj_operand reg(const char *name) { return cj_make_register(name); }
static cj_operand imm(uint64_t val)     { return cj_make_constant(val); }
static cj_operand mem(const char *base, int32_t disp) {
    return cj_make_memory(base, NULL, 1, disp);
}

/* Load a 64-bit immediate into a register */
static void emit_load_imm64(cj_ctx *ctx, const char *dst, uint64_t value) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, reg(dst), imm(value));
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* Use movz/movk sequence for 64-bit constants */
    cj_operand d = reg(dst);
    if (value == 0) {
        cj_mov(ctx, d, reg("xzr"));
        return;
    }
    uint16_t chunk0 = (uint16_t)(value & 0xFFFF);
    cj_movz(ctx, d, imm(chunk0));
    for (int shift = 16; shift < 64; shift += 16) {
        uint16_t part = (uint16_t)((value >> shift) & 0xFFFF);
        if (!part) continue;
        uint64_t encoded = (uint64_t)part | ((uint64_t)(shift / 16) << 16);
        cj_movk(ctx, d, imm(encoded));
    }
#endif
}

/* Load a 64-bit value from memory [base + disp] into dst */
static void emit_load64(cj_ctx *ctx, const char *dst, const char *base, int32_t disp) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, reg(dst), mem(base, disp));
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_ldr(ctx, reg(dst), mem(base, disp));
#endif
}

/* Store a 64-bit value from src to memory [base + disp] */
static void emit_store64(cj_ctx *ctx, const char *src, const char *base, int32_t disp) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, mem(base, disp), reg(src));
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_str(ctx, reg(src), mem(base, disp));
#endif
}

/* Emit an indirect call to an absolute C function pointer */
static void emit_call_abs(cj_ctx *ctx, void *fn_ptr) {
    emit_load_imm64(ctx, REG_CALLADDR, (uint64_t)(uintptr_t)fn_ptr);
#if defined(__x86_64__) || defined(_M_X64)
    cj_call(ctx, reg(REG_CALLADDR));
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_blr(ctx, reg(REG_CALLADDR));
#endif
}

/* --- Stack operations --- */

/* Push a value (in tmp reg) onto the JIT stack (via REG_STKTOP register) */
static void emit_push(cj_ctx *ctx, const char *src_reg) {
    emit_store64(ctx, src_reg, REG_STKTOP, 0);
    cj_add(ctx, reg(REG_STKTOP), imm(8));
}

/* Pop a value from the JIT stack into dst_reg */
static void emit_pop(cj_ctx *ctx, const char *dst_reg) {
    cj_sub(ctx, reg(REG_STKTOP), imm(8));
    emit_load64(ctx, dst_reg, REG_STKTOP, 0);
}

/* Peek at stack_top[-1-distance] into dst_reg */
static void emit_peek(cj_ctx *ctx, const char *dst_reg, int distance) {
    int32_t offset = (int32_t)(-8 * (1 + distance));
    emit_load64(ctx, dst_reg, REG_STKTOP, offset);
}

/* Sync stack_top register to vm->stack_top */
static void emit_sync_stack_top(cj_ctx *ctx) {
    emit_store64(ctx, REG_STKTOP, REG_VM,
                 (int32_t)offsetof(lisa_vm, stack_top));
}

/* Reload stack_top from vm->stack_top */
static void emit_reload_stack_top(cj_ctx *ctx) {
    emit_load64(ctx, REG_STKTOP, REG_VM,
                (int32_t)offsetof(lisa_vm, stack_top));
}

/* --- Prologue/Epilogue --- */

static void emit_prologue(cj_ctx *ctx) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_push(ctx, reg("rbp"));
    cj_mov(ctx, reg("rbp"), reg("rsp"));
    cj_push(ctx, reg("rbx"));
    cj_push(ctx, reg("r12"));
    cj_push(ctx, reg("r13"));
    cj_push(ctx, reg("r14"));
    cj_push(ctx, reg("r15"));
    /* Align stack to 16 bytes: 5 pushes (40 bytes) + push rbp (8) + ret addr (8)
     * = 56, need 8 more for 16-byte alignment */
    cj_sub(ctx, reg("rsp"), imm(8));

    /* Move arguments to callee-saved registers */
    /* SysV ABI: rdi=vm, rsi=closure, rdx=slots */
    cj_mov(ctx, reg(REG_VM), reg("rdi"));
    cj_mov(ctx, reg(REG_CLOSURE), reg("rsi"));
    cj_mov(ctx, reg(REG_SLOTS), reg("rdx"));
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* Save frame pointer, link register, and callee-saved registers */
    cj_stp(ctx, reg("x29"), reg("x30"), cj_make_preindexed("sp", -80));
    cj_mov(ctx, reg("x29"), reg("sp"));
    cj_stp(ctx, reg("x19"), reg("x20"), mem("sp", 16));
    cj_stp(ctx, reg("x21"), reg("x22"), mem("sp", 32));
    cj_str(ctx, reg("x23"), mem("sp", 48));

    /* Move arguments to callee-saved registers */
    /* AAPCS: x0=vm, x1=closure, x2=slots */
    cj_mov(ctx, reg(REG_VM), reg("x0"));
    cj_mov(ctx, reg(REG_CLOSURE), reg("x1"));
    cj_mov(ctx, reg(REG_SLOTS), reg("x2"));
#endif

    /* Load stack_top from vm->stack_top */
    emit_reload_stack_top(ctx);

    /* Load constants: closure->function->chunk.constants.values */
    emit_load64(ctx, REG_TMP1, REG_CLOSURE,
                (int32_t)offsetof(lisa_obj_closure, function));
    emit_load64(ctx, REG_CONSTS, REG_TMP1,
                (int32_t)(offsetof(lisa_obj_function, chunk)
                        + offsetof(lisa_chunk, constants)
                        + offsetof(lisa_value_array, values)));
}

static void emit_epilogue(cj_ctx *ctx) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_add(ctx, reg("rsp"), imm(8));
    cj_pop(ctx, reg("r15"));
    cj_pop(ctx, reg("r14"));
    cj_pop(ctx, reg("r13"));
    cj_pop(ctx, reg("r12"));
    cj_pop(ctx, reg("rbx"));
    cj_pop(ctx, reg("rbp"));
    cj_ret(ctx);
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_ldp(ctx, reg("x19"), reg("x20"), mem("sp", 16));
    cj_ldp(ctx, reg("x21"), reg("x22"), mem("sp", 32));
    cj_ldr(ctx, reg("x23"), mem("sp", 48));
    cj_ldp(ctx, reg("x29"), reg("x30"), cj_make_postindexed("sp", 80));
    cj_ret(ctx);
#endif
}

/* --- Call C helper with sync/reload --- */

/* Call with vm as arg0 and an int immediate as arg1 */
static void emit_call_vm_int(cj_ctx *ctx, void *fn_ptr, int int_arg) {
    emit_sync_stack_top(ctx);
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
    emit_load_imm64(ctx, REG_ARG1, (uint64_t)(uint32_t)int_arg);
    emit_call_abs(ctx, fn_ptr);
    emit_reload_stack_top(ctx);
}

/* Call with vm as arg0 and two lisa_value args (from tmp regs) */
static void emit_call_vm_val_val(cj_ctx *ctx, void *fn_ptr,
                                  const char *val_a, const char *val_b) {
    emit_sync_stack_top(ctx);
#if defined(__x86_64__) || defined(_M_X64)
    /* Args: rdi=vm, rsi=a, rdx=b */
    /* val_a and val_b are in TMP regs. Need to be careful:
       If val_a is "rax" and val_b is "rcx", move to rsi/rdx */
    cj_mov(ctx, reg(REG_ARG2), reg(val_b));
    cj_mov(ctx, reg(REG_ARG1), reg(val_a));
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* Args: x0=vm, x1=a, x2=b */
    /* val_a/val_b might be in x0/x1, need careful ordering */
    cj_mov(ctx, reg(REG_ARG2), reg(val_b));
    cj_mov(ctx, reg(REG_ARG1), reg(val_a));
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
#endif
    emit_call_abs(ctx, fn_ptr);
    emit_reload_stack_top(ctx);
}

/* --- Bytecode scanner for branch targets --- */

typedef struct {
    cj_label *labels;       /* label per bytecode offset */
    bool *is_target;        /* which offsets are branch targets */
    int code_len;
} label_map;

static void scan_branch_targets(lisa_chunk *chunk, label_map *map, cj_ctx *ctx) {
    int len = chunk->count;
    map->code_len = len;
    map->is_target = calloc((size_t)len, sizeof(bool));
    map->labels = calloc((size_t)len, sizeof(cj_label));

    /* First pass: find all branch targets */
    int i = 0;
    while (i < len) {
        uint8_t op = chunk->code[i];
        switch (op) {
        case OP_JUMP:
        case OP_JUMP_IF_FALSE: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 + offset;
            if (target >= 0 && target < len)
                map->is_target[target] = true;
            i += 3;
            break;
        }
        case OP_LOOP: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 - offset;
            if (target >= 0 && target < len)
                map->is_target[target] = true;
            i += 3;
            break;
        }
        case OP_CLOSURE: {
            uint8_t fn_idx = chunk->code[i + 1];
            lisa_obj_function *fn = AS_FUNCTION(chunk->constants.values[fn_idx]);
            i += 2 + fn->upvalue_count * 2;
            break;
        }
        /* Instructions with 1-byte operand */
        case OP_CONSTANT: case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_GET_UPVALUE: case OP_SET_UPVALUE:
        case OP_GET_GLOBAL: case OP_DEF_GLOBAL:
        case OP_CALL: case OP_TAIL_CALL:
        case OP_LIST: case OP_PRINTLN:
            i += 2;
            break;
        /* No-operand instructions */
        default:
            i += 1;
            break;
        }
    }

    /* Create labels for all branch targets */
    for (i = 0; i < len; i++) {
        if (map->is_target[i]) {
            map->labels[i] = cj_create_label(ctx);
        }
    }
}

static void free_label_map(label_map *map) {
    free(map->labels);
    free(map->is_target);
}

/* --- Main JIT compilation --- */

bool lisa_jit_compile(lisa_vm *vm, lisa_obj_function *fn) {
    (void)vm;

    if (fn->jit_code) return true; /* already compiled */

    cj_ctx *ctx = create_cj_ctx();
    if (!ctx) return false;

    lisa_chunk *chunk = &fn->chunk;

    /* Scan for branch targets and create labels */
    label_map map;
    scan_branch_targets(chunk, &map, ctx);

    /* Create a label for the function entry (for self-tail-calls) */
    cj_label entry_label = cj_create_label(ctx);

    /* Emit prologue */
    emit_prologue(ctx);
    cj_mark_label(ctx, entry_label);

    /* Walk bytecode and emit native code */
    int i = 0;
    while (i < chunk->count) {
        /* Mark label if this offset is a branch target */
        if (map.is_target[i]) {
            cj_mark_label(ctx, map.labels[i]);
        }

        uint8_t op = chunk->code[i];
        switch (op) {

        case OP_CONSTANT: {
            uint8_t idx = chunk->code[i + 1];
            /* tmp1 = constants[idx] */
            emit_load64(ctx, REG_TMP1, REG_CONSTS, (int32_t)(idx * 8));
            emit_push(ctx, REG_TMP1);
            i += 2;
            break;
        }

        case OP_NIL:
            emit_load_imm64(ctx, REG_TMP1, LISA_NIL);
            emit_push(ctx, REG_TMP1);
            i += 1;
            break;

        case OP_TRUE:
            emit_load_imm64(ctx, REG_TMP1, LISA_TRUE);
            emit_push(ctx, REG_TMP1);
            i += 1;
            break;

        case OP_FALSE:
            emit_load_imm64(ctx, REG_TMP1, LISA_FALSE);
            emit_push(ctx, REG_TMP1);
            i += 1;
            break;

        case OP_POP:
            cj_sub(ctx, reg(REG_STKTOP), imm(8));
            i += 1;
            break;

        case OP_GET_LOCAL: {
            uint8_t slot = chunk->code[i + 1];
            emit_load64(ctx, REG_TMP1, REG_SLOTS, (int32_t)(slot * 8));
            emit_push(ctx, REG_TMP1);
            i += 2;
            break;
        }

        case OP_SET_LOCAL: {
            uint8_t slot = chunk->code[i + 1];
            emit_peek(ctx, REG_TMP1, 0);
            emit_store64(ctx, REG_TMP1, REG_SLOTS, (int32_t)(slot * 8));
            i += 2;
            break;
        }

        case OP_GET_UPVALUE: {
            uint8_t slot = chunk->code[i + 1];
            /* closure->upvalues[slot]->location -> read value */
            emit_load64(ctx, REG_TMP1, REG_CLOSURE,
                        (int32_t)offsetof(lisa_obj_closure, upvalues));
            emit_load64(ctx, REG_TMP1, REG_TMP1, (int32_t)(slot * 8));
            emit_load64(ctx, REG_TMP1, REG_TMP1,
                        (int32_t)offsetof(lisa_obj_upvalue, location));
            emit_load64(ctx, REG_TMP1, REG_TMP1, 0);
            emit_push(ctx, REG_TMP1);
            i += 2;
            break;
        }

        case OP_SET_UPVALUE: {
            uint8_t slot = chunk->code[i + 1];
            emit_peek(ctx, REG_TMP2, 0); /* value */
            emit_load64(ctx, REG_TMP1, REG_CLOSURE,
                        (int32_t)offsetof(lisa_obj_closure, upvalues));
            emit_load64(ctx, REG_TMP1, REG_TMP1, (int32_t)(slot * 8));
            emit_load64(ctx, REG_TMP1, REG_TMP1,
                        (int32_t)offsetof(lisa_obj_upvalue, location));
            emit_store64(ctx, REG_TMP2, REG_TMP1, 0);
            i += 2;
            break;
        }

        case OP_GET_GLOBAL: {
            uint8_t idx = chunk->code[i + 1];
            /* Call lisa_jit_get_global(vm, idx) */
            emit_call_vm_int(ctx, (void *)lisa_jit_get_global, idx);
            /* Result is in REG_RET, push it */
            emit_push(ctx, REG_RET);
            i += 2;
            break;
        }

        case OP_DEF_GLOBAL: {
            uint8_t idx = chunk->code[i + 1];
            emit_peek(ctx, REG_TMP1, 0); /* value */
            cj_sub(ctx, reg(REG_STKTOP), imm(8)); /* pop */
            /* Call lisa_jit_def_global(vm, idx, value) */
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_load_imm64(ctx, REG_ARG1, (uint64_t)(uint32_t)idx);
            cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP1));
            emit_call_abs(ctx, (void *)lisa_jit_def_global);
            emit_reload_stack_top(ctx);
            i += 2;
            break;
        }

        case OP_ADD: {
            /* Pop two values, call helper, push result */
            emit_pop(ctx, REG_TMP2);  /* b */
            emit_pop(ctx, REG_TMP1);  /* a */
            emit_call_vm_val_val(ctx, (void *)lisa_jit_add, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_SUB: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_sub, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_MUL: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_mul, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_DIV: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_div, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_MOD: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_mod, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_NEGATE: {
            emit_pop(ctx, REG_TMP1);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP1));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_negate);
            emit_reload_stack_top(ctx);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_EQUAL: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_equal, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_NOT_EQUAL: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_not_equal, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_LESS: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_less, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_LESS_EQUAL: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_less_equal, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_GREATER: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_greater, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_GREATER_EQUAL: {
            emit_pop(ctx, REG_TMP2);
            emit_pop(ctx, REG_TMP1);
            emit_call_vm_val_val(ctx, (void *)lisa_jit_greater_equal, REG_TMP1, REG_TMP2);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_NOT: {
            emit_pop(ctx, REG_TMP1);
            /* inline falsey check: nil or false */
            cj_label is_falsey = cj_create_label(ctx);
            cj_label done = cj_create_label(ctx);

            /* Check for nil */
            emit_load_imm64(ctx, REG_TMP2, LISA_NIL);
            cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
#if defined(__x86_64__) || defined(_M_X64)
            cj_je(ctx, is_falsey);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_beq(ctx, is_falsey);
#endif
            /* Check for false */
            emit_load_imm64(ctx, REG_TMP2, LISA_FALSE);
            cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
#if defined(__x86_64__) || defined(_M_X64)
            cj_je(ctx, is_falsey);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_beq(ctx, is_falsey);
#endif
            /* Truthy: push false */
            emit_load_imm64(ctx, REG_TMP1, LISA_FALSE);
#if defined(__x86_64__) || defined(_M_X64)
            cj_jmp(ctx, done);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_b(ctx, done);
#endif
            /* Falsey: push true */
            cj_mark_label(ctx, is_falsey);
            emit_load_imm64(ctx, REG_TMP1, LISA_TRUE);

            cj_mark_label(ctx, done);
            emit_push(ctx, REG_TMP1);
            i += 1;
            break;
        }

        case OP_JUMP: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 + offset;
#if defined(__x86_64__) || defined(_M_X64)
            cj_jmp(ctx, map.labels[target]);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_b(ctx, map.labels[target]);
#endif
            i += 3;
            break;
        }

        case OP_JUMP_IF_FALSE: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 + offset;

            /* Peek and pop: check if falsey (nil or false) */
            emit_peek(ctx, REG_TMP1, 0);
            cj_sub(ctx, reg(REG_STKTOP), imm(8)); /* pop */

            /* if (val == LISA_NIL) goto target */
            emit_load_imm64(ctx, REG_TMP2, LISA_NIL);
            cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
#if defined(__x86_64__) || defined(_M_X64)
            cj_je(ctx, map.labels[target]);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_beq(ctx, map.labels[target]);
#endif
            /* if (val == LISA_FALSE) goto target */
            emit_load_imm64(ctx, REG_TMP2, LISA_FALSE);
            cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
#if defined(__x86_64__) || defined(_M_X64)
            cj_je(ctx, map.labels[target]);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_beq(ctx, map.labels[target]);
#endif
            i += 3;
            break;
        }

        case OP_LOOP: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 - offset;
#if defined(__x86_64__) || defined(_M_X64)
            cj_jmp(ctx, map.labels[target]);
#elif defined(__aarch64__) || defined(_M_ARM64)
            cj_b(ctx, map.labels[target]);
#endif
            i += 3;
            break;
        }

        case OP_CLOSURE: {
            uint8_t fn_idx = chunk->code[i + 1];
            lisa_obj_function *closure_fn = AS_FUNCTION(chunk->constants.values[fn_idx]);
            int uv_count = closure_fn->upvalue_count;
            /* ip points to the upvalue pairs */
            uint8_t *uv_ip = &chunk->code[i + 2];

            /* Call lisa_jit_make_closure(vm, enclosing, fn, ip) */
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_CLOSURE));
            emit_load_imm64(ctx, REG_ARG2, (uint64_t)(uintptr_t)closure_fn);
            emit_load_imm64(ctx, REG_ARG3, (uint64_t)(uintptr_t)uv_ip);
            emit_call_abs(ctx, (void *)lisa_jit_make_closure);
            emit_reload_stack_top(ctx);
            emit_push(ctx, REG_RET);

            i += 2 + uv_count * 2;
            break;
        }

        case OP_CALL: {
            int argc = chunk->code[i + 1];
            /* Sync stack, call helper */
            emit_call_vm_int(ctx, (void *)lisa_jit_call_helper, argc);
            /* Result is in REG_RET; stack_top has been adjusted by helper.
               The result is already pushed by the helper. */
            i += 2;
            break;
        }

        case OP_TAIL_CALL: {
            int argc = chunk->code[i + 1];

            /* Check for self-tail-call:
               If the preceding instruction was OP_GET_GLOBAL and the global
               name matches our function name, emit a self-tail-call jump. */
            bool is_self_call = false;
            if (fn->name != NULL && i >= 2 && chunk->code[i - 2] == OP_GET_GLOBAL) {
                uint8_t name_idx = chunk->code[i - 1];
                lisa_value name_val = chunk->constants.values[name_idx];
                if (IS_STRING(name_val)) {
                    lisa_obj_string *name_str = AS_STRING(name_val);
                    if (name_str == fn->name) {
                        is_self_call = true;
                    }
                }
            }

            if (is_self_call) {
                /* Self-tail-call: pop callee (the function ref we just pushed
                   via GET_GLOBAL), copy args to slots, jump to entry.
                   Stack layout: [... callee arg0 arg1 ... argN-1]
                   where callee is at stack_top[-argc-1] */

                /* Copy args to slots[1..argc] (slot 0 is the function itself) */
                for (int a = 0; a < argc; a++) {
                    /* arg[a] is at stack_top[-argc + a] */
                    int32_t src_off = (int32_t)(-8 * (argc - a));
                    emit_load64(ctx, REG_TMP1, REG_STKTOP, src_off);
                    emit_store64(ctx, REG_TMP1, REG_SLOTS, (int32_t)((1 + a) * 8));
                }
                /* Reset stack_top to just after the slots frame:
                   slots + argc + 1 (function + args) */
                cj_mov(ctx, reg(REG_STKTOP), reg(REG_SLOTS));
                cj_add(ctx, reg(REG_STKTOP), imm((uint64_t)(argc + 1) * 8));
                /* Jump back to function entry */
#if defined(__x86_64__) || defined(_M_X64)
                cj_jmp(ctx, entry_label);
#elif defined(__aarch64__) || defined(_M_ARM64)
                cj_b(ctx, entry_label);
#endif
            } else {
                /* General tail call: delegate to helper.
                   The helper will reuse the frame and dispatch. */
                emit_call_vm_int(ctx, (void *)lisa_jit_tail_call_helper, argc);
                /* The function's work is done; return the result */
                cj_mov(ctx, reg(REG_RET), reg(REG_RET)); /* result already in REG_RET */
                emit_epilogue(ctx);
            }
            i += 2;
            break;
        }

        case OP_RETURN: {
            /* Pop the return value into REG_RET */
            emit_pop(ctx, REG_RET);
            /* Sync stack_top before returning so the caller can see it */
            emit_sync_stack_top(ctx);
            emit_epilogue(ctx);
            i += 1;
            break;
        }

        case OP_CLOSE_UPVALUE: {
            /* close_upvalues(vm, stack_top - 1); stack_top-- */
            cj_sub(ctx, reg(REG_STKTOP), imm(8));
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_STKTOP));
            emit_call_abs(ctx, (void *)lisa_jit_close_upvalue);
            emit_reload_stack_top(ctx);
            i += 1;
            break;
        }

        case OP_CONS: {
            emit_pop(ctx, REG_TMP2);  /* cdr */
            emit_pop(ctx, REG_TMP1);  /* car */
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP1));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_cons);
            emit_reload_stack_top(ctx);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_CAR: {
            emit_pop(ctx, REG_TMP1);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP1));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_car);
            emit_reload_stack_top(ctx);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_CDR: {
            emit_pop(ctx, REG_TMP1);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP1));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_cdr);
            emit_reload_stack_top(ctx);
            emit_push(ctx, REG_RET);
            i += 1;
            break;
        }

        case OP_LIST: {
            int n = chunk->code[i + 1];
            /* Sync stack so helper can read items from vm->stack_top */
            emit_call_vm_int(ctx, (void *)lisa_jit_list, n);
            emit_push(ctx, REG_RET);
            i += 2;
            break;
        }

        case OP_PRINTLN: {
            int argc = chunk->code[i + 1];
            emit_call_vm_int(ctx, (void *)lisa_jit_println, argc);
            emit_push(ctx, REG_RET); /* pushes nil */
            i += 2;
            break;
        }

        default:
            /* Unknown/unhandled opcode — bail out of JIT compilation */
            fprintf(stderr, "JIT: unsupported opcode %d at offset %d\n", op, i);
            free_label_map(&map);
            destroy_cj_ctx(ctx);
            return false;
        }
    }

    /* Finalize executable code */
    cj_fn module = create_cj_fn(ctx);
    if (!module) {
        free_label_map(&map);
        destroy_cj_ctx(ctx);
        return false;
    }

    /* The entry point is the whole module (starts at beginning) */
    void *entry = cj_resolve_label(ctx, module, entry_label);
    fn->jit_code = entry;
    fn->jit_ctx = ctx;

    free_label_map(&map);
    return true;
}

void lisa_jit_free(lisa_obj_function *fn) {
    if (fn->jit_code && fn->jit_ctx) {
        cj_ctx *ctx = (cj_ctx *)fn->jit_ctx;
        /* We need to find the cj_fn to destroy. The module base is stored
           in the ctx's executable_base. Cast it back to cj_fn. */
        if (ctx->executable_base) {
            destroy_cj_fn(ctx, (cj_fn)(void *)ctx->executable_base);
        }
        destroy_cj_ctx(ctx);
        fn->jit_code = NULL;
        fn->jit_ctx = NULL;
    }
}
