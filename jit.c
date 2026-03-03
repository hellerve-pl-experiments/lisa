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
#include "builder.h"
#pragma GCC diagnostic pop

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef lisa_value (*lisa_jit_fn)(lisa_vm *vm, lisa_obj_closure *closure,
                                  lisa_value *slots);

/* ===== Platform Register Definitions ===== */

#if defined(__x86_64__) || defined(_M_X64)

#define REG_VM       "rbx"
#define REG_SLOTS    "r12"
#define REG_CLOSURE  "r13"
#define REG_STKTOP   "r14"
#define REG_CONSTS   "r15"

#define REG_CACHE0   "r8"
#define REG_CACHE1   "r9"
#define REG_CACHE2   "r10"
#define REG_CACHE3   "r11"

#define REG_TMP1     "rax"
#define REG_TMP2     "rcx"
#define REG_TMP3     "rdx"
#define REG_TMP4     "rsi"
#define REG_TMP5     "rdi"
#define REG_CALLADDR "r10"

#define REG_ARG0     "rdi"
#define REG_ARG1     "rsi"
#define REG_ARG2     "rdx"
#define REG_ARG3     "rcx"
#define REG_RET      "rax"

#define EMIT_JEQ(ctx, label) cj_jz(ctx, label)
#define EMIT_JNE(ctx, label) cj_jnz(ctx, label)
#define EMIT_JLT(ctx, label) cj_jl(ctx, label)
#define EMIT_JLE(ctx, label) cj_jle(ctx, label)
#define EMIT_JGT(ctx, label) cj_jg(ctx, label)
#define EMIT_JGE(ctx, label) cj_jge(ctx, label)
#define EMIT_JMP(ctx, label) cj_jmp(ctx, label)
#define EMIT_JB(ctx, label)  cj_jb(ctx, label)

#elif defined(__aarch64__) || defined(_M_ARM64)

#define REG_VM       "x19"
#define REG_SLOTS    "x20"
#define REG_CLOSURE  "x21"
#define REG_STKTOP   "x22"
#define REG_CONSTS   "x23"

#define REG_CACHE0   "x10"
#define REG_CACHE1   "x11"
#define REG_CACHE2   "x12"
#define REG_CACHE3   "x13"

#define REG_TMP1     "x0"
#define REG_TMP2     "x1"
#define REG_TMP3     "x2"
#define REG_TMP4     "x3"
#define REG_TMP5     "x4"
#define REG_CALLADDR "x9"

#define REG_ARG0     "x0"
#define REG_ARG1     "x1"
#define REG_ARG2     "x2"
#define REG_ARG3     "x3"
#define REG_RET      "x0"

#define EMIT_JEQ(ctx, label) cj_beq(ctx, label)
#define EMIT_JNE(ctx, label) cj_bne(ctx, label)
#define EMIT_JLT(ctx, label) cj_blt(ctx, label)
#define EMIT_JLE(ctx, label) cj_ble(ctx, label)
#define EMIT_JGT(ctx, label) cj_bgt(ctx, label)
#define EMIT_JGE(ctx, label) cj_bge(ctx, label)
#define EMIT_JMP(ctx, label) cj_b(ctx, label)
#define EMIT_JB(ctx, label)  cj_bcc(ctx, label)

#endif

/* NaN-boxing constants */
#define TAG_INT_FULL (QNAN | TAG_INT) /* 0x7FFE000000000000 */
#define TAG_INT_HI   0x7FFE           /* top 16 bits of an integer value */
#define TAG_NONDBL   0x7FFC           /* minimum top-16 for any tagged value */

/* ===== Operand Helpers ===== */

static cj_operand reg(const char *name) { return cj_make_register(name); }
static cj_operand imm(uint64_t val)     { return cj_make_constant(val); }
static cj_operand mem(const char *base, int32_t disp) {
    return cj_make_memory(base, NULL, 1, disp);
}

/* ===== Low-level Emit Helpers ===== */

static void emit_load_imm64(cj_ctx *ctx, const char *dst, uint64_t value) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, reg(dst), imm(value));
#elif defined(__aarch64__) || defined(_M_ARM64)
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

static void emit_load64(cj_ctx *ctx, const char *dst, const char *base, int32_t disp) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, reg(dst), mem(base, disp));
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* ARM64 LDR unsigned offset max = 4095*8 = 32760. */
    if (disp >= 0 && disp <= 32760 && (disp % 8) == 0) {
        cj_ldr(ctx, reg(dst), mem(base, disp));
    } else if (disp < 0 && (-disp) <= 4095) {
        /* Small negative offset: SUB then LDR */
        cj_mov(ctx, reg(dst), reg(base));
        cj_sub(ctx, reg(dst), imm((uint64_t)(uint32_t)(-disp)));
        cj_ldr(ctx, reg(dst), mem(dst, 0));
    } else {
        /* Large offset: load into dst, add base, load */
        if (disp >= 0) {
            emit_load_imm64(ctx, dst, (uint64_t)(uint32_t)disp);
            cj_add(ctx, reg(dst), reg(base));
        } else {
            cj_mov(ctx, reg(dst), reg(base));
            emit_load_imm64(ctx, REG_TMP4, (uint64_t)(uint32_t)(-disp));
            cj_sub(ctx, reg(dst), reg(REG_TMP4));
        }
        cj_ldr(ctx, reg(dst), mem(dst, 0));
    }
#endif
}

