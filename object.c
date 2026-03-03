#include "object.h"
#include "fiber.h"
#include "jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Hashing --- */

static uint32_t hash_string(const char *key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

/* --- Allocation helpers --- */

static lisa_obj *allocate_object(lisa_gc *gc, size_t size, lisa_obj_type type) {
    lisa_obj *obj = malloc(size);
    obj->type = type;
    obj->is_marked = false;
    obj->next = gc->objects;
    gc->objects = obj;
    gc->bytes_allocated += size;
    return obj;
}

#define ALLOCATE_OBJ(gc, type, obj_type) \
    ((type*)allocate_object(gc, sizeof(type), obj_type))

/* --- String interning --- */

static void string_table_grow(lisa_gc *gc) {
    int new_cap = gc->string_capacity < 8 ? 8 : gc->string_capacity * 2;
    lisa_obj_string **new_table = calloc((size_t)new_cap, sizeof(lisa_obj_string*));

    /* Rehash */
    for (int i = 0; i < gc->string_capacity; i++) {
        lisa_obj_string *s = gc->strings[i];
        if (s == NULL) continue;
        int idx = (int)(s->hash % (uint32_t)new_cap);
        while (new_table[idx] != NULL) {
            idx = (idx + 1) % new_cap;
        }
        new_table[idx] = s;
    }

    free(gc->strings);
    gc->strings = new_table;
    gc->string_capacity = new_cap;
}

static lisa_obj_string *string_table_find(lisa_gc *gc, const char *chars, int length, uint32_t hash) {
    if (gc->string_count == 0) return NULL;

    int idx = (int)(hash % (uint32_t)gc->string_capacity);
    for (;;) {
        lisa_obj_string *s = gc->strings[idx];
        if (s == NULL) return NULL;
        if (s->length == length && s->hash == hash &&
            memcmp(s->chars, chars, (size_t)length) == 0) {
            return s;
        }
        idx = (idx + 1) % gc->string_capacity;
    }
}

static void string_table_set(lisa_gc *gc, lisa_obj_string *str) {
    if (gc->string_count + 1 > gc->string_capacity * 3 / 4) {
        string_table_grow(gc);
    }
    int idx = (int)(str->hash % (uint32_t)gc->string_capacity);
    while (gc->strings[idx] != NULL) {
        idx = (idx + 1) % gc->string_capacity;
    }
    gc->strings[idx] = str;
    gc->string_count++;
}

static void string_table_remove(lisa_gc *gc, lisa_obj_string *str) {
    if (gc->string_count == 0) return;
    int idx = (int)(str->hash % (uint32_t)gc->string_capacity);
    for (;;) {
        if (gc->strings[idx] == NULL) return;
        if (gc->strings[idx] == str) {
            gc->strings[idx] = NULL;
            gc->string_count--;
            /* Rehash subsequent entries to fix linear probing chain */
            idx = (idx + 1) % gc->string_capacity;
            while (gc->strings[idx] != NULL) {
                lisa_obj_string *rehash = gc->strings[idx];
                gc->strings[idx] = NULL;
                gc->string_count--;
                string_table_set(gc, rehash);
                idx = (idx + 1) % gc->string_capacity;
            }
            return;
        }
        idx = (idx + 1) % gc->string_capacity;
    }
}

/* --- String creation --- */

static lisa_obj_string *allocate_string(lisa_gc *gc, char *chars, int length, uint32_t h) {
    lisa_obj_string *str = (lisa_obj_string*)allocate_object(
        gc, sizeof(lisa_obj_string) + (size_t)length + 1, OBJ_STRING);
    str->length = length;
    str->hash = h;
    memcpy(str->chars, chars, (size_t)length);
    str->chars[length] = '\0';
    string_table_set(gc, str);
    return str;
}

lisa_obj_string *lisa_copy_string(lisa_gc *gc, const char *chars, int length) {
    uint32_t h = hash_string(chars, length);
    lisa_obj_string *interned = string_table_find(gc, chars, length, h);
    if (interned != NULL) return interned;
    return allocate_string(gc, (char*)chars, length, h);
}

lisa_obj_string *lisa_take_string(lisa_gc *gc, char *chars, int length) {
    uint32_t h = hash_string(chars, length);
    lisa_obj_string *interned = string_table_find(gc, chars, length, h);
    if (interned != NULL) {
        free(chars);
        return interned;
    }
    return allocate_string(gc, chars, length, h);
}

/* --- Object creation --- */

lisa_obj_function *lisa_new_function(lisa_gc *gc) {
    lisa_obj_function *fn = ALLOCATE_OBJ(gc, lisa_obj_function, OBJ_FUNCTION);
    fn->arity = 0;
    fn->upvalue_count = 0;
    fn->name = NULL;
    fn->jit_code = NULL;
    fn->jit_ctx = NULL;
    lisa_chunk_init(&fn->chunk);
    return fn;
}

lisa_obj_closure *lisa_new_closure(lisa_gc *gc, lisa_obj_function *function) {
    lisa_obj_upvalue **upvalues = malloc(sizeof(lisa_obj_upvalue*) * (size_t)function->upvalue_count);
    for (int i = 0; i < function->upvalue_count; i++) {
        upvalues[i] = NULL;
    }
    lisa_obj_closure *closure = ALLOCATE_OBJ(gc, lisa_obj_closure, OBJ_CLOSURE);
    closure->function = function;
    closure->upvalues = upvalues;
    closure->upvalue_count = function->upvalue_count;
    gc->bytes_allocated += sizeof(lisa_obj_upvalue*) * (size_t)function->upvalue_count;
    return closure;
}

lisa_obj_upvalue *lisa_new_upvalue(lisa_gc *gc, lisa_value *slot) {
    lisa_obj_upvalue *upvalue = ALLOCATE_OBJ(gc, lisa_obj_upvalue, OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = LISA_NIL;
    upvalue->next = NULL;
    return upvalue;
}

lisa_obj_list *lisa_new_list(lisa_gc *gc, lisa_value car, lisa_value cdr) {
    lisa_obj_list *list = ALLOCATE_OBJ(gc, lisa_obj_list, OBJ_LIST);
    list->car = car;
    list->cdr = cdr;
    return list;
}

lisa_obj_native *lisa_new_native(lisa_gc *gc, lisa_native_fn function, const char *name, int arity) {
    lisa_obj_native *native = ALLOCATE_OBJ(gc, lisa_obj_native, OBJ_NATIVE);
    native->function = function;
    native->name = name;
    native->arity = arity;
    return native;
}

/* --- Printing --- */

static void print_list(FILE *f, lisa_obj_list *list) {
    fprintf(f, "(");
    lisa_fprint_value(f, list->car);
    lisa_value rest = list->cdr;
    while (IS_LIST_OBJ(rest)) {
        fprintf(f, " ");
        lisa_fprint_value(f, AS_LIST(rest)->car);
        rest = AS_LIST(rest)->cdr;
    }
    if (!IS_NIL(rest)) {
        fprintf(f, " . ");
        lisa_fprint_value(f, rest);
    }
    fprintf(f, ")");
}

void lisa_print_object(FILE *f, lisa_value value) {
    switch (OBJ_TYPE(value)) {
    case OBJ_STRING:
        fprintf(f, "%s", AS_CSTRING(value));
        break;
    case OBJ_FUNCTION:
        if (AS_FUNCTION(value)->name == NULL) {
            fprintf(f, "<script>");
        } else {
            fprintf(f, "<fn %s>", AS_FUNCTION(value)->name->chars);
        }
        break;
    case OBJ_CLOSURE:
        if (AS_CLOSURE(value)->function->name == NULL) {
            fprintf(f, "<script>");
        } else {
            fprintf(f, "<fn %s>", AS_CLOSURE(value)->function->name->chars);
        }
        break;
    case OBJ_UPVALUE:
        fprintf(f, "<upvalue>");
        break;
    case OBJ_LIST:
        print_list(f, AS_LIST(value));
        break;
    case OBJ_NATIVE:
        fprintf(f, "<native %s>", AS_NATIVE(value)->name);
        break;
    case OBJ_FIBER:
        fprintf(f, "<fiber>");
        break;
    case OBJ_CHANNEL:
        fprintf(f, "<channel>");
        break;
    }
}

/* --- GC --- */

static void mark_object(lisa_obj *obj);

static void mark_value(lisa_value value) {
    if (IS_OBJ(value)) mark_object(AS_OBJ(value));
}

static void mark_object(lisa_obj *obj) {
    if (obj == NULL || obj->is_marked) return;
    obj->is_marked = true;

    switch (obj->type) {
    case OBJ_STRING:
        break;
    case OBJ_UPVALUE:
        mark_value(((lisa_obj_upvalue*)obj)->closed);
        break;
    case OBJ_FUNCTION: {
        lisa_obj_function *fn = (lisa_obj_function*)obj;
        if (fn->name) mark_object((lisa_obj*)fn->name);
        for (int i = 0; i < fn->chunk.constants.count; i++) {
            mark_value(fn->chunk.constants.values[i]);
        }
        break;
    }
    case OBJ_CLOSURE: {
        lisa_obj_closure *closure = (lisa_obj_closure*)obj;
        mark_object((lisa_obj*)closure->function);
        for (int i = 0; i < closure->upvalue_count; i++) {
            if (closure->upvalues[i]) {
                mark_object((lisa_obj*)closure->upvalues[i]);
            }
        }
        break;
    }
    case OBJ_LIST: {
        lisa_obj_list *list = (lisa_obj_list*)obj;
        mark_value(list->car);
        mark_value(list->cdr);
        break;
    }
    case OBJ_NATIVE:
        break;
    case OBJ_FIBER: {
        lisa_fiber *fiber = (lisa_fiber*)obj;
        /* Mark fiber's stack values */
        if (fiber->stack) {
            for (lisa_value *slot = fiber->stack; slot < fiber->stack_top; slot++)
                mark_value(*slot);
        }
        /* Mark fiber's open upvalues */
        for (lisa_obj_upvalue *uv2 = fiber->open_upvalues; uv2; uv2 = uv2->next)
            mark_object((lisa_obj*)uv2);
        /* Mark frame closures */
        for (int i = 0; i < fiber->frame_count; i++)
            mark_object((lisa_obj*)fiber->frames[i].closure);
        /* Mark entry closure */
        if (fiber->entry) mark_object((lisa_obj*)fiber->entry);
        mark_value(fiber->result);
        break;
    }
    case OBJ_CHANNEL: {
        lisa_channel *ch = (lisa_channel*)obj;
        mark_value(ch->value);
        if (ch->sender) mark_object((lisa_obj*)ch->sender);
        if (ch->receiver) mark_object((lisa_obj*)ch->receiver);
        break;
    }
    }
}

static void mark_roots(lisa_gc *gc) {
    /* Mark current stack values */
    for (int i = 0; i < gc->stack_count; i++) {
        mark_value(gc->stack[i]);
    }
    /* Mark open upvalues */
    lisa_obj_upvalue *uv = gc->open_upvalues;
    while (uv != NULL) {
        mark_object((lisa_obj*)uv);
        uv = uv->next;
    }
    /* Mark all live fibers (traverses each fiber's stack/frames/upvalues) */
    for (lisa_fiber *f = gc->all_fibers; f != NULL; f = f->next_fiber) {
        mark_object((lisa_obj*)f);
    }
}

static void free_object(lisa_gc *gc, lisa_obj *obj) {
    switch (obj->type) {
    case OBJ_STRING: {
        lisa_obj_string *str = (lisa_obj_string*)obj;
        gc->bytes_allocated -= sizeof(lisa_obj_string) + (size_t)str->length + 1;
        string_table_remove(gc, str);
        free(obj);
        break;
    }
    case OBJ_FUNCTION: {
        lisa_obj_function *fn = (lisa_obj_function*)obj;
        lisa_jit_free(fn);
        lisa_chunk_free(&fn->chunk);
        gc->bytes_allocated -= sizeof(lisa_obj_function);
        free(obj);
        break;
    }
    case OBJ_CLOSURE: {
        lisa_obj_closure *closure = (lisa_obj_closure*)obj;
        gc->bytes_allocated -= sizeof(lisa_obj_upvalue*) * (size_t)closure->upvalue_count;
        free(closure->upvalues);
        gc->bytes_allocated -= sizeof(lisa_obj_closure);
        free(obj);
        break;
    }
    case OBJ_UPVALUE:
        gc->bytes_allocated -= sizeof(lisa_obj_upvalue);
        free(obj);
        break;
    case OBJ_LIST:
        gc->bytes_allocated -= sizeof(lisa_obj_list);
        free(obj);
        break;
    case OBJ_NATIVE:
        gc->bytes_allocated -= sizeof(lisa_obj_native);
        free(obj);
        break;
    case OBJ_FIBER: {
        lisa_fiber *fiber = (lisa_fiber*)obj;
        lisa_fiber_free_stacks(fiber);
        gc->bytes_allocated -= sizeof(lisa_fiber);
        free(obj);
        break;
    }
    case OBJ_CHANNEL:
        gc->bytes_allocated -= sizeof(lisa_channel);
        free(obj);
        break;
    }
}

static void sweep(lisa_gc *gc) {
    lisa_obj *prev = NULL;
    lisa_obj *obj = gc->objects;
    while (obj != NULL) {
        if (obj->is_marked) {
            obj->is_marked = false;
            prev = obj;
            obj = obj->next;
        } else {
            lisa_obj *unreached = obj;
            obj = obj->next;
            if (prev != NULL) {
                prev->next = obj;
            } else {
                gc->objects = obj;
            }
            free_object(gc, unreached);
        }
    }
}

static void rebuild_fiber_list(lisa_gc *gc) {
    /* Rebuild the all_fibers linked list from surviving objects */
    gc->all_fibers = NULL;
    for (lisa_obj *obj = gc->objects; obj != NULL; obj = obj->next) {
        if (obj->type == OBJ_FIBER) {
            lisa_fiber *f = (lisa_fiber *)obj;
            f->next_fiber = gc->all_fibers;
            gc->all_fibers = f;
        }
    }
}

void lisa_gc_collect(lisa_gc *gc) {
    mark_roots(gc);
    sweep(gc);
    rebuild_fiber_list(gc);
    gc->next_gc = gc->bytes_allocated * 2;
}

void lisa_gc_init(lisa_gc *gc) {
    gc->objects = NULL;
    gc->strings = NULL;
    gc->string_count = 0;
    gc->string_capacity = 0;
    gc->bytes_allocated = 0;
    gc->next_gc = 1024 * 1024;
    gc->stack = NULL;
    gc->stack_count = 0;
    gc->open_upvalues = NULL;
    gc->all_fibers = NULL;
}

void lisa_gc_free(lisa_gc *gc) {
    /* Free all objects */
    lisa_obj *obj = gc->objects;
    while (obj != NULL) {
        lisa_obj *next = obj->next;
        free_object(gc, obj);
        obj = next;
    }
    free(gc->strings);
    lisa_gc_init(gc);
}
