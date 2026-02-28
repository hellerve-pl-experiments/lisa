#ifndef LISA_JIT_H
#define LISA_JIT_H

#include "vm.h"

/* Compile a function's bytecode to native code via cj.
   Sets fn->jit_code and fn->jit_ctx on success. */
bool lisa_jit_compile(lisa_vm *vm, lisa_obj_function *fn);

/* Free JIT-compiled code for a function */
void lisa_jit_free(lisa_obj_function *fn);

#endif