static void emit_store64(cj_ctx *ctx, const char *src, const char *base, int32_t disp) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_mov(ctx, mem(base, disp), reg(src));
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (disp >= 0 && disp <= 32760 && (disp % 8) == 0) {
        cj_str(ctx, reg(src), mem(base, disp));
    } else if (disp < 0 && (-disp) <= 4095) {
        cj_mov(ctx, reg(REG_TMP4), reg(base));
        cj_sub(ctx, reg(REG_TMP4), imm((uint64_t)(uint32_t)(-disp)));
        cj_str(ctx, reg(src), mem(REG_TMP4, 0));
    } else {
        if (disp >= 0) {
            emit_load_imm64(ctx, REG_TMP4, (uint64_t)(uint32_t)disp);
            cj_add(ctx, reg(REG_TMP4), reg(base));
        } else {
            cj_mov(ctx, reg(REG_TMP4), reg(base));
            emit_load_imm64(ctx, REG_TMP5, (uint64_t)(uint32_t)(-disp));
            cj_sub(ctx, reg(REG_TMP4), reg(REG_TMP5));
        }
        cj_str(ctx, reg(src), mem(REG_TMP4, 0));
    }
#endif
}

static void emit_call_abs(cj_ctx *ctx, void *fn_ptr) {
    emit_load_imm64(ctx, REG_CALLADDR, (uint64_t)(uintptr_t)fn_ptr);
#if defined(__x86_64__) || defined(_M_X64)
    cj_call(ctx, reg(REG_CALLADDR));
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_blr(ctx, reg(REG_CALLADDR));
#endif
}

static void emit_pop(cj_ctx *ctx, const char *dst_reg) {
    cj_sub(ctx, reg(REG_STKTOP), imm(8));
    emit_load64(ctx, dst_reg, REG_STKTOP, 0);
}

static void emit_peek(cj_ctx *ctx, const char *dst_reg, int distance) {
    int32_t offset = (int32_t)(-8 * (1 + distance));
    emit_load64(ctx, dst_reg, REG_STKTOP, offset);
}

static void emit_sync_stack_top(cj_ctx *ctx) {
    emit_store64(ctx, REG_STKTOP, REG_VM,
                 (int32_t)offsetof(lisa_vm, stack_top));
}

static void emit_reload_stack_top(cj_ctx *ctx) {
    emit_load64(ctx, REG_STKTOP, REG_VM,
                (int32_t)offsetof(lisa_vm, stack_top));
}

/* ===== Platform-Specific Shift Helpers ===== */

/* Logical shift right: dst = src >> shift (zero-extend) */
static void emit_lsr_imm(cj_ctx *ctx, const char *dst, const char *src, int shift) {
    if (strcmp(dst, src) != 0)
        cj_mov(ctx, reg(dst), reg(src));
    cj_builder_shr(ctx, reg(dst), shift);
}

/* Logical shift left: dst = src << shift */
static void emit_lsl_imm(cj_ctx *ctx, const char *dst, const char *src, int shift) {
    if (strcmp(dst, src) != 0)
        cj_mov(ctx, reg(dst), reg(src));
    cj_builder_shl(ctx, reg(dst), shift);
}

/* Clear top 16 bits: r &= 0x0000FFFFFFFFFFFF (unsigned 48-bit payload) */
static void emit_mask48(cj_ctx *ctx, const char *r) {
    cj_builder_shl(ctx, reg(r), 16);
    cj_builder_shr(ctx, reg(r), 16);
}

/* Sign-extend from bit 47: r = sign_extend_48(r) */
static void emit_sign_extend48(cj_ctx *ctx, const char *r) {
    cj_builder_shl(ctx, reg(r), 16);
    cj_builder_sar(ctx, reg(r), 16);
}

/* OR dst |= src */
static void emit_or(cj_ctx *ctx, const char *dst, const char *src) {
    cj_builder_or(ctx, reg(dst), reg(src));
}

/* Re-tag a masked 48-bit payload as an integer. Uses REG_TMP1 as scratch. */
static void emit_retag_int(cj_ctx *ctx, const char *r) {
    emit_load_imm64(ctx, REG_TMP1, TAG_INT_FULL);
    emit_or(ctx, r, REG_TMP1);
}

/* ARM64 CSET defines removed — now using cj_builder_cset from builder.h */

/* ===== Register Cache ===== */

#define MAX_CACHE 4

typedef struct {
    int depth;
    const char *regs[MAX_CACHE];
} reg_cache_t;

static void cache_init(reg_cache_t *cache) {
    cache->depth = 0;
    cache->regs[0] = REG_CACHE0;
    cache->regs[1] = REG_CACHE1;
    cache->regs[2] = REG_CACHE2;
    cache->regs[3] = REG_CACHE3;
}

static void cache_flush(cj_ctx *ctx, reg_cache_t *cache) {
    for (int i = 0; i < cache->depth; i++)
        emit_store64(ctx, cache->regs[i], REG_STKTOP, i * 8);
    if (cache->depth > 0)
        cj_add(ctx, reg(REG_STKTOP), imm((uint64_t)cache->depth * 8));
    cache->depth = 0;
}

/* Flush all entries except the top `keep` entries.
   Shifts kept entries down to regs[0..keep-1]. */
static void cache_flush_to(cj_ctx *ctx, reg_cache_t *cache, int keep) {
    if (keep >= cache->depth) return;
    int n = cache->depth - keep;
    for (int i = 0; i < n; i++)
        emit_store64(ctx, cache->regs[i], REG_STKTOP, i * 8);
    if (n > 0)
        cj_add(ctx, reg(REG_STKTOP), imm((uint64_t)n * 8));
    for (int i = 0; i < keep; i++)
        cj_mov(ctx, reg(cache->regs[i]), reg(cache->regs[n + i]));
    cache->depth = keep;
}

