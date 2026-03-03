#include "fiber.h"
#include "vm.h"
#include "jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#define FIBER_C_STACK_SIZE (64 * 1024) /* 64 KB per fiber */

/* ===== Context Switch (inline assembly) ===== */

#if defined(__aarch64__) || defined(_M_ARM64)

/*
 * lisa_fiber_switch(void **save_sp, void *restore_sp)
 *
 * Saves callee-saved registers (x19-x28, x29/fp, x30/lr) onto the current
 * stack, stores SP into *save_sp, loads SP from restore_sp, restores regs,
 * and returns (via restored x30).
 */
__attribute__((naked))
void lisa_fiber_switch(void **save_sp __attribute__((unused)),
                       void *restore_sp __attribute__((unused))) {
    __asm__ volatile(
        "stp x19, x20, [sp, #-16]!\n"
        "stp x21, x22, [sp, #-16]!\n"
        "stp x23, x24, [sp, #-16]!\n"
        "stp x25, x26, [sp, #-16]!\n"
        "stp x27, x28, [sp, #-16]!\n"
        "stp x29, x30, [sp, #-16]!\n"
        /* Save SP into *save_sp (x0) */
        "mov x2, sp\n"
        "str x2, [x0]\n"
        /* Load SP from restore_sp (x1) */
        "mov sp, x1\n"
        /* Restore callee-saved regs */
        "ldp x29, x30, [sp], #16\n"
        "ldp x27, x28, [sp], #16\n"
        "ldp x25, x26, [sp], #16\n"
        "ldp x23, x24, [sp], #16\n"
        "ldp x21, x22, [sp], #16\n"
        "ldp x19, x20, [sp], #16\n"
        "ret\n"
    );
}

#elif defined(__x86_64__) || defined(_M_X64)

__attribute__((naked))
void lisa_fiber_switch(void **save_sp __attribute__((unused)),
                       void *restore_sp __attribute__((unused))) {
    __asm__ volatile(
        /* Save callee-saved regs */
        "pushq %%rbp\n"
        "pushq %%rbx\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"
        /* Save RSP into *save_sp (rdi) */
        "movq %%rsp, (%%rdi)\n"
        /* Load RSP from restore_sp (rsi) */
        "movq %%rsi, %%rsp\n"
        /* Restore callee-saved regs */
        "popq %%r15\n"
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%rbx\n"
        "popq %%rbp\n"
        "retq\n"
    );
}

#endif

/* ===== C Stack Allocation ===== */

static void *alloc_c_stack(size_t *out_size) {
#if defined(__unix__) || defined(__APPLE__)
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    /* Round up to page boundary, add one guard page */
    size_t stack_size = (FIBER_C_STACK_SIZE + (size_t)page_size - 1)
                        & ~((size_t)page_size - 1);
    size_t total = stack_size + (size_t)page_size; /* guard page at bottom */

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;

    /* Guard page at the bottom (low address) */
    mprotect(base, (size_t)page_size, PROT_NONE);

    *out_size = total;
    return base;
#else
    size_t total = FIBER_C_STACK_SIZE;
    void *base = malloc(total);
    *out_size = total;
    return base;
#endif
}

static void free_c_stack(void *base, size_t size) {
    if (!base) return;
#if defined(__unix__) || defined(__APPLE__)
    munmap(base, size);
#else
    (void)size;
    free(base);
#endif
}

/* ===== Fiber Trampoline ===== */

/*
 * Global VM pointer set by the scheduler before switching to a new fiber.
 * Safe because fibers are cooperative (no preemption).
 */
static lisa_vm *g_trampoline_vm;

static void fiber_trampoline(void);

/*
 * fiber_trampoline_entry: normal C function that fiber_switch "returns" into
 * when a fiber starts for the first time. Reads the VM pointer from the global
 * and calls the real trampoline.
 */
static void fiber_trampoline_entry(void) {
    fiber_trampoline();
    /* Should never return */
    __builtin_unreachable();
}

/*
 * fiber_trampoline: runs the fiber's entry closure to completion.
 * Called on the fiber's own C stack.
 */
