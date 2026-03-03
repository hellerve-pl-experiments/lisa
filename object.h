#ifndef LISA_OBJECT_H
#define LISA_OBJECT_H

#include "value.h"
#include "chunk.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations (defined in vm.h / fiber.h) */
typedef struct lisa_vm lisa_vm;
typedef struct lisa_fiber lisa_fiber;
typedef struct lisa_channel lisa_channel;

typedef enum {
    OBJ_STRING,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_LIST,
    OBJ_NATIVE,
    OBJ_FIBER,
    OBJ_CHANNEL,
} lisa_obj_type;

struct lisa_obj {
    lisa_obj_type type;
    bool is_marked;
    struct lisa_obj *next;
};

typedef struct {
    lisa_obj obj;
    int length;
    uint32_t hash;
    char chars[];   /* flexible array */
} lisa_obj_string;

typedef struct {
    lisa_obj obj;
    int arity;
    int upvalue_count;
    lisa_chunk chunk;
    lisa_obj_string *name;
    void *jit_code;    /* JIT-compiled native code, or NULL */
    void *jit_ctx;     /* cj_ctx* for cleanup, or NULL */
} lisa_obj_function;

typedef struct lisa_obj_upvalue {
    lisa_obj obj;
    lisa_value *location;
    lisa_value closed;
    struct lisa_obj_upvalue *next;
} lisa_obj_upvalue;

typedef struct {
    lisa_obj obj;
    lisa_obj_function *function;
    lisa_obj_upvalue **upvalues;
    int upvalue_count;
} lisa_obj_closure;

typedef struct {
    lisa_obj obj;
    lisa_value car;
    lisa_value cdr;
} lisa_obj_list;

typedef lisa_value (*lisa_native_fn)(lisa_vm *vm, int argc, lisa_value *args);

typedef struct {
    lisa_obj obj;
    lisa_native_fn function;
    const char *name;
    int arity; /* -1 for variadic */
} lisa_obj_native;

/* Type checks */
#define OBJ_TYPE(value)    (AS_OBJ(value)->type)
#define IS_STRING(value)   (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_STRING)
#define IS_FUNCTION(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_FUNCTION)
#define IS_CLOSURE(value)  (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_CLOSURE)
#define IS_NATIVE(value)   (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_NATIVE)
#define IS_LIST_OBJ(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_LIST)
#define IS_FIBER(value)    (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_FIBER)
#define IS_CHANNEL(value)  (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_CHANNEL)

/* Cast macros */
#define AS_STRING(value)   ((lisa_obj_string*)AS_OBJ(value))
#define AS_CSTRING(value)  (((lisa_obj_string*)AS_OBJ(value))->chars)
#define AS_FUNCTION(value) ((lisa_obj_function*)AS_OBJ(value))
#define AS_CLOSURE(value)  ((lisa_obj_closure*)AS_OBJ(value))
#define AS_UPVALUE(value)  ((lisa_obj_upvalue*)AS_OBJ(value))
#define AS_NATIVE(value)   ((lisa_obj_native*)AS_OBJ(value))
#define AS_LIST(value)     ((lisa_obj_list*)AS_OBJ(value))
#define AS_FIBER(value)    ((lisa_fiber*)AS_OBJ(value))
#define AS_CHANNEL(value)  ((lisa_channel*)AS_OBJ(value))

/* GC state */
typedef struct {
    lisa_obj *objects;        /* linked list of all allocated objects */
    lisa_obj_string **strings; /* interning hash table */
    int string_count;
    int string_capacity;
    size_t bytes_allocated;
    size_t next_gc;
    /* GC marking state — set externally by the VM */
    lisa_value *stack;
    int stack_count;
    lisa_obj_upvalue *open_upvalues;
    lisa_fiber *all_fibers;  /* linked list of all live fibers for GC */
} lisa_gc;

void lisa_gc_init(lisa_gc *gc);
void lisa_gc_free(lisa_gc *gc);
void lisa_gc_collect(lisa_gc *gc);

/* Allocation */
lisa_obj_string *lisa_copy_string(lisa_gc *gc, const char *chars, int length);
lisa_obj_string *lisa_take_string(lisa_gc *gc, char *chars, int length);
lisa_obj_function *lisa_new_function(lisa_gc *gc);
lisa_obj_closure *lisa_new_closure(lisa_gc *gc, lisa_obj_function *function);
lisa_obj_upvalue *lisa_new_upvalue(lisa_gc *gc, lisa_value *slot);
lisa_obj_list *lisa_new_list(lisa_gc *gc, lisa_value car, lisa_value cdr);
lisa_obj_native *lisa_new_native(lisa_gc *gc, lisa_native_fn function, const char *name, int arity);

/* Printing */
void lisa_print_object(FILE *f, lisa_value value);

#endif
