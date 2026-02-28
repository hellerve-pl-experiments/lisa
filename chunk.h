#ifndef LISA_CHUNK_H
#define LISA_CHUNK_H

#include "value.h"
#include <stdint.h>

typedef enum {
    OP_CONSTANT,      /* [idx]          push constants[idx] */
    OP_NIL,           /*                push nil */
    OP_TRUE,          /*                push true */
    OP_FALSE,         /*                push false */
    OP_POP,           /*                pop top */

    OP_GET_LOCAL,     /* [slot]         push stack[base+slot] */
    OP_SET_LOCAL,     /* [slot]         stack[base+slot] = peek */
    OP_GET_UPVALUE,   /* [idx]          push *upvalues[idx]->location */
    OP_SET_UPVALUE,   /* [idx]          *upvalues[idx]->location = peek */
    OP_GET_GLOBAL,    /* [idx]          push globals[constants[idx]] */
    OP_DEF_GLOBAL,    /* [idx]          globals[constants[idx]] = pop */

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEGATE,

    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,

    OP_NOT,

    OP_JUMP,          /* [lo][hi]       ip += offset */
    OP_JUMP_IF_FALSE, /* [lo][hi]       if falsey(pop) ip += offset */
    OP_LOOP,          /* [lo][hi]       ip -= offset */

    OP_CLOSURE,       /* [idx] then pairs of [is_local, index] */
    OP_CALL,          /* [argc]         call top function with argc args */
    OP_TAIL_CALL,     /* [argc]         tail call: reuse current frame */
    OP_RETURN,        /*                return top of stack */

    OP_CLOSE_UPVALUE, /*                close upvalue at stack top */

    OP_CONS,          /*                push cons(pop2, pop1) */
    OP_CAR,           /*                push car(pop) */
    OP_CDR,           /*                push cdr(pop) */
    OP_LIST,          /* [n]            pop n items, build list */

    OP_PRINTLN,       /* [argc]         print argc values with spaces, newline */
} lisa_op;

/* Dynamic array of constants */
typedef struct {
    int count;
    int capacity;
    lisa_value *values;
} lisa_value_array;

void lisa_value_array_init(lisa_value_array *arr);
void lisa_value_array_write(lisa_value_array *arr, lisa_value value);
void lisa_value_array_free(lisa_value_array *arr);

/* Bytecode chunk */
typedef struct {
    int count;
    int capacity;
    uint8_t *code;
    int *lines;          /* source line per bytecode byte */
    lisa_value_array constants;
} lisa_chunk;

void lisa_chunk_init(lisa_chunk *chunk);
void lisa_chunk_write(lisa_chunk *chunk, uint8_t byte, int line);
void lisa_chunk_free(lisa_chunk *chunk);
int lisa_chunk_add_constant(lisa_chunk *chunk, lisa_value value);

#endif