static void cache_push(cj_ctx *ctx, reg_cache_t *cache, const char *src) {
    if (cache->depth >= MAX_CACHE)
        cache_flush(ctx, cache);
    if (strcmp(src, cache->regs[cache->depth]) != 0)
        cj_mov(ctx, reg(cache->regs[cache->depth]), reg(src));
    cache->depth++;
}

/* Pop top value. Returns register name holding the value.
   If cache empty, loads from memory stack into REG_TMP1. */
static const char *cache_pop(cj_ctx *ctx, reg_cache_t *cache) {
    if (cache->depth > 0) {
        cache->depth--;
        return cache->regs[cache->depth];
    }
    cj_sub(ctx, reg(REG_STKTOP), imm(8));
    emit_load64(ctx, REG_TMP1, REG_STKTOP, 0);
    return REG_TMP1;
}

/* ===== Prologue / Epilogue ===== */

static void emit_prologue(cj_ctx *ctx) {
#if defined(__x86_64__) || defined(_M_X64)
    cj_push(ctx, reg("rbp"));
    cj_mov(ctx, reg("rbp"), reg("rsp"));
    cj_push(ctx, reg("rbx"));
    cj_push(ctx, reg("r12"));
    cj_push(ctx, reg("r13"));
    cj_push(ctx, reg("r14"));
    cj_push(ctx, reg("r15"));
    cj_sub(ctx, reg("rsp"), imm(8)); /* 16-byte alignment */

    cj_mov(ctx, reg(REG_VM), reg("rdi"));
    cj_mov(ctx, reg(REG_CLOSURE), reg("rsi"));
    cj_mov(ctx, reg(REG_SLOTS), reg("rdx"));
#elif defined(__aarch64__) || defined(_M_ARM64)
    /* cj_stp ignores pre-indexed mode, so manually adjust SP */
    cj_sub(ctx, reg("sp"), imm(80));
    cj_stp(ctx, reg("x29"), reg("x30"), mem("sp", 0));
    /* cj_mov(x29, sp) generates ORR x29,XZR,XZR=0 (backend bug:
       reg 31 is XZR in ORR, not SP). Use raw ADD x29, sp, #0. */
    cj_add_u32(ctx, 0x910003FD); /* ADD x29, sp, #0 */
    cj_stp(ctx, reg("x19"), reg("x20"), mem("sp", 16));
    cj_stp(ctx, reg("x21"), reg("x22"), mem("sp", 32));
    cj_str(ctx, reg("x23"), mem("sp", 48));

    cj_mov(ctx, reg(REG_VM), reg("x0"));
    cj_mov(ctx, reg(REG_CLOSURE), reg("x1"));
    cj_mov(ctx, reg(REG_SLOTS), reg("x2"));
#endif
    emit_reload_stack_top(ctx);

    /* Load constants pointer: closure->function->chunk.constants.values */
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
    cj_ldp(ctx, reg("x29"), reg("x30"), mem("sp", 0));
    cj_add(ctx, reg("sp"), imm(80));
    cj_ret(ctx);
#endif
}

/* ===== Inline Type-Check Helpers ===== */

/* Check if val_reg is an integer. Jumps to fail_label if not.
   Clobbers REG_TMP1 (and REG_TMP2 on ARM64). */
static void emit_int_type_check(cj_ctx *ctx, const char *val_reg, cj_label fail_label) {
    emit_lsr_imm(ctx, REG_TMP1, val_reg, 48);
#if defined(__x86_64__) || defined(_M_X64)
    cj_cmp(ctx, reg(REG_TMP1), imm(TAG_INT_HI));
    cj_jnz(ctx, fail_label);
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_movz(ctx, reg(REG_TMP2), imm(TAG_INT_HI));
    cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
    cj_bne(ctx, fail_label);
#endif
}

/* Check if val_reg is NOT a double (top 16 bits >= 0x7FFC).
   Jumps to fail_label if it IS a double.
   Clobbers REG_TMP1 (and REG_TMP2 on ARM64). */
static void emit_non_double_check(cj_ctx *ctx, const char *val_reg, cj_label fail_label) {
    emit_lsr_imm(ctx, REG_TMP1, val_reg, 48);
#if defined(__x86_64__) || defined(_M_X64)
    cj_cmp(ctx, reg(REG_TMP1), imm(TAG_NONDBL));
    cj_jb(ctx, fail_label);
#elif defined(__aarch64__) || defined(_M_ARM64)
    cj_movz(ctx, reg(REG_TMP2), imm(TAG_NONDBL));
    cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
    cj_bcc(ctx, fail_label);
#endif
}

/* Emit boolean result (LISA_TRUE or LISA_FALSE) from comparison flags.
   On x86: uses REG_TMP1 = "rax", writes setcc into "al".
   On ARM64: uses CSET into REG_TMP1, then OR with LISA_FALSE.
   Result is left in REG_TMP1. */
typedef enum { CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE } cmp_kind;

