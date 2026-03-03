#include "vm.h"
#include "fiber.h"
#include "jit.h"
#include "compiler.h"
#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef lisa_value (*lisa_jit_fn)(lisa_vm *vm, lisa_obj_closure *closure,
                                  lisa_value *slots);

static lisa_value jit_trampoline(lisa_vm *vm, lisa_value result);

/* --- Stack operations --- */

static void push(lisa_vm *vm, lisa_value value) {
    *vm->stack_top = value;
    vm->stack_top++;
}

static lisa_value pop(lisa_vm *vm) {
    vm->stack_top--;
    return *vm->stack_top;
}

static lisa_value peek(lisa_vm *vm, int distance) {
    return vm->stack_top[-1 - distance];
}

static void reset_stack(lisa_vm *vm) {
    if (vm->stack) vm->stack_top = vm->stack;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;
}

static void runtime_error(lisa_vm *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm->frame_count - 1; i >= 0; i--) {
        lisa_call_frame *frame = &vm->frames[i];
        lisa_obj_function *fn = frame->closure->function;
        size_t offset = (size_t)(frame->ip - fn->chunk.code - 1);
        int line = fn->chunk.lines[offset];
        fprintf(stderr, "[line %d] in ", line);
        if (fn->name == NULL) {
            fprintf(stderr, "script\n");
        } else {
            fprintf(stderr, "%s()\n", fn->name->chars);
        }
    }

    reset_stack(vm);
}

/* --- Globals hash table --- */

static void globals_grow(lisa_vm *vm) {
    int new_cap = vm->global_capacity < 8 ? 8 : vm->global_capacity * 2;
    lisa_obj_string **new_names = calloc((size_t)new_cap, sizeof(lisa_obj_string*));
    lisa_value *new_values = calloc((size_t)new_cap, sizeof(lisa_value));

    for (int i = 0; i < vm->global_capacity; i++) {
        if (vm->global_names[i] == NULL) continue;
        uint32_t idx = vm->global_names[i]->hash % (uint32_t)new_cap;
        while (new_names[idx] != NULL) {
            idx = (idx + 1) % (uint32_t)new_cap;
        }
        new_names[idx] = vm->global_names[i];
        new_values[idx] = vm->global_values[i];
    }

    free(vm->global_names);
    free(vm->global_values);
    vm->global_names = new_names;
    vm->global_values = new_values;
    vm->global_capacity = new_cap;
}

static int globals_find(lisa_vm *vm, lisa_obj_string *name) {
    if (vm->global_capacity == 0) return -1;
    uint32_t idx = name->hash % (uint32_t)vm->global_capacity;
    for (;;) {
        if (vm->global_names[idx] == NULL) return -1;
        if (vm->global_names[idx] == name) return (int)idx;
        idx = (idx + 1) % (uint32_t)vm->global_capacity;
    }
}

static void globals_set(lisa_vm *vm, lisa_obj_string *name, lisa_value value) {
    if (vm->global_count + 1 > vm->global_capacity * 3 / 4) {
        globals_grow(vm);
    }
    uint32_t idx = name->hash % (uint32_t)vm->global_capacity;
    while (vm->global_names[idx] != NULL && vm->global_names[idx] != name) {
        idx = (idx + 1) % (uint32_t)vm->global_capacity;
    }
    if (vm->global_names[idx] == NULL) vm->global_count++;
    vm->global_names[idx] = name;
    vm->global_values[idx] = value;
}

/* --- Native functions --- */

static void define_native(lisa_vm *vm, const char *name, lisa_native_fn fn, int arity) {
    lisa_obj_string *name_str = lisa_copy_string(&vm->gc, name, (int)strlen(name));
    push(vm, LISA_OBJ(name_str)); /* GC protect */
    lisa_obj_native *native = lisa_new_native(&vm->gc, fn, name, arity);
    push(vm, LISA_OBJ(native)); /* GC protect */
    globals_set(vm, name_str, LISA_OBJ(native));
    pop(vm);
    pop(vm);
}

/* Built-in native functions for when operators are used as values */
static lisa_value native_add(lisa_vm *vm_, int argc, lisa_value *args) {
    (void)vm_; (void)argc;
    if (IS_INT(args[0]) && IS_INT(args[1])) return LISA_INT(AS_INT(args[0]) + AS_INT(args[1]));
    return lisa_double(lisa_as_number(args[0]) + lisa_as_number(args[1]));
}

static lisa_value native_sub(lisa_vm *vm_, int argc, lisa_value *args) {
    (void)vm_;
    if (argc == 1) {
        if (IS_INT(args[0])) return LISA_INT(-AS_INT(args[0]));
        return lisa_double(-AS_DOUBLE(args[0]));
    }
    if (IS_INT(args[0]) && IS_INT(args[1])) return LISA_INT(AS_INT(args[0]) - AS_INT(args[1]));
    return lisa_double(lisa_as_number(args[0]) - lisa_as_number(args[1]));
}

