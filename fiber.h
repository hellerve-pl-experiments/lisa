#ifndef LISA_FIBER_H
#define LISA_FIBER_H

#include "object.h"

/* Forward declaration (defined in vm.h) */
typedef struct lisa_call_frame lisa_call_frame;

typedef enum {
    FIBER_READY,
    FIBER_RUNNING,
    FIBER_SUSPENDED,
    FIBER_DONE,
} lisa_fiber_state;

struct lisa_fiber {
    lisa_obj obj;
    lisa_fiber_state state;

    /* Lisa VM state (owned by this fiber) */
    lisa_value *stack;
    lisa_value *stack_top;
    lisa_call_frame *frames;
    int frame_count;
    lisa_obj_upvalue *open_upvalues;

    /* C stack for JIT (mmap'd with guard page) */
    void *c_stack;        /* base of mmap region */
    size_t c_stack_size;  /* total mmap size including guard */
    void *c_sp;           /* saved C stack pointer (for context switch) */

    /* Coroutine state */
    lisa_value result;    /* value passed into/out of yield */
    lisa_obj_closure *entry;  /* closure to call when first started */
    int argc;

    /* Linked list for GC traversal */
    lisa_fiber *next_fiber;
};

struct lisa_channel {
    lisa_obj obj;
    lisa_value value;       /* buffered value (for handoff) */
    lisa_fiber *sender;     /* fiber blocked on send, or NULL */
    lisa_fiber *receiver;   /* fiber blocked on recv, or NULL */
    bool closed;
};

/* Scheduler */
typedef struct {
    lisa_fiber **queue;
    int head, tail, capacity;
} lisa_scheduler;

/* Fiber lifecycle */
lisa_fiber *lisa_new_fiber(lisa_vm *vm, lisa_obj_closure *entry, int argc,
                           lisa_value *args);
lisa_fiber *lisa_new_main_fiber(lisa_vm *vm);
void lisa_fiber_free_stacks(lisa_fiber *fiber);

/* Channel */
lisa_channel *lisa_new_channel(lisa_vm *vm);

/* Scheduler */
void lisa_sched_init(lisa_scheduler *sched);
void lisa_sched_free(lisa_scheduler *sched);
void lisa_sched_enqueue(lisa_scheduler *sched, lisa_fiber *fiber);
lisa_fiber *lisa_sched_dequeue(lisa_scheduler *sched);
bool lisa_sched_empty(lisa_scheduler *sched);

/* Context switch (saves callee-saved regs + SP) */
void lisa_fiber_switch(void **save_sp, void *restore_sp);

/* Fiber save/restore VM state */
void lisa_fiber_save(lisa_vm *vm);
void lisa_fiber_restore(lisa_vm *vm, lisa_fiber *f);

/* Run all enqueued fibers to completion */
void lisa_run_scheduler(lisa_vm *vm);

/* Native functions for fibers */
lisa_value native_chan(lisa_vm *vm, int argc, lisa_value *args);
lisa_value native_spawn(lisa_vm *vm, int argc, lisa_value *args);
lisa_value native_send(lisa_vm *vm, int argc, lisa_value *args);
lisa_value native_recv(lisa_vm *vm, int argc, lisa_value *args);
lisa_value native_yield(lisa_vm *vm, int argc, lisa_value *args);

#endif