static void emit_bool_from_flags(cj_ctx *ctx, cmp_kind kind) {
    /* Map cmp_kind to cj_condition */
    cj_condition cond;
    switch (kind) {
    case CMP_LT: cond = CJ_COND_L;  break;
    case CMP_LE: cond = CJ_COND_LE; break;
    case CMP_GT: cond = CJ_COND_G;  break;
    case CMP_GE: cond = CJ_COND_GE; break;
    case CMP_EQ: cond = CJ_COND_Z;  break;
    case CMP_NE: cond = CJ_COND_NZ; break;
    }
    /* CSET: TMP1 = 0 or 1 from flags (reads flags, then MOVZX/CSINC) */
    cj_builder_cset(ctx, reg(REG_TMP1), cond);
    /* OR with LISA_FALSE to produce LISA_FALSE or LISA_TRUE */
    emit_load_imm64(ctx, REG_TMP2, LISA_FALSE);
    emit_or(ctx, REG_TMP1, REG_TMP2);
}

/* ===== Call Helpers (flush-aware) ===== */

static void emit_call_vm_int(cj_ctx *ctx, void *fn_ptr, int int_arg) {
    emit_sync_stack_top(ctx);
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
    emit_load_imm64(ctx, REG_ARG1, (uint64_t)(uint32_t)int_arg);
    emit_call_abs(ctx, fn_ptr);
    emit_reload_stack_top(ctx);
}

/* ===== Inline Fast-Path Generators ===== */

typedef enum { ARITH_ADD, ARITH_SUB, ARITH_MUL } arith_op;

static void emit_arith_compute(cj_ctx *ctx, const char *dst, const char *src, arith_op op) {
    switch (op) {
    case ARITH_ADD: cj_add(ctx, reg(dst), reg(src)); break;
    case ARITH_SUB: cj_sub(ctx, reg(dst), reg(src)); break;
    case ARITH_MUL:
        cj_builder_mul(ctx, reg(dst), reg(src));
        break;
    }
}

/* Emit inline integer fast path for ADD/SUB/MUL.
   Expects cache->depth >= 2 and cache already flushed to depth 2.
   After this, cache->depth = 1, result in cache->regs[0]. */
static void emit_binop_int_fast(cj_ctx *ctx, reg_cache_t *cache,
                                 arith_op op, void *slow_fn) {
    const char *a_reg = cache->regs[0];
    const char *b_reg = cache->regs[1];

    cj_label slow = cj_create_label(ctx);
    cj_label done = cj_create_label(ctx);

    /* Type-check both operands (non-destructive: only REG_TMP1/TMP2 clobbered) */
    emit_int_type_check(ctx, a_reg, slow);
    emit_int_type_check(ctx, b_reg, slow);

    /* Fast path: extract payloads, compute, mask, retag */
    emit_mask48(ctx, a_reg);
    emit_mask48(ctx, b_reg);
    emit_arith_compute(ctx, a_reg, b_reg, op);
    emit_mask48(ctx, a_reg);
    emit_retag_int(ctx, a_reg);

    EMIT_JMP(ctx, done);

    cj_mark_label(ctx, slow);
    /* a and b are unchanged (type checks non-destructive). */
    emit_sync_stack_top(ctx);
    cj_mov(ctx, reg(REG_ARG2), reg(b_reg));
    cj_mov(ctx, reg(REG_ARG1), reg(a_reg));
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
    emit_call_abs(ctx, slow_fn);
    emit_reload_stack_top(ctx);
    cj_mov(ctx, reg(cache->regs[0]), reg(REG_RET));

    cj_mark_label(ctx, done);
    cache->depth = 1;
}

/* Emit inline integer fast path for comparison ops (LT/LE/GT/GE).
   Uses signed comparison of shifted payloads.
   After this, cache->depth = 1, result in cache->regs[0]. */
static void emit_cmpop_int_fast(cj_ctx *ctx, reg_cache_t *cache,
                                 cmp_kind kind, void *slow_fn) {
    const char *a_reg = cache->regs[0];
    const char *b_reg = cache->regs[1];

    cj_label slow = cj_create_label(ctx);
    cj_label done = cj_create_label(ctx);

    emit_int_type_check(ctx, a_reg, slow);
    emit_int_type_check(ctx, b_reg, slow);

    /* Shift left by 16 to align sign bit at bit 63 for signed compare */
    emit_lsl_imm(ctx, REG_TMP1, a_reg, 16);
    emit_lsl_imm(ctx, REG_TMP2, b_reg, 16);
    cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));

    emit_bool_from_flags(ctx, kind);
    cj_mov(ctx, reg(cache->regs[0]), reg(REG_TMP1));

    EMIT_JMP(ctx, done);

    cj_mark_label(ctx, slow);
    emit_sync_stack_top(ctx);
    cj_mov(ctx, reg(REG_ARG2), reg(b_reg));
    cj_mov(ctx, reg(REG_ARG1), reg(a_reg));
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
    emit_call_abs(ctx, slow_fn);
    emit_reload_stack_top(ctx);
    cj_mov(ctx, reg(cache->regs[0]), reg(REG_RET));

    cj_mark_label(ctx, done);
    cache->depth = 1;
}

/* Emit inline bitwise equality fast path (correct for int, bool, nil, interned strings).
   Falls through to helper for doubles.
   After this, cache->depth = 1, result in cache->regs[0]. */