static void fiber_trampoline(void) {
    lisa_vm *vm = g_trampoline_vm;
    lisa_fiber *fiber = vm->current_fiber;
    lisa_obj_closure *closure = fiber->entry;
    int argc = fiber->argc;

    /* The fiber's Lisa stack already has [closure, arg0, arg1, ...].
       Set up the call via the public call_value API. */
    if (!lisa_call_value(vm, LISA_OBJ(closure), argc)) {
        fiber->state = FIBER_DONE;
        fiber->result = LISA_NIL;
    } else {
        /* call_value set up the call frame. Now run the interpreter. */
        lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
        int base = vm->frame_count - 1;

        /* JIT dispatch for the entry call */
        if (frame->closure->function->jit_code) {
            typedef lisa_value (*lisa_jit_fn)(lisa_vm *, lisa_obj_closure *,
                                              lisa_value *);
            lisa_jit_fn jit_fn = (lisa_jit_fn)frame->closure->function->jit_code;
            lisa_value result = jit_fn(vm, frame->closure, frame->slots);
            /* Handle tail-call trampoline */
            while ((result >> 48) == 0xDEAD) {
                result = lisa_jit_call_helper(vm, (int)(result & 0xFF));
            }
            /* Pop the JIT frame */
            vm->frame_count--;
            vm->stack_top = frame->slots;
            *vm->stack_top++ = result;
            fiber->result = result;
        } else {
            lisa_interpret_result res = lisa_run(vm, base);
            if (res == INTERPRET_OK && vm->stack_top > vm->stack) {
                fiber->result = *(vm->stack_top - 1);
            } else {
                fiber->result = LISA_NIL;
            }
        }
        fiber->state = FIBER_DONE;
    }

    /* Switch back to the main fiber's C stack.
       The scheduler loop will see FIBER_DONE. */
    lisa_fiber_save(vm);
    lisa_fiber_switch(&fiber->c_sp, vm->main_fiber->c_sp);

    /* Should never reach here */
    __builtin_unreachable();
}

/* ===== C Stack Setup ===== */

/*
 * Set up a new fiber's C stack so that fiber_switch "returns" into
 * fiber_trampoline_entry.
 *
 * The stack layout matches what fiber_switch's restore sequence expects:
 * it pops callee-saved registers then does ret (x86) or restores x30
 * and does ret (ARM64).
 */
static void setup_c_stack(lisa_fiber *fiber) {
    size_t size;
    void *base = alloc_c_stack(&size);
    if (!base) {
        fprintf(stderr, "Failed to allocate fiber C stack\n");
        abort();
    }
    fiber->c_stack = base;
    fiber->c_stack_size = size;

    /* Stack grows downward. Top of usable area: base + size */
    uintptr_t top = (uintptr_t)base + size;
    /* Align to 16 bytes */
    top &= ~(uintptr_t)15;

#if defined(__aarch64__) || defined(_M_ARM64)
    /*
     * fiber_switch restores (in order):
     *   ldp x29, x30, [sp], #16   — pair 1 (bottom of saved area)
     *   ldp x27, x28, [sp], #16   — pair 2
     *   ldp x25, x26, [sp], #16   — pair 3
     *   ldp x23, x24, [sp], #16   — pair 4
     *   ldp x21, x22, [sp], #16   — pair 5
     *   ldp x19, x20, [sp], #16   — pair 6
     *   ret  (jumps to x30)
     *
     * So the stack layout from low to high address:
     *   [sp+0]:  x29, [sp+8]:  x30 (lr = fiber_trampoline_entry)
     *   [sp+16]: x27, [sp+24]: x28
     *   [sp+32]: x25, [sp+40]: x26
     *   [sp+48]: x23, [sp+56]: x24
     *   [sp+64]: x21, [sp+72]: x22
     *   [sp+80]: x19, [sp+88]: x20
     *
     * Total: 96 bytes (6 pairs * 16). After restore, SP = saved_sp + 96.
     * SP must be 16-byte aligned throughout.
     */
    uint64_t *sp = (uint64_t *)top;
    sp -= 12; /* 96 bytes = 12 * 8 */

    sp[0] = 0;  /* x29 (fp) */
    sp[1] = (uint64_t)(uintptr_t)fiber_trampoline_entry; /* x30 (lr) */
    sp[2] = 0; sp[3] = 0;   /* x27, x28 */
    sp[4] = 0; sp[5] = 0;   /* x25, x26 */
    sp[6] = 0; sp[7] = 0;   /* x23, x24 */
    sp[8] = 0; sp[9] = 0;   /* x21, x22 */
    sp[10] = 0; sp[11] = 0; /* x19, x20 */

    fiber->c_sp = sp;

#elif defined(__x86_64__) || defined(_M_X64)
    /*
     * fiber_switch restores (pop order):
     *   pop r15, pop r14, pop r13, pop r12, pop rbx, pop rbp, retq
     *
     * Stack layout from low to high address (sp is lowest):
     *   [sp+0]:  r15  (first pop)
     *   [sp+8]:  r14
     *   [sp+16]: r13
     *   [sp+24]: r12
     *   [sp+32]: rbx
     *   [sp+40]: rbp
     *   [sp+48]: return address (fiber_trampoline_entry)
     *   [sp+56]: (alignment padding)
     *
     * After all pops + retq: RSP = sp + 56.
     * x86_64 ABI: at function entry, RSP % 16 == 8 (call pushes 8 bytes).
     * top is 16-byte aligned. sp = top - 64. sp+56 = top - 8.
     * (top - 8) % 16 == 8. Correct!
     */
    uint64_t *sp = (uint64_t *)top;
    sp -= 8; /* 64 bytes = 8 * 8 */

    sp[0] = 0;  /* r15 */
    sp[1] = 0;  /* r14 */
    sp[2] = 0;  /* r13 */
    sp[3] = 0;  /* r12 */
    sp[4] = 0;  /* rbx */
    sp[5] = 0;  /* rbp */
    sp[6] = (uint64_t)(uintptr_t)fiber_trampoline_entry; /* return address */
    sp[7] = 0;  /* alignment padding */

    fiber->c_sp = sp;
#endif
}