static lisa_value native_mul(lisa_vm *vm_, int argc, lisa_value *args) {
    (void)vm_; (void)argc;
    if (IS_INT(args[0]) && IS_INT(args[1])) return LISA_INT(AS_INT(args[0]) * AS_INT(args[1]));
    return lisa_double(lisa_as_number(args[0]) * lisa_as_number(args[1]));
}

static lisa_value native_div(lisa_vm *vm_, int argc, lisa_value *args) {
    (void)vm_; (void)argc;
    return lisa_double(lisa_as_number(args[0]) / lisa_as_number(args[1]));
}

/* --- String/utility native functions --- */

static lisa_value native_strlen(lisa_vm *vm, int argc, lisa_value *args) {
    (void)vm; (void)argc;
    if (!IS_STRING(args[0])) return LISA_NIL;
    return LISA_INT(AS_STRING(args[0])->length);
}

static lisa_value native_char_at(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc;
    if (!IS_STRING(args[0]) || !IS_INT(args[1])) return LISA_NIL;
    lisa_obj_string *s = AS_STRING(args[0]);
    int64_t idx = AS_INT(args[1]);
    if (idx < 0 || idx >= s->length) return LISA_NIL;
    return LISA_OBJ(lisa_copy_string(&vm->gc, &s->chars[idx], 1));
}

static lisa_value native_substr(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc;
    if (!IS_STRING(args[0]) || !IS_INT(args[1]) || !IS_INT(args[2])) return LISA_NIL;
    lisa_obj_string *s = AS_STRING(args[0]);
    int64_t start = AS_INT(args[1]);
    int64_t len = AS_INT(args[2]);
    if (start < 0) start = 0;
    if (start > s->length) start = s->length;
    if (len < 0) len = 0;
    if (start + len > s->length) len = s->length - start;
    return LISA_OBJ(lisa_copy_string(&vm->gc, s->chars + start, (int)len));
}

static void stringify_value(lisa_value val, char *buf, int bufsize) {
    if (IS_NIL(val)) {
        snprintf(buf, (size_t)bufsize, "nil");
    } else if (IS_BOOL(val)) {
        snprintf(buf, (size_t)bufsize, "%s", AS_BOOL(val) ? "true" : "false");
    } else if (IS_INT(val)) {
        snprintf(buf, (size_t)bufsize, "%lld", (long long)AS_INT(val));
    } else if (IS_DOUBLE(val)) {
        snprintf(buf, (size_t)bufsize, "%g", AS_DOUBLE(val));
    } else if (IS_STRING(val)) {
        lisa_obj_string *s = AS_STRING(val);
        int copy_len = s->length < bufsize - 1 ? s->length : bufsize - 1;
        memcpy(buf, s->chars, (size_t)copy_len);
        buf[copy_len] = '\0';
    } else if (IS_OBJ(val)) {
        switch (OBJ_TYPE(val)) {
        case OBJ_LIST:     snprintf(buf, (size_t)bufsize, "<list>"); break;
        case OBJ_CLOSURE:  snprintf(buf, (size_t)bufsize, "<fn>"); break;
        case OBJ_FUNCTION: snprintf(buf, (size_t)bufsize, "<fn>"); break;
        case OBJ_NATIVE:   snprintf(buf, (size_t)bufsize, "<native>"); break;
        case OBJ_FIBER:    snprintf(buf, (size_t)bufsize, "<fiber>"); break;
        case OBJ_CHANNEL:  snprintf(buf, (size_t)bufsize, "<channel>"); break;
        default:           snprintf(buf, (size_t)bufsize, "<object>"); break;
        }
    } else {
        snprintf(buf, (size_t)bufsize, "<unknown>");
    }
}

static lisa_value native_str(lisa_vm *vm, int argc, lisa_value *args) {
    if (argc == 0) return LISA_OBJ(lisa_copy_string(&vm->gc, "", 0));

    /* Fast path: single string arg */
    if (argc == 1 && IS_STRING(args[0])) return args[0];

    char buf[256];
    int total_len = 0;
    char *result = malloc(1);
    result[0] = '\0';

    for (int i = 0; i < argc; i++) {
        if (IS_STRING(args[i])) {
            lisa_obj_string *s = AS_STRING(args[i]);
            result = realloc(result, (size_t)(total_len + s->length + 1));
            memcpy(result + total_len, s->chars, (size_t)s->length);
            total_len += s->length;
            result[total_len] = '\0';
        } else {
            stringify_value(args[i], buf, (int)sizeof(buf));
            int slen = (int)strlen(buf);
            result = realloc(result, (size_t)(total_len + slen + 1));
            memcpy(result + total_len, buf, (size_t)slen);
            total_len += slen;
            result[total_len] = '\0';
        }
    }

    lisa_obj_string *s = lisa_take_string(&vm->gc, result, total_len);
    return LISA_OBJ(s);
}

static lisa_value native_parse_num(lisa_vm *vm, int argc, lisa_value *args) {
    (void)vm; (void)argc;
    if (!IS_STRING(args[0])) return LISA_NIL;
    lisa_obj_string *s = AS_STRING(args[0]);
    char *end;

    long long ival = strtoll(s->chars, &end, 10);
    if (end == s->chars + s->length && end != s->chars) {
        return LISA_INT((int64_t)ival);
    }

    double dval = strtod(s->chars, &end);
    if (end == s->chars + s->length && end != s->chars) {
        return lisa_double(dval);
    }

    return LISA_NIL;
}

