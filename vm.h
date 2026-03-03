#ifndef LISA_VM_H
#define LISA_VM_H

#include "value.h"
#include "object.h"
#include "fiber.h"
#include "chunk.h"

#define STACK_MAX 4096
#define FRAMES_MAX 256

typedef struct lisa_call_frame {
    lisa_obj_closure *closure;
    uint8_t *ip;
    lisa_value *slots; /* pointer into vm stack */
} lisa_call_frame;

struct lisa_vm {
    lisa_call_frame *frames;  /* points to current_fiber->frames */
    int frame_count;

    lisa_value *stack;        /* points to current_fiber->stack */
    lisa_value *stack_top;

    /* Global variables: hash table of string -> value */
    lisa_obj_string **global_names;
    lisa_value *global_values;
    int global_count;
    int global_capacity;

    lisa_obj_upvalue *open_upvalues;

    bool jit_enabled;

    /* Fiber support */
    lisa_fiber *current_fiber;
    lisa_fiber *main_fiber;
    lisa_scheduler scheduler;

    lisa_gc gc;
};

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
} lisa_interpret_result;

void lisa_vm_init(lisa_vm *vm);
void lisa_vm_free(lisa_vm *vm);
lisa_interpret_result lisa_interpret(lisa_vm *vm, const char *source);

/* Run the bytecode interpreter starting from the current top frame.
   Stops when frame_count drops to base_frame. */
lisa_interpret_result lisa_run(lisa_vm *vm, int base_frame);

/* Call a value (closure or native) with argc arguments on the stack.
   Returns true on success; for closures, sets up the call frame (use lisa_run after). */
bool lisa_call_value(lisa_vm *vm, lisa_value callee, int argc);

/* JIT trampoline sentinel: top 16 bits = 0xDEAD (invalid as any lisa_value tag),
   low 8 bits = argc for the pending tail call. */
#define LISA_TAIL_PENDING_BASE ((uint64_t)0xDEAD000000000000)
#define LISA_TAIL_PENDING(argc) (LISA_TAIL_PENDING_BASE | (uint64_t)(argc))
#define IS_TAIL_PENDING(v) (((v) >> 48) == 0xDEAD)
#define TAIL_PENDING_ARGC(v) ((int)((v) & 0xFF))

/* Helpers called by JIT-compiled code */
lisa_value lisa_jit_call_helper(lisa_vm *vm, int argc);
lisa_value lisa_jit_get_global(lisa_vm *vm, int name_idx);
void lisa_jit_def_global(lisa_vm *vm, int name_idx, lisa_value value);
lisa_value lisa_jit_get_upvalue(lisa_obj_closure *closure, int idx);
void lisa_jit_set_upvalue(lisa_obj_closure *closure, int idx, lisa_value value);
void lisa_jit_close_upvalue(lisa_vm *vm, lisa_value *addr);
lisa_value lisa_jit_make_closure(lisa_vm *vm, lisa_obj_closure *enclosing,
                                 lisa_obj_function *fn, uint8_t *ip);
void lisa_jit_runtime_error(lisa_vm *vm, const char *msg);
lisa_value lisa_jit_add(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_sub(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_mul(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_div(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_mod(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_negate(lisa_vm *vm, lisa_value v);
lisa_value lisa_jit_less(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_less_equal(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_greater(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_greater_equal(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_equal(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_not_equal(lisa_vm *vm, lisa_value a, lisa_value b);
lisa_value lisa_jit_cons(lisa_vm *vm, lisa_value car, lisa_value cdr);
lisa_value lisa_jit_car(lisa_vm *vm, lisa_value v);
lisa_value lisa_jit_cdr(lisa_vm *vm, lisa_value v);
lisa_value lisa_jit_list(lisa_vm *vm, int n);
lisa_value lisa_jit_println(lisa_vm *vm, int argc);

#endif