/* ===== Fiber Creation ===== */

lisa_fiber *lisa_new_main_fiber(lisa_vm *vm) {
    lisa_fiber *fiber = malloc(sizeof(lisa_fiber));
    fiber->obj.type = OBJ_FIBER;
    fiber->obj.is_marked = false;
    fiber->obj.next = vm->gc.objects;
    vm->gc.objects = (lisa_obj *)fiber;
    vm->gc.bytes_allocated += sizeof(lisa_fiber);

    fiber->state = FIBER_RUNNING;
    fiber->stack = malloc(sizeof(lisa_value) * STACK_MAX);
    fiber->stack_top = fiber->stack;
    fiber->frames = malloc(sizeof(lisa_call_frame) * FRAMES_MAX);
    fiber->frame_count = 0;
    fiber->open_upvalues = NULL;

    fiber->c_stack = NULL; /* main fiber uses the OS stack */
    fiber->c_stack_size = 0;
    fiber->c_sp = NULL;

    fiber->result = LISA_NIL;
    fiber->entry = NULL;
    fiber->argc = 0;

    fiber->next_fiber = vm->gc.all_fibers;
    vm->gc.all_fibers = fiber;

    return fiber;
}

lisa_fiber *lisa_new_fiber(lisa_vm *vm, lisa_obj_closure *entry, int argc,
                           lisa_value *args) {
    lisa_fiber *fiber = malloc(sizeof(lisa_fiber));
    fiber->obj.type = OBJ_FIBER;
    fiber->obj.is_marked = false;
    fiber->obj.next = vm->gc.objects;
    vm->gc.objects = (lisa_obj *)fiber;
    vm->gc.bytes_allocated += sizeof(lisa_fiber);

    fiber->state = FIBER_READY;
    fiber->stack = malloc(sizeof(lisa_value) * STACK_MAX);
    fiber->stack_top = fiber->stack;
    fiber->frames = malloc(sizeof(lisa_call_frame) * FRAMES_MAX);
    fiber->frame_count = 0;
    fiber->open_upvalues = NULL;

    fiber->result = LISA_NIL;
    fiber->entry = entry;
    fiber->argc = argc;

    /* Push the closure onto the fiber's stack (slot 0), then the args */
    *fiber->stack_top++ = LISA_OBJ(entry);
    for (int i = 0; i < argc; i++) {
        *fiber->stack_top++ = args[i];
    }

    /* Set up C stack for JIT / context switch */
    setup_c_stack(fiber);

    fiber->next_fiber = vm->gc.all_fibers;
    vm->gc.all_fibers = fiber;

    return fiber;
}

void lisa_fiber_free_stacks(lisa_fiber *fiber) {
    free(fiber->stack);
    free(fiber->frames);
    fiber->stack = NULL;
    fiber->frames = NULL;
    free_c_stack(fiber->c_stack, fiber->c_stack_size);
    fiber->c_stack = NULL;
}