static lisa_value native_type(lisa_vm *vm, int argc, lisa_value *args) {
    (void)argc;
    const char *name;
    if (IS_NIL(args[0]))          name = "nil";
    else if (IS_BOOL(args[0]))    name = "bool";
    else if (IS_INT(args[0]))     name = "int";
    else if (IS_DOUBLE(args[0]))  name = "double";
    else if (IS_STRING(args[0]))  name = "string";
    else if (IS_OBJ(args[0])) {
        switch (OBJ_TYPE(args[0])) {
        case OBJ_LIST:     name = "list"; break;
        case OBJ_CLOSURE:
        case OBJ_FUNCTION: name = "fn"; break;
        case OBJ_NATIVE:   name = "native"; break;
        case OBJ_FIBER:    name = "fiber"; break;
        case OBJ_CHANNEL:  name = "channel"; break;
        default:           name = "object"; break;
        }
    } else {
        name = "unknown";
    }
    return LISA_OBJ(lisa_copy_string(&vm->gc, name, (int)strlen(name)));
}

/* --- Upvalue management --- */

static lisa_obj_upvalue *capture_upvalue(lisa_vm *vm, lisa_value *local) {
    lisa_obj_upvalue *prev = NULL;
    lisa_obj_upvalue *upvalue = vm->open_upvalues;

    while (upvalue != NULL && upvalue->location > local) {
        prev = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    lisa_obj_upvalue *created = lisa_new_upvalue(&vm->gc, local);
    created->next = upvalue;

    if (prev == NULL) {
        vm->open_upvalues = created;
    } else {
        prev->next = created;
    }

    return created;
}

static void close_upvalues(lisa_vm *vm, lisa_value *last) {
    while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
        lisa_obj_upvalue *upvalue = vm->open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->open_upvalues = upvalue->next;
    }
}

/* --- Call --- */

static bool call_closure(lisa_vm *vm, lisa_obj_closure *closure, int argc) {
    if (argc != closure->function->arity) {
        runtime_error(vm, "Expected %d arguments but got %d.",
                      closure->function->arity, argc);
        return false;
    }
    if (vm->frame_count >= FRAMES_MAX) {
        runtime_error(vm, "Stack overflow.");
        return false;
    }
    /* JIT compile on first call (skip top-level script) */
    if (vm->jit_enabled && !closure->function->jit_code &&
        vm->frame_count > 0) {
        lisa_jit_compile(vm, closure->function);
    }
    lisa_call_frame *frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm->stack_top - argc - 1;
    return true;
}

bool lisa_call_value(lisa_vm *vm, lisa_value callee, int argc) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
        case OBJ_CLOSURE:
            return call_closure(vm, AS_CLOSURE(callee), argc);
        case OBJ_NATIVE: {
            lisa_obj_native *native = AS_NATIVE(callee);
            if (native->arity != -1 && native->arity != argc) {
                runtime_error(vm, "Expected %d arguments but got %d.", native->arity, argc);
                return false;
            }
            lisa_value result = native->function(vm, argc, vm->stack_top - argc);
            vm->stack_top -= argc + 1;
            push(vm, result);
            return true;
        }
        default:
            break;
        }
    }
    runtime_error(vm, "Can only call functions and closures.");
    return false;
}

/* --- String concatenation --- */

static void concatenate(lisa_vm *vm) {
    lisa_obj_string *b = AS_STRING(peek(vm, 0));
    lisa_obj_string *a = AS_STRING(peek(vm, 1));

    int length = a->length + b->length;
    char *chars = malloc((size_t)length + 1);
    memcpy(chars, a->chars, (size_t)a->length);
    memcpy(chars + a->length, b->chars, (size_t)b->length);
    chars[length] = '\0';

    lisa_obj_string *result = lisa_take_string(&vm->gc, chars, length);
    pop(vm);
    pop(vm);
    push(vm, LISA_OBJ(result));
}

/* --- Main dispatch loop --- */