static void emit_eqop_fast(cj_ctx *ctx, reg_cache_t *cache,
                            cmp_kind kind, void *slow_fn) {
    const char *a_reg = cache->regs[0];
    const char *b_reg = cache->regs[1];

    cj_label slow = cj_create_label(ctx);
    cj_label done = cj_create_label(ctx);

    /* Check neither is a double: top 16 bits >= 0x7FFC */
    emit_non_double_check(ctx, a_reg, slow);
    emit_non_double_check(ctx, b_reg, slow);

    /* Both tagged: bitwise compare */
    cj_cmp(ctx, reg(a_reg), reg(b_reg));
    emit_bool_from_flags(ctx, kind);
    cj_mov(ctx, reg(cache->regs[0]), reg(REG_TMP1));

    EMIT_JMP(ctx, done);

    cj_mark_label(ctx, slow);
    emit_sync_stack_top(ctx);
    cj_mov(ctx, reg(REG_ARG2), reg(b_reg));
    cj_mov(ctx, reg(REG_ARG1), reg(a_reg));
    cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
    emit_call_abs(ctx, slow_fn);
    emit_reload_stack_top(ctx);
    cj_mov(ctx, reg(cache->regs[0]), reg(REG_RET));

    cj_mark_label(ctx, done);
    cache->depth = 1;
}

/* Helper: emit a full binary op with cache. Handles both cached and non-cached cases. */
static void emit_binop(cj_ctx *ctx, reg_cache_t *cache,
                        arith_op op, void *slow_fn) {
    if (cache->depth >= 2) {
        cache_flush_to(ctx, cache, 2);
        emit_binop_int_fast(ctx, cache, op, slow_fn);
    } else {
        cache_flush(ctx, cache);
        emit_pop(ctx, REG_TMP3);  /* b */
        emit_pop(ctx, REG_TMP2);  /* a */
        emit_sync_stack_top(ctx);
        cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
        cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
        cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
        emit_call_abs(ctx, slow_fn);
        emit_reload_stack_top(ctx);
        cache_push(ctx, cache, REG_RET);
    }
}

static void emit_cmpop(cj_ctx *ctx, reg_cache_t *cache,
                        cmp_kind kind, void *slow_fn) {
    if (cache->depth >= 2) {
        cache_flush_to(ctx, cache, 2);
        emit_cmpop_int_fast(ctx, cache, kind, slow_fn);
    } else {
        cache_flush(ctx, cache);
        emit_pop(ctx, REG_TMP3);
        emit_pop(ctx, REG_TMP2);
        emit_sync_stack_top(ctx);
        cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
        cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
        cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
        emit_call_abs(ctx, slow_fn);
        emit_reload_stack_top(ctx);
        cache_push(ctx, cache, REG_RET);
    }
}

static void emit_eqop(cj_ctx *ctx, reg_cache_t *cache,
                       cmp_kind kind, void *slow_fn) {
    if (cache->depth >= 2) {
        cache_flush_to(ctx, cache, 2);
        emit_eqop_fast(ctx, cache, kind, slow_fn);
    } else {
        cache_flush(ctx, cache);
        emit_pop(ctx, REG_TMP3);
        emit_pop(ctx, REG_TMP2);
        emit_sync_stack_top(ctx);
        cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
        cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
        cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
        emit_call_abs(ctx, slow_fn);
        emit_reload_stack_top(ctx);
        cache_push(ctx, cache, REG_RET);
    }
}

/* ===== Bytecode Scanner ===== */

typedef struct {
    cj_label *labels;
    bool *is_target;
    int code_len;
} label_map;

static void scan_branch_targets(lisa_chunk *chunk, label_map *map, cj_ctx *ctx) {
    int len = chunk->count;
    map->code_len = len;
    map->is_target = calloc((size_t)len, sizeof(bool));
    map->labels = calloc((size_t)len, sizeof(cj_label));

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
            lisa_obj_function *cfn = AS_FUNCTION(chunk->constants.values[fn_idx]);
            i += 2 + cfn->upvalue_count * 2;
            break;
        }
        case OP_CONSTANT: case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_GET_UPVALUE: case OP_SET_UPVALUE:
        case OP_GET_GLOBAL: case OP_DEF_GLOBAL:
        case OP_CALL: case OP_TAIL_CALL:
        case OP_LIST: case OP_PRINTLN:
        case OP_CLOSE_UPVALUES_AT:
            i += 2;
            break;
        default:
            i += 1;
            break;
        }
    }

    for (i = 0; i < len; i++) {
        if (map->is_target[i])
            map->labels[i] = cj_create_label(ctx);
    }
}

static void free_label_map(label_map *map) {
    free(map->labels);
    free(map->is_target);
}

/* ===== Main JIT Compilation ===== */