/* ===== Channel ===== */

lisa_channel *lisa_new_channel(lisa_vm *vm) {
    lisa_channel *ch = malloc(sizeof(lisa_channel));
    ch->obj.type = OBJ_CHANNEL;
    ch->obj.is_marked = false;
    ch->obj.next = vm->gc.objects;
    vm->gc.objects = (lisa_obj *)ch;
    vm->gc.bytes_allocated += sizeof(lisa_channel);

    ch->value = LISA_NIL;
    ch->sender = NULL;
    ch->receiver = NULL;
    ch->closed = false;

    return ch;
}

/* ===== Scheduler ===== */

void lisa_sched_init(lisa_scheduler *sched) {
    sched->capacity = 16;
    sched->queue = malloc(sizeof(lisa_fiber *) * (size_t)sched->capacity);
    sched->head = 0;
    sched->tail = 0;
}

void lisa_sched_free(lisa_scheduler *sched) {
    free(sched->queue);
    sched->queue = NULL;
    sched->capacity = 0;
    sched->head = sched->tail = 0;
}

static int sched_count(lisa_scheduler *sched) {
    return (sched->tail - sched->head + sched->capacity) % sched->capacity;
}

void lisa_sched_enqueue(lisa_scheduler *sched, lisa_fiber *fiber) {
    if (sched_count(sched) >= sched->capacity - 1) {
        int old_cap = sched->capacity;
        int new_cap = old_cap * 2;
        lisa_fiber **new_q = malloc(sizeof(lisa_fiber *) * (size_t)new_cap);
        int n = sched_count(sched);
        for (int i = 0; i < n; i++) {
            new_q[i] = sched->queue[(sched->head + i) % old_cap];
        }
        free(sched->queue);
        sched->queue = new_q;
        sched->head = 0;
        sched->tail = n;
        sched->capacity = new_cap;
    }
    sched->queue[sched->tail] = fiber;
    sched->tail = (sched->tail + 1) % sched->capacity;
}

lisa_fiber *lisa_sched_dequeue(lisa_scheduler *sched) {
    if (sched->head == sched->tail) return NULL;
    lisa_fiber *f = sched->queue[sched->head];
    sched->head = (sched->head + 1) % sched->capacity;
    return f;
}

bool lisa_sched_empty(lisa_scheduler *sched) {
    return sched->head == sched->tail;
}

/* ===== Fiber Save / Restore ===== */

void lisa_fiber_save(lisa_vm *vm) {
    lisa_fiber *f = vm->current_fiber;
    f->stack_top = vm->stack_top;
    f->frame_count = vm->frame_count;
    f->open_upvalues = vm->open_upvalues;
}

void lisa_fiber_restore(lisa_vm *vm, lisa_fiber *f) {
    vm->current_fiber = f;
    vm->stack = f->stack;
    vm->stack_top = f->stack_top;
    vm->frames = f->frames;
    vm->frame_count = f->frame_count;
    vm->open_upvalues = f->open_upvalues;
}

/* ===== Scheduler Core ===== */

/*
 * Run one iteration of the scheduler: dequeue a fiber, switch to it,
 * return when it yields or completes. Runs on the main fiber's (OS) C stack.
 * Returns false if the queue was empty.
 */
static bool scheduler_step(lisa_vm *vm) {
    lisa_fiber *next = lisa_sched_dequeue(&vm->scheduler);
    if (!next) return false;
    if (next->state == FIBER_DONE) return true; /* skip, try next */

    /* If the dequeued fiber is the main fiber, don't touch its state —
       yield_to_scheduler will see state == FIBER_READY and exit its loop. */
    if (next == vm->main_fiber) {
        return true;
    }

    next->state = FIBER_RUNNING;
    lisa_fiber_restore(vm, next);

    /* Set global vm pointer for trampoline entry (new fibers) */
    g_trampoline_vm = vm;

    /* Switch C stacks: save main fiber's SP, jump to next fiber */
    lisa_fiber_switch(&vm->main_fiber->c_sp, next->c_sp);

    /* Back on main fiber's C stack. Restore main fiber as current. */
    lisa_fiber_restore(vm, vm->main_fiber);
    return true;
}

/* ===== Yield to Scheduler ===== */