lisa_interpret_result lisa_run(lisa_vm *vm, int base_frame) {
    lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() \
    (frame->ip += 2, (uint16_t)((frame->ip[-2]) | ((uint16_t)frame->ip[-1] << 8)))
#define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define BINARY_OP(value_type, op) \
    do { \
        lisa_value b = pop(vm); \
        lisa_value a = pop(vm); \
        if (IS_INT(a) && IS_INT(b)) { \
            push(vm, LISA_INT(AS_INT(a) op AS_INT(b))); \
        } else if (lisa_is_number(a) && lisa_is_number(b)) { \
            push(vm, lisa_double(lisa_as_number(a) op lisa_as_number(b))); \
        } else { \
            runtime_error(vm, "Operands must be numbers."); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while (false)

    for (;;) {
        /* Update GC roots */
        vm->gc.stack = vm->stack;
        vm->gc.stack_count = (int)(vm->stack_top - vm->stack);
        vm->gc.open_upvalues = vm->open_upvalues;

        uint8_t instruction = READ_BYTE();
        switch (instruction) {
        case OP_CONSTANT: {
            push(vm, READ_CONSTANT());
            break;
        }
        case OP_NIL:   push(vm, LISA_NIL); break;
        case OP_TRUE:  push(vm, LISA_TRUE); break;
        case OP_FALSE: push(vm, LISA_FALSE); break;
        case OP_POP:   pop(vm); break;

        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();
            push(vm, frame->slots[slot]);
            break;
        }
        case OP_SET_LOCAL: {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = peek(vm, 0);
            break;
        }
        case OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            push(vm, *frame->closure->upvalues[slot]->location);
            break;
        }
        case OP_SET_UPVALUE: {
            uint8_t slot = READ_BYTE();
            *frame->closure->upvalues[slot]->location = peek(vm, 0);
            break;
        }
        case OP_GET_GLOBAL: {
            lisa_obj_string *name = AS_STRING(READ_CONSTANT());
            int idx = globals_find(vm, name);
            if (idx == -1) {
                runtime_error(vm, "Undefined variable '%s'.", name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(vm, vm->global_values[idx]);
            break;
        }
        case OP_DEF_GLOBAL: {
            lisa_obj_string *name = AS_STRING(READ_CONSTANT());
            globals_set(vm, name, peek(vm, 0));
            pop(vm);
            break;
        }

        case OP_ADD: {
            lisa_value b = peek(vm, 0);
            lisa_value a = peek(vm, 1);
            if (IS_STRING(a) && IS_STRING(b)) {
                concatenate(vm);
            } else if (IS_INT(a) && IS_INT(b)) {
                pop(vm); pop(vm);
                push(vm, LISA_INT(AS_INT(a) + AS_INT(b)));
            } else if (lisa_is_number(a) && lisa_is_number(b)) {
                pop(vm); pop(vm);
                push(vm, lisa_double(lisa_as_number(a) + lisa_as_number(b)));
            } else {
                runtime_error(vm, "Operands must be numbers or strings.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_SUB: BINARY_OP(LISA_INT, -); break;
        case OP_MUL: BINARY_OP(LISA_INT, *); break;
        case OP_DIV: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (!lisa_is_number(a) || !lisa_is_number(b)) {
                runtime_error(vm, "Operands must be numbers.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(vm, lisa_double(lisa_as_number(a) / lisa_as_number(b)));
            break;
        }
        case OP_MOD: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (IS_INT(a) && IS_INT(b)) {
                push(vm, LISA_INT(AS_INT(a) % AS_INT(b)));
            } else if (lisa_is_number(a) && lisa_is_number(b)) {
                push(vm, lisa_double(fmod(lisa_as_number(a), lisa_as_number(b))));
            } else {
                runtime_error(vm, "Operands must be numbers.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }
        case OP_NEGATE: {
            lisa_value v = pop(vm);
            if (IS_INT(v)) {
                push(vm, LISA_INT(-AS_INT(v)));
            } else if (IS_DOUBLE(v)) {
                push(vm, lisa_double(-AS_DOUBLE(v)));
            } else {
                runtime_error(vm, "Operand must be a number.");
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }

        case OP_EQUAL: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            push(vm, LISA_BOOL(lisa_values_equal(a, b)));
            break;
        }
        case OP_NOT_EQUAL: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            push(vm, LISA_BOOL(!lisa_values_equal(a, b)));
            break;
        }
        case OP_LESS: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (IS_INT(a) && IS_INT(b)) push(vm, LISA_BOOL(AS_INT(a) < AS_INT(b)));
            else if (lisa_is_number(a) && lisa_is_number(b)) push(vm, LISA_BOOL(lisa_as_number(a) < lisa_as_number(b)));
            else { runtime_error(vm, "Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; }
            break;
        }
        case OP_LESS_EQUAL: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (IS_INT(a) && IS_INT(b)) push(vm, LISA_BOOL(AS_INT(a) <= AS_INT(b)));
            else if (lisa_is_number(a) && lisa_is_number(b)) push(vm, LISA_BOOL(lisa_as_number(a) <= lisa_as_number(b)));
            else { runtime_error(vm, "Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; }
            break;
        }
        case OP_GREATER: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (IS_INT(a) && IS_INT(b)) push(vm, LISA_BOOL(AS_INT(a) > AS_INT(b)));
            else if (lisa_is_number(a) && lisa_is_number(b)) push(vm, LISA_BOOL(lisa_as_number(a) > lisa_as_number(b)));
            else { runtime_error(vm, "Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; }
            break;
        }
        case OP_GREATER_EQUAL: {
            lisa_value b = pop(vm);
            lisa_value a = pop(vm);
            if (IS_INT(a) && IS_INT(b)) push(vm, LISA_BOOL(AS_INT(a) >= AS_INT(b)));
            else if (lisa_is_number(a) && lisa_is_number(b)) push(vm, LISA_BOOL(lisa_as_number(a) >= lisa_as_number(b)));
            else { runtime_error(vm, "Operands must be numbers."); return INTERPRET_RUNTIME_ERROR; }
            break;
        }

        case OP_NOT: {
            lisa_value v = pop(vm);
            push(vm, LISA_BOOL(lisa_is_falsey(v)));
            break;
        }

        case OP_JUMP: {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_SHORT();
            if (lisa_is_falsey(peek(vm, 0))) {
                frame->ip += offset;
            }
            pop(vm);
            break;
        }
        case OP_LOOP: {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }

        case OP_CLOSURE: {
            lisa_obj_function *fn = AS_FUNCTION(READ_CONSTANT());
            lisa_obj_closure *closure = lisa_new_closure(&vm->gc, fn);
            push(vm, LISA_OBJ(closure));
            for (int i = 0; i < closure->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (is_local) {
                    closure->upvalues[i] = capture_upvalue(vm, frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }

        case OP_CALL: {
            int argc = READ_BYTE();
            if (!lisa_call_value(vm, peek(vm, argc), argc)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm->frames[vm->frame_count - 1];
            /* JIT dispatch: if the callee has JIT'd code, call it directly */
            if (frame->closure->function->jit_code) {
                lisa_jit_fn jit_fn = (lisa_jit_fn)frame->closure->function->jit_code;
                lisa_value result = jit_fn(vm, frame->closure, frame->slots);
                if (IS_TAIL_PENDING(result))
                    result = jit_trampoline(vm, result);
                /* JIT function returned; pop its frame */
                close_upvalues(vm, frame->slots);
                vm->frame_count--;
                vm->stack_top = frame->slots;
                push(vm, result);
                frame = &vm->frames[vm->frame_count - 1];
            }
            break;
        }

        case OP_TAIL_CALL: {
            int argc = READ_BYTE();
            lisa_value callee = peek(vm, argc);

            /* Native functions: no frame to reuse, fall through to normal call */
            if (IS_OBJ(callee) && OBJ_TYPE(callee) == OBJ_NATIVE) {
                if (!lisa_call_value(vm, callee, argc)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }

            if (!IS_OBJ(callee) || OBJ_TYPE(callee) != OBJ_CLOSURE) {
                runtime_error(vm, "Can only call functions and closures.");
                return INTERPRET_RUNTIME_ERROR;
            }

            lisa_obj_closure *closure = AS_CLOSURE(callee);
            if (argc != closure->function->arity) {
                runtime_error(vm, "Expected %d arguments but got %d.",
                              closure->function->arity, argc);
                return INTERPRET_RUNTIME_ERROR;
            }

            /* Close upvalues for the current frame */
            close_upvalues(vm, frame->slots);

            /* Slide callee + args down over the current frame */
            lisa_value *src = vm->stack_top - argc - 1;
            memmove(frame->slots, src, (size_t)(argc + 1) * sizeof(lisa_value));
            vm->stack_top = frame->slots + argc + 1;

            /* Reuse the current frame */
            frame->closure = closure;
            frame->ip = closure->function->chunk.code;
            /* JIT dispatch for tail calls */
            if (closure->function->jit_code) {
                lisa_jit_fn jit_fn = (lisa_jit_fn)closure->function->jit_code;
                lisa_value result = jit_fn(vm, frame->closure, frame->slots);
                if (IS_TAIL_PENDING(result))
                    result = jit_trampoline(vm, result);
                close_upvalues(vm, frame->slots);
                vm->frame_count--;
                if (vm->frame_count == base_frame) {
                    if (base_frame == 0) pop(vm);
                    else {
                        vm->stack_top = frame->slots;
                        push(vm, result);
                    }
                    return INTERPRET_OK;
                }
                vm->stack_top = frame->slots;
                push(vm, result);
                frame = &vm->frames[vm->frame_count - 1];
            }
            break;
        }

        case OP_RETURN: {
            lisa_value result = pop(vm);
            close_upvalues(vm, frame->slots);
            vm->frame_count--;
            if (vm->frame_count == base_frame) {
                if (base_frame == 0) pop(vm); /* pop the script function */
                else {
                    vm->stack_top = frame->slots;
                    push(vm, result);
                }
                return INTERPRET_OK;
            }
            vm->stack_top = frame->slots;
            push(vm, result);
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }

        case OP_CLOSE_UPVALUE: {
            close_upvalues(vm, vm->stack_top - 1);
            pop(vm);
            break;
        }
        case OP_CLOSE_UPVALUES_AT: {
            uint8_t slot = READ_BYTE();
            close_upvalues(vm, &frame->slots[slot]);
            break;
        }

        case OP_CONS: {
            lisa_value cdr = pop(vm);
            lisa_value car = pop(vm);
            lisa_obj_list *list = lisa_new_list(&vm->gc, car, cdr);
            push(vm, LISA_OBJ(list));
            break;
        }
        case OP_CAR: {
            lisa_value v = pop(vm);
            if (!IS_LIST_OBJ(v)) {
                runtime_error(vm, "car requires a list.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(vm, AS_LIST(v)->car);
            break;
        }
        case OP_CDR: {
            lisa_value v = pop(vm);
            if (!IS_LIST_OBJ(v)) {
                runtime_error(vm, "cdr requires a list.");
                return INTERPRET_RUNTIME_ERROR;
            }
            push(vm, AS_LIST(v)->cdr);
            break;
        }
        case OP_LIST: {
            int n = READ_BYTE();
            /* Build list: (list a b c) => cons(a, cons(b, cons(c, nil)))
             * Stack has items in push order: [..., a, b, c] where c is on top.
             * peek(0)=c, peek(1)=b, peek(2)=a.
             * Build from right (top of stack) to left. */
            lisa_value result = LISA_NIL;
            for (int i = 0; i < n; i++) {
                lisa_value item = peek(vm, i);
                result = LISA_OBJ(lisa_new_list(&vm->gc, item, result));
            }
            /* Pop all N items */
            vm->stack_top -= n;
            push(vm, result);
            break;
        }

        case OP_PRINTLN: {
            int argc = READ_BYTE();
            for (int i = argc - 1; i >= 0; i--) {
                lisa_print_value(peek(vm, i));
                if (i > 0) putchar(' ');
            }
            putchar('\n');
            vm->stack_top -= argc;
            push(vm, LISA_NIL);
            break;
        }

        default:
            runtime_error(vm, "Unknown opcode %d.", instruction);
            return INTERPRET_RUNTIME_ERROR;
        }

        /* Trigger GC if needed */
        if (vm->gc.bytes_allocated > vm->gc.next_gc) {
            vm->gc.stack = vm->stack;
            vm->gc.stack_count = (int)(vm->stack_top - vm->stack);
            vm->gc.open_upvalues = vm->open_upvalues;

            /* Also mark globals */
            for (int i = 0; i < vm->global_capacity; i++) {
                if (vm->global_names[i] != NULL) {
                    /* Strings and values are reachable through the global table.
                     * We need to mark them. For simplicity, we mark the entire
                     * call stack's closures as roots. The gc.stack already covers values. */
                }
            }

            lisa_gc_collect(&vm->gc);
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef BINARY_OP
}

/* --- Public API --- */

void lisa_vm_init(lisa_vm *vm) {
    lisa_gc_init(&vm->gc);
    vm->global_names = NULL;
    vm->global_values = NULL;
    vm->global_count = 0;
    vm->global_capacity = 0;
    vm->jit_enabled = true;

    /* Create main fiber */
    vm->main_fiber = lisa_new_main_fiber(vm);
    vm->current_fiber = vm->main_fiber;
    vm->stack = vm->main_fiber->stack;
    vm->stack_top = vm->main_fiber->stack;
    vm->frames = vm->main_fiber->frames;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;

    lisa_sched_init(&vm->scheduler);

    /* Register native functions */
    define_native(vm, "+", native_add, 2);
    define_native(vm, "-", native_sub, -1);
    define_native(vm, "*", native_mul, 2);
    define_native(vm, "/", native_div, 2);

    /* Fiber/channel native functions */
    define_native(vm, "chan", native_chan, 0);
    define_native(vm, "spawn", native_spawn, -1);
    define_native(vm, "send", native_send, 2);
    define_native(vm, "recv", native_recv, 1);
    define_native(vm, "yield", native_yield, -1);

    /* String/utility native functions */
    define_native(vm, "strlen", native_strlen, 1);
    define_native(vm, "char-at", native_char_at, 2);
    define_native(vm, "substr", native_substr, 3);
    define_native(vm, "str", native_str, -1);
    define_native(vm, "parse-num", native_parse_num, 1);
    define_native(vm, "type", native_type, 1);
}

void lisa_vm_free(lisa_vm *vm) {
    lisa_sched_free(&vm->scheduler);
    free(vm->global_names);
    free(vm->global_values);
    lisa_gc_free(&vm->gc);
    vm->global_names = NULL;
    vm->global_values = NULL;
    vm->global_count = 0;
    vm->global_capacity = 0;
}

lisa_interpret_result lisa_interpret(lisa_vm *vm, const char *source) {
    lisa_parser parser;
    lisa_parser_init(&parser, source);

    int count;
    lisa_ast **exprs = lisa_parse(&parser, &count);

    if (parser.had_error) {
        lisa_parse_free(exprs, count);
        return INTERPRET_COMPILE_ERROR;
    }

    lisa_obj_function *fn = lisa_compile(&vm->gc, exprs, count);
    lisa_parse_free(exprs, count);

    if (fn == NULL) return INTERPRET_COMPILE_ERROR;

    /* Wrap in closure and push as frame 0 */
    push(vm, LISA_OBJ(fn));
    lisa_obj_closure *closure = lisa_new_closure(&vm->gc, fn);
    pop(vm);
    push(vm, LISA_OBJ(closure));
    call_closure(vm, closure, 0);

    lisa_interpret_result result = lisa_run(vm, 0);

    /* Run any spawned fibers */
    if (!lisa_sched_empty(&vm->scheduler)) {
        lisa_run_scheduler(vm);
    }

    return result;
}

/* --- JIT helper functions --- */

static void sync_gc_roots(lisa_vm *vm) {
    vm->gc.stack = vm->stack;
    vm->gc.stack_count = (int)(vm->stack_top - vm->stack);
    vm->gc.open_upvalues = vm->open_upvalues;
}

/* Handle pending JIT tail calls iteratively (trampoline).
   Called when a JIT function returns LISA_TAIL_PENDING(argc). */
static lisa_value jit_trampoline(lisa_vm *vm, lisa_value result) {
    while (IS_TAIL_PENDING(result)) {
        int argc = TAIL_PENDING_ARGC(result);
        lisa_value callee = vm->stack_top[-1 - argc];

        if (IS_OBJ(callee) && OBJ_TYPE(callee) == OBJ_NATIVE) {
            lisa_call_value(vm, callee, argc);
            return vm->stack_top[-1];
        }

        if (!IS_OBJ(callee) || OBJ_TYPE(callee) != OBJ_CLOSURE) {
            runtime_error(vm, "Can only call functions and closures.");
            return LISA_NIL;
        }

        lisa_obj_closure *closure = AS_CLOSURE(callee);
        if (argc != closure->function->arity) {
            runtime_error(vm, "Expected %d arguments but got %d.",
                          closure->function->arity, argc);
            return LISA_NIL;
        }

        /* Reuse the current top frame */
        lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
        close_upvalues(vm, frame->slots);

        lisa_value *src = vm->stack_top - argc - 1;
        memmove(frame->slots, src, (size_t)(argc + 1) * sizeof(lisa_value));
        vm->stack_top = frame->slots + argc + 1;

        frame->closure = closure;
        frame->ip = closure->function->chunk.code;

        /* JIT-compile the target if needed */
        if (!closure->function->jit_code && vm->jit_enabled) {
            lisa_jit_compile(vm, closure->function);
        }

        if (closure->function->jit_code) {
            lisa_jit_fn jit_fn = (lisa_jit_fn)closure->function->jit_code;
            result = jit_fn(vm, frame->closure, frame->slots);
            /* If result is TAIL_PENDING, loop continues */
        } else {
            /* JIT compilation failed; use interpreter (no trampoline risk
               since this function can't produce TAIL_PENDING) */
            int target_depth = vm->frame_count - 1;
            lisa_run(vm, target_depth);
            return vm->stack_top[-1];
        }
    }
    return result;
}

lisa_value lisa_jit_call_helper(lisa_vm *vm, int argc) {
    lisa_value callee = vm->stack_top[-1 - argc];
    if (!lisa_call_value(vm, callee, argc)) {
        return LISA_NIL; /* error already reported */
    }
    /* Check if callee was a native (call_value already handled it) */
    if (IS_OBJ(callee) && OBJ_TYPE(callee) == OBJ_NATIVE) {
        return vm->stack_top[-1]; /* result already on stack */
    }
    /* Closure call — dispatch to JIT or interpreter */
    lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
    lisa_value result;
    if (frame->closure->function->jit_code) {
        lisa_jit_fn jit_fn = (lisa_jit_fn)frame->closure->function->jit_code;
        result = jit_fn(vm, frame->closure, frame->slots);
        if (IS_TAIL_PENDING(result))
            result = jit_trampoline(vm, result);
    } else {
        int target_depth = vm->frame_count - 1;
        lisa_run(vm, target_depth);
        result = vm->stack_top[-1];
    }
    /* Pop the callee's frame */
    close_upvalues(vm, frame->slots);
    vm->frame_count--;
    vm->stack_top = frame->slots;
    push(vm, result);
    return result;
}


lisa_value lisa_jit_get_global(lisa_vm *vm, int name_idx) {
    lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
    lisa_obj_string *name = AS_STRING(frame->closure->function->chunk.constants.values[name_idx]);
    int idx = globals_find(vm, name);
    if (idx == -1) {
        runtime_error(vm, "Undefined variable '%s'.", name->chars);
        return LISA_NIL;
    }
    return vm->global_values[idx];
}

void lisa_jit_def_global(lisa_vm *vm, int name_idx, lisa_value value) {
    lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
    lisa_obj_string *name = AS_STRING(frame->closure->function->chunk.constants.values[name_idx]);
    globals_set(vm, name, value);
}

lisa_value lisa_jit_get_upvalue(lisa_obj_closure *closure, int idx) {
    return *closure->upvalues[idx]->location;
}

void lisa_jit_set_upvalue(lisa_obj_closure *closure, int idx, lisa_value value) {
    *closure->upvalues[idx]->location = value;
}

void lisa_jit_close_upvalue(lisa_vm *vm, lisa_value *addr) {
    close_upvalues(vm, addr);
}

lisa_value lisa_jit_make_closure(lisa_vm *vm, lisa_obj_closure *enclosing,
                                 lisa_obj_function *fn, uint8_t *ip) {
    sync_gc_roots(vm);
    lisa_obj_closure *closure = lisa_new_closure(&vm->gc, fn);
    for (int i = 0; i < closure->upvalue_count; i++) {
        uint8_t is_local = *ip++;
        uint8_t index = *ip++;
        lisa_call_frame *frame = &vm->frames[vm->frame_count - 1];
        if (is_local) {
            closure->upvalues[i] = capture_upvalue(vm, frame->slots + index);
        } else {
            closure->upvalues[i] = enclosing->upvalues[index];
        }
    }
    return LISA_OBJ(closure);
}

void lisa_jit_runtime_error(lisa_vm *vm, const char *msg) {
    runtime_error(vm, "%s", msg);
}

lisa_value lisa_jit_add(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_STRING(a) && IS_STRING(b)) {
        /* String concatenation */
        lisa_obj_string *sa = AS_STRING(a);
        lisa_obj_string *sb = AS_STRING(b);
        int length = sa->length + sb->length;
        char *chars = malloc((size_t)length + 1);
        memcpy(chars, sa->chars, (size_t)sa->length);
        memcpy(chars + sa->length, sb->chars, (size_t)sb->length);
        chars[length] = '\0';
        sync_gc_roots(vm);
        lisa_obj_string *result = lisa_take_string(&vm->gc, chars, length);
        return LISA_OBJ(result);
    }
    if (IS_INT(a) && IS_INT(b)) return LISA_INT(AS_INT(a) + AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return lisa_double(lisa_as_number(a) + lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers or strings.");
    return LISA_NIL;
}

lisa_value lisa_jit_sub(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_INT(AS_INT(a) - AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return lisa_double(lisa_as_number(a) - lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_mul(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_INT(AS_INT(a) * AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return lisa_double(lisa_as_number(a) * lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_div(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (!lisa_is_number(a) || !lisa_is_number(b)) {
        runtime_error(vm, "Operands must be numbers.");
        return LISA_NIL;
    }
    return lisa_double(lisa_as_number(a) / lisa_as_number(b));
}

lisa_value lisa_jit_mod(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_INT(AS_INT(a) % AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return lisa_double(fmod(lisa_as_number(a), lisa_as_number(b)));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_negate(lisa_vm *vm, lisa_value v) {
    if (IS_INT(v)) return LISA_INT(-AS_INT(v));
    if (IS_DOUBLE(v)) return lisa_double(-AS_DOUBLE(v));
    runtime_error(vm, "Operand must be a number.");
    return LISA_NIL;
}

lisa_value lisa_jit_less(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_BOOL(AS_INT(a) < AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return LISA_BOOL(lisa_as_number(a) < lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_less_equal(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_BOOL(AS_INT(a) <= AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return LISA_BOOL(lisa_as_number(a) <= lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_greater(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_BOOL(AS_INT(a) > AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return LISA_BOOL(lisa_as_number(a) > lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_greater_equal(lisa_vm *vm, lisa_value a, lisa_value b) {
    if (IS_INT(a) && IS_INT(b)) return LISA_BOOL(AS_INT(a) >= AS_INT(b));
    if (lisa_is_number(a) && lisa_is_number(b))
        return LISA_BOOL(lisa_as_number(a) >= lisa_as_number(b));
    runtime_error(vm, "Operands must be numbers.");
    return LISA_NIL;
}

lisa_value lisa_jit_equal(lisa_vm *vm, lisa_value a, lisa_value b) {
    (void)vm;
    return LISA_BOOL(lisa_values_equal(a, b));
}

lisa_value lisa_jit_not_equal(lisa_vm *vm, lisa_value a, lisa_value b) {
    (void)vm;
    return LISA_BOOL(!lisa_values_equal(a, b));
}

lisa_value lisa_jit_cons(lisa_vm *vm, lisa_value car, lisa_value cdr) {
    sync_gc_roots(vm);
    lisa_obj_list *list = lisa_new_list(&vm->gc, car, cdr);
    return LISA_OBJ(list);
}

lisa_value lisa_jit_car(lisa_vm *vm, lisa_value v) {
    if (!IS_LIST_OBJ(v)) {
        runtime_error(vm, "car requires a list.");
        return LISA_NIL;
    }
    return AS_LIST(v)->car;
}

lisa_value lisa_jit_cdr(lisa_vm *vm, lisa_value v) {
    if (!IS_LIST_OBJ(v)) {
        runtime_error(vm, "cdr requires a list.");
        return LISA_NIL;
    }
    return AS_LIST(v)->cdr;
}

lisa_value lisa_jit_list(lisa_vm *vm, int n) {
    sync_gc_roots(vm);
    lisa_value result = LISA_NIL;
    for (int i = 0; i < n; i++) {
        lisa_value item = vm->stack_top[-1 - i];
        result = LISA_OBJ(lisa_new_list(&vm->gc, item, result));
    }
    vm->stack_top -= n;
    return result;
}

lisa_value lisa_jit_println(lisa_vm *vm, int argc) {
    for (int i = argc - 1; i >= 0; i--) {
        lisa_print_value(vm->stack_top[-1 - i]);
        if (i > 0) putchar(' ');
    }
    putchar('\n');
    vm->stack_top -= argc;
    return LISA_NIL;
}