bool lisa_jit_compile(lisa_vm *vm, lisa_obj_function *fn) {
    (void)vm;

    if (fn->jit_code) return true;

    cj_ctx *ctx = create_cj_ctx();
    if (!ctx) return false;

    lisa_chunk *chunk = &fn->chunk;

    label_map map;
    scan_branch_targets(chunk, &map, ctx);

    cj_label entry_label = cj_create_label(ctx);

    cj_mark_label(ctx, entry_label);
    emit_prologue(ctx);

    /* body_label: target for self-tail-call loop (after prologue) */
    cj_label body_label = cj_create_label(ctx);
    cj_mark_label(ctx, body_label);

    reg_cache_t cache;
    cache_init(&cache);

    int i = 0;
    while (i < chunk->count) {
        /* At branch targets, ensure cache is empty */
        if (map.is_target[i]) {
            cache_flush(ctx, &cache);
            cj_mark_label(ctx, map.labels[i]);
        }

        uint8_t op = chunk->code[i];
        switch (op) {

        case OP_CONSTANT: {
            uint8_t idx = chunk->code[i + 1];
            emit_load64(ctx, REG_TMP1, REG_CONSTS, (int32_t)(idx * 8));
            cache_push(ctx, &cache, REG_TMP1);
            i += 2;
            break;
        }

        case OP_NIL:
            emit_load_imm64(ctx, REG_TMP1, LISA_NIL);
            cache_push(ctx, &cache, REG_TMP1);
            i += 1;
            break;

        case OP_TRUE:
            emit_load_imm64(ctx, REG_TMP1, LISA_TRUE);
            cache_push(ctx, &cache, REG_TMP1);
            i += 1;
            break;

        case OP_FALSE:
            emit_load_imm64(ctx, REG_TMP1, LISA_FALSE);
            cache_push(ctx, &cache, REG_TMP1);
            i += 1;
            break;

        case OP_POP:
            if (cache.depth > 0)
                cache.depth--;
            else
                cj_sub(ctx, reg(REG_STKTOP), imm(8));
            i += 1;
            break;

        case OP_GET_LOCAL: {
            uint8_t slot = chunk->code[i + 1];
            /* Flush cached values to memory first so that locals
             * created by let/def (pushed via OP_CONSTANT) are visible
             * at their slot positions in the frame. */
            cache_flush(ctx, &cache);
            emit_load64(ctx, REG_TMP1, REG_SLOTS, (int32_t)(slot * 8));
            cache_push(ctx, &cache, REG_TMP1);
            i += 2;
            break;
        }

        case OP_SET_LOCAL: {
            uint8_t slot = chunk->code[i + 1];
            /* Flush first so stale cached values don't later overwrite
             * the slot when the cache is flushed by a subsequent op. */
            cache_flush(ctx, &cache);
            emit_peek(ctx, REG_TMP1, 0);
            emit_store64(ctx, REG_TMP1, REG_SLOTS, (int32_t)(slot * 8));
            i += 2;
            break;
        }

        case OP_GET_UPVALUE: {
            uint8_t slot = chunk->code[i + 1];
            emit_load64(ctx, REG_TMP1, REG_CLOSURE,
                        (int32_t)offsetof(lisa_obj_closure, upvalues));
            emit_load64(ctx, REG_TMP1, REG_TMP1, (int32_t)(slot * 8));
            emit_load64(ctx, REG_TMP1, REG_TMP1,
                        (int32_t)offsetof(lisa_obj_upvalue, location));
            emit_load64(ctx, REG_TMP1, REG_TMP1, 0);
            cache_push(ctx, &cache, REG_TMP1);
            i += 2;
            break;
        }

        case OP_SET_UPVALUE: {
            uint8_t slot = chunk->code[i + 1];
            const char *val;
            if (cache.depth > 0) {
                val = cache.regs[cache.depth - 1];
            } else {
                emit_peek(ctx, REG_TMP3, 0);
                val = REG_TMP3;
            }
            emit_load64(ctx, REG_TMP2, REG_CLOSURE,
                        (int32_t)offsetof(lisa_obj_closure, upvalues));
            emit_load64(ctx, REG_TMP2, REG_TMP2, (int32_t)(slot * 8));
            emit_load64(ctx, REG_TMP2, REG_TMP2,
                        (int32_t)offsetof(lisa_obj_upvalue, location));
            emit_store64(ctx, val, REG_TMP2, 0);
            i += 2;
            break;
        }

        case OP_GET_GLOBAL: {
            uint8_t idx = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_call_vm_int(ctx, (void *)lisa_jit_get_global, idx);
            cache_push(ctx, &cache, REG_RET);
            i += 2;
            break;
        }

        case OP_DEF_GLOBAL: {
            uint8_t idx = chunk->code[i + 1];
            const char *val = cache_pop(ctx, &cache);
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG2), reg(val));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_load_imm64(ctx, REG_ARG1, (uint64_t)(uint32_t)idx);
            emit_call_abs(ctx, (void *)lisa_jit_def_global);
            emit_reload_stack_top(ctx);
            i += 2;
            break;
        }

        /* --- Arithmetic with inline int fast paths --- */

        case OP_ADD:
            emit_binop(ctx, &cache, ARITH_ADD, (void *)lisa_jit_add);
            i += 1;
            break;

        case OP_SUB:
            emit_binop(ctx, &cache, ARITH_SUB, (void *)lisa_jit_sub);
            i += 1;
            break;

        case OP_MUL:
            emit_binop(ctx, &cache, ARITH_MUL, (void *)lisa_jit_mul);
            i += 1;
            break;

        case OP_DIV: {
            /* Always use helper (produces doubles / edge cases) */
            cache_flush(ctx, &cache);
            emit_pop(ctx, REG_TMP3);
            emit_pop(ctx, REG_TMP2);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_div);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);
            i += 1;
            break;
        }

        case OP_MOD: {
            cache_flush(ctx, &cache);
            emit_pop(ctx, REG_TMP3);
            emit_pop(ctx, REG_TMP2);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_mod);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);
            i += 1;
            break;
        }

        case OP_NEGATE: {
            if (cache.depth >= 1) {
                cache_flush_to(ctx, &cache, 1);
                const char *a_reg = cache.regs[0];
                cj_label slow = cj_create_label(ctx);
                cj_label done = cj_create_label(ctx);

                emit_int_type_check(ctx, a_reg, slow);

                /* Extract signed payload, negate, mask, retag */
                emit_sign_extend48(ctx, a_reg);
                cj_builder_neg(ctx, reg(a_reg));
                emit_mask48(ctx, a_reg);
                emit_retag_int(ctx, a_reg);

                EMIT_JMP(ctx, done);

                cj_mark_label(ctx, slow);
                emit_sync_stack_top(ctx);
                cj_mov(ctx, reg(REG_ARG1), reg(a_reg));
                cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
                emit_call_abs(ctx, (void *)lisa_jit_negate);
                emit_reload_stack_top(ctx);
                cj_mov(ctx, reg(cache.regs[0]), reg(REG_RET));

                cj_mark_label(ctx, done);
                cache.depth = 1;
            } else {
                cache_flush(ctx, &cache);
                emit_pop(ctx, REG_TMP2);
                emit_sync_stack_top(ctx);
                cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
                cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
                emit_call_abs(ctx, (void *)lisa_jit_negate);
                emit_reload_stack_top(ctx);
                cache_push(ctx, &cache, REG_RET);
            }
            i += 1;
            break;
        }

        /* --- Comparisons with inline int fast paths --- */

        case OP_LESS:
            emit_cmpop(ctx, &cache, CMP_LT, (void *)lisa_jit_less);
            i += 1;
            break;

        case OP_LESS_EQUAL:
            emit_cmpop(ctx, &cache, CMP_LE, (void *)lisa_jit_less_equal);
            i += 1;
            break;

        case OP_GREATER:
            emit_cmpop(ctx, &cache, CMP_GT, (void *)lisa_jit_greater);
            i += 1;
            break;

        case OP_GREATER_EQUAL:
            emit_cmpop(ctx, &cache, CMP_GE, (void *)lisa_jit_greater_equal);
            i += 1;
            break;

        case OP_EQUAL:
            emit_eqop(ctx, &cache, CMP_EQ, (void *)lisa_jit_equal);
            i += 1;
            break;

        case OP_NOT_EQUAL:
            emit_eqop(ctx, &cache, CMP_NE, (void *)lisa_jit_not_equal);
            i += 1;
            break;

        /* --- NOT (inline falsey check) --- */

        case OP_NOT: {
            const char *val = cache_pop(ctx, &cache);

            cj_label is_falsey = cj_create_label(ctx);
            cj_label done_not = cj_create_label(ctx);

            emit_load_imm64(ctx, REG_TMP2, LISA_NIL);
            cj_cmp(ctx, reg(val), reg(REG_TMP2));
            EMIT_JEQ(ctx, is_falsey);

            emit_load_imm64(ctx, REG_TMP2, LISA_FALSE);
            cj_cmp(ctx, reg(val), reg(REG_TMP2));
            EMIT_JEQ(ctx, is_falsey);

            /* Truthy → push false */
            emit_load_imm64(ctx, REG_TMP1, LISA_FALSE);
            EMIT_JMP(ctx, done_not);

            cj_mark_label(ctx, is_falsey);
            emit_load_imm64(ctx, REG_TMP1, LISA_TRUE);

            cj_mark_label(ctx, done_not);
            cache_push(ctx, &cache, REG_TMP1);
            i += 1;
            break;
        }

        /* --- Control flow --- */

        case OP_JUMP: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 + offset;
            cache_flush(ctx, &cache);
            EMIT_JMP(ctx, map.labels[target]);
            i += 3;
            break;
        }

        case OP_JUMP_IF_FALSE: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 + offset;

            const char *val = cache_pop(ctx, &cache);
            cache_flush(ctx, &cache);

            /* Inline falsey check */
            emit_load_imm64(ctx, REG_TMP2, LISA_NIL);
            cj_cmp(ctx, reg(val), reg(REG_TMP2));
            EMIT_JEQ(ctx, map.labels[target]);

            emit_load_imm64(ctx, REG_TMP2, LISA_FALSE);
            cj_cmp(ctx, reg(val), reg(REG_TMP2));
            EMIT_JEQ(ctx, map.labels[target]);

            i += 3;
            break;
        }

        case OP_LOOP: {
            uint8_t lo = chunk->code[i + 1];
            uint8_t hi = chunk->code[i + 2];
            uint16_t offset = (uint16_t)(lo | (hi << 8));
            int target = i + 3 - offset;
            cache_flush(ctx, &cache);
            EMIT_JMP(ctx, map.labels[target]);
            i += 3;
            break;
        }

        /* --- Function ops --- */

        case OP_CLOSURE: {
            uint8_t fn_idx = chunk->code[i + 1];
            lisa_obj_function *closure_fn = AS_FUNCTION(chunk->constants.values[fn_idx]);
            int uv_count = closure_fn->upvalue_count;
            uint8_t *uv_ip = &chunk->code[i + 2];

            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_CLOSURE));
            emit_load_imm64(ctx, REG_ARG2, (uint64_t)(uintptr_t)closure_fn);
            emit_load_imm64(ctx, REG_ARG3, (uint64_t)(uintptr_t)uv_ip);
            emit_call_abs(ctx, (void *)lisa_jit_make_closure);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);

            i += 2 + uv_count * 2;
            break;
        }

        case OP_CALL: {
            int argc = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_call_vm_int(ctx, (void *)lisa_jit_call_helper, argc);
            /* Result already pushed to memory stack by helper */
            i += 2;
            break;
        }

        case OP_TAIL_CALL: {
            int argc = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);

            /* Runtime self-call check: compare callee with current closure.
               Callee on stack is NaN-boxed (QNAN|TAG_OBJ|ptr), but REG_CLOSURE
               is a raw pointer. NaN-box REG_CLOSURE into TMP2 for comparison. */
            cj_label not_self = cj_create_label(ctx);
            int32_t callee_off = (int32_t)(-8 * (argc + 1));
            emit_load64(ctx, REG_TMP1, REG_STKTOP, callee_off);
            emit_load_imm64(ctx, REG_TMP2, QNAN | TAG_OBJ);
            emit_or(ctx, REG_TMP2, REG_CLOSURE);
            cj_cmp(ctx, reg(REG_TMP1), reg(REG_TMP2));
            EMIT_JNE(ctx, not_self);

            /* Self-call: move args to slots, reset stack, jump to body */
            for (int a = 0; a < argc; a++) {
                int32_t src_off = (int32_t)(-8 * (argc - a));
                emit_load64(ctx, REG_TMP1, REG_STKTOP, src_off);
                emit_store64(ctx, REG_TMP1, REG_SLOTS, (int32_t)((1 + a) * 8));
            }
            cj_mov(ctx, reg(REG_STKTOP), reg(REG_SLOTS));
            cj_add(ctx, reg(REG_STKTOP), imm((uint64_t)(argc + 1) * 8));
            emit_sync_stack_top(ctx);
            EMIT_JMP(ctx, body_label);

            /* Non-self tail call: return sentinel for trampoline */
            cj_mark_label(ctx, not_self);
            emit_load_imm64(ctx, REG_RET, LISA_TAIL_PENDING(argc));
            emit_epilogue(ctx);

            i += 2;
            break;
        }

        case OP_RETURN: {
            if (cache.depth > 0) {
                const char *ret_src = cache.regs[cache.depth - 1];
                cj_mov(ctx, reg(REG_RET), reg(ret_src));
                cache.depth--;
            } else {
                emit_pop(ctx, REG_RET);
            }
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            emit_epilogue(ctx);
            i += 1;
            break;
        }

        case OP_CLOSE_UPVALUE: {
            cache_flush(ctx, &cache);
            cj_sub(ctx, reg(REG_STKTOP), imm(8));
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_STKTOP));
            emit_call_abs(ctx, (void *)lisa_jit_close_upvalue);
            emit_reload_stack_top(ctx);
            i += 1;
            break;
        }

        case OP_CLOSE_UPVALUES_AT: {
            uint8_t slot = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            /* Compute &frame->slots[slot] */
            cj_mov(ctx, reg(REG_ARG1), reg(REG_SLOTS));
            if (slot > 0)
                cj_add(ctx, reg(REG_ARG1), imm((uint64_t)slot * 8));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_close_upvalue);
            emit_reload_stack_top(ctx);
            i += 2;
            break;
        }

        /* --- List ops (always helper) --- */

        case OP_CONS: {
            const char *cdr_reg = cache_pop(ctx, &cache);
            /* Need to save cdr since cache_pop of car might clobber REG_TMP1 */
            cj_mov(ctx, reg(REG_TMP3), reg(cdr_reg));
            const char *car_reg = cache_pop(ctx, &cache);
            cj_mov(ctx, reg(REG_TMP2), reg(car_reg));
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG2), reg(REG_TMP3));
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_cons);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);
            i += 1;
            break;
        }

        case OP_CAR: {
            const char *val = cache_pop(ctx, &cache);
            cj_mov(ctx, reg(REG_TMP2), reg(val));
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_car);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);
            i += 1;
            break;
        }

        case OP_CDR: {
            const char *val = cache_pop(ctx, &cache);
            cj_mov(ctx, reg(REG_TMP2), reg(val));
            cache_flush(ctx, &cache);
            emit_sync_stack_top(ctx);
            cj_mov(ctx, reg(REG_ARG1), reg(REG_TMP2));
            cj_mov(ctx, reg(REG_ARG0), reg(REG_VM));
            emit_call_abs(ctx, (void *)lisa_jit_cdr);
            emit_reload_stack_top(ctx);
            cache_push(ctx, &cache, REG_RET);
            i += 1;
            break;
        }

        case OP_LIST: {
            int n = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_call_vm_int(ctx, (void *)lisa_jit_list, n);
            cache_push(ctx, &cache, REG_RET);
            i += 2;
            break;
        }

        case OP_PRINTLN: {
            int argc = chunk->code[i + 1];
            cache_flush(ctx, &cache);
            emit_call_vm_int(ctx, (void *)lisa_jit_println, argc);
            cache_push(ctx, &cache, REG_RET);
            i += 2;
            break;
        }

        default:
            fprintf(stderr, "JIT: unsupported opcode %d at offset %d\n", op, i);
            free_label_map(&map);
            destroy_cj_ctx(ctx);
            return false;
        }
    }

    cj_fn module = create_cj_fn(ctx);
    if (!module) {
        free_label_map(&map);
        destroy_cj_ctx(ctx);
        return false;
    }

    void *entry = cj_resolve_label(ctx, module, entry_label);
    fn->jit_code = entry;
    fn->jit_ctx = ctx;




    free_label_map(&map);
    return true;
}

void lisa_jit_free(lisa_obj_function *fn) {
    if (fn->jit_code && fn->jit_ctx) {
        cj_ctx *ctx = (cj_ctx *)fn->jit_ctx;
        if (ctx->executable_base) {
            destroy_cj_fn(ctx, (cj_fn)(void *)ctx->executable_base);
        }
        destroy_cj_ctx(ctx);
        fn->jit_code = NULL;
        fn->jit_ctx = NULL;
    }
}