static void yield_to_scheduler(lisa_vm *vm) {
    lisa_fiber *current = vm->current_fiber;
    lisa_fiber_save(vm);

    if (current == vm->main_fiber) {
        /*
         * Main fiber: can't context-switch away (uses OS stack).
         * Instead, run the scheduler loop inline until we're woken up.
         * Another fiber will set our state to FIBER_READY and enqueue us.
         */
        while (current->state != FIBER_READY) {
            if (!scheduler_step(vm)) {
                /* Queue empty but we're still suspended — deadlock */
                fprintf(stderr, "deadlock: main fiber blocked with no runnable fibers\n");
                exit(70);
            }
        }
        /* Woken up. Restore our state. */
        current->state = FIBER_RUNNING;
        lisa_fiber_restore(vm, current);
    } else {
        /* Spawned fiber: switch C stacks to return to the scheduler
           (which is running on the main fiber's OS stack). */
        lisa_fiber_switch(&current->c_sp, vm->main_fiber->c_sp);
        /* Resumed by the scheduler */
    }
}

/* ===== Scheduler Run Loop ===== */

static void scheduler_run(lisa_vm *vm) {
    while (!lisa_sched_empty(&vm->scheduler)) {
        scheduler_step(vm);
    }
}

/* ===== Native Functions ===== */

lisa_value native_chan(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc; (void)args;
    lisa_channel *ch = lisa_new_channel(vm);
    return LISA_OBJ(ch);
}

lisa_value native_spawn(lisa_vm *vm, int argc, lisa_value *args) {
    if (argc < 1 || !IS_CLOSURE(args[0])) {
        fprintf(stderr, "spawn: first argument must be a function\n");
        return LISA_NIL;
    }
    lisa_obj_closure *closure = AS_CLOSURE(args[0]);
    int fn_argc = argc - 1;
    lisa_value *fn_args = args + 1;

    lisa_fiber *fiber = lisa_new_fiber(vm, closure, fn_argc, fn_args);
    lisa_sched_enqueue(&vm->scheduler, fiber);

    return LISA_OBJ(fiber);
}

lisa_value native_send(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc;
    if (!IS_CHANNEL(args[0])) {
        fprintf(stderr, "send: first argument must be a channel\n");
        return LISA_NIL;
    }
    lisa_channel *ch = AS_CHANNEL(args[0]);
    lisa_value val = args[1];

    if (ch->receiver) {
        /* A receiver is waiting — hand off directly */
        lisa_fiber *recv_fiber = ch->receiver;
        ch->receiver = NULL;
        recv_fiber->result = val;
        recv_fiber->state = FIBER_READY;
        lisa_sched_enqueue(&vm->scheduler, recv_fiber);
        return LISA_NIL;
    }

    /* No receiver — block the sender */
    lisa_fiber *current = vm->current_fiber;
    ch->sender = current;
    ch->value = val;
    current->state = FIBER_SUSPENDED;

    yield_to_scheduler(vm);

    return LISA_NIL;
}

lisa_value native_recv(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc;
    if (!IS_CHANNEL(args[0])) {
        fprintf(stderr, "recv: first argument must be a channel\n");
        return LISA_NIL;
    }
    lisa_channel *ch = AS_CHANNEL(args[0]);

    if (ch->sender) {
        /* A sender is waiting — take the value and wake it */
        lisa_fiber *send_fiber = ch->sender;
        lisa_value val = ch->value;
        ch->sender = NULL;
        ch->value = LISA_NIL;
        send_fiber->state = FIBER_READY;
        lisa_sched_enqueue(&vm->scheduler, send_fiber);
        return val;
    }

    /* No sender — block the receiver */
    lisa_fiber *current = vm->current_fiber;
    ch->receiver = current;
    current->state = FIBER_SUSPENDED;

    yield_to_scheduler(vm);

    /* Resumed — result was placed in fiber->result by the sender */
    return vm->current_fiber->result;
}

lisa_value native_yield(lisa_vm *vm, int argc, lisa_value *args) {
    lisa_fiber *current = vm->current_fiber;

    /* Can't yield from the main fiber */
    if (current == vm->main_fiber) {
        return argc > 0 ? args[0] : LISA_NIL;
    }

    current->result = argc > 0 ? args[0] : LISA_NIL;
    current->state = FIBER_READY;
    lisa_sched_enqueue(&vm->scheduler, current);

    yield_to_scheduler(vm);

    return vm->current_fiber->result;
}

/* ===== Public API ===== */

void lisa_run_scheduler(lisa_vm *vm) {
    scheduler_run(vm);
}
