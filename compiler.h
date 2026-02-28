#ifndef LISA_COMPILER_H
#define LISA_COMPILER_H

#include "ast.h"
#include "object.h"
#include <stdbool.h>

typedef struct {
    const char *name;
    int name_length;
    int depth;
    bool is_captured;
} lisa_local;

typedef struct {
    uint8_t index;
    bool is_local;
} lisa_compiler_upvalue;

typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT,
} lisa_function_type;

typedef struct lisa_compiler {
    struct lisa_compiler *enclosing;
    lisa_obj_function *function;
    lisa_function_type type;

    lisa_local locals[256];
    int local_count;
    int scope_depth;

    lisa_compiler_upvalue upvalues[256];

    lisa_gc *gc;
} lisa_compiler;

lisa_obj_function *lisa_compile(lisa_gc *gc, lisa_ast **exprs, int count);

#endif
