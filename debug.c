#include "debug.h"
#include "object.h"
#include <stdio.h>

static int simple_instruction(const char *name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

static int byte_instruction(const char *name, lisa_chunk *chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-20s %4d\n", name, slot);
    return offset + 2;
}

static int constant_instruction(const char *name, lisa_chunk *chunk, int offset) {
    uint8_t idx = chunk->code[offset + 1];
    printf("%-20s %4d '", name, idx);
    lisa_print_value(chunk->constants.values[idx]);
    printf("'\n");
    return offset + 2;
}

static int jump_instruction(const char *name, int sign, lisa_chunk *chunk, int offset) {
    uint8_t lo = chunk->code[offset + 1];
    uint8_t hi = chunk->code[offset + 2];
    uint16_t jump = (uint16_t)(lo | (hi << 8));
    printf("%-20s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

void lisa_disassemble_chunk(lisa_chunk *chunk, const char *name) {
    printf("== %s ==\n", name);
    int offset = 0;
    while (offset < chunk->count) {
        offset = lisa_disassemble_instruction(chunk, offset);
    }
}

int lisa_disassemble_instruction(lisa_chunk *chunk, int offset) {
    printf("%04d ", offset);

    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
    case OP_CONSTANT:      return constant_instruction("OP_CONSTANT", chunk, offset);
    case OP_NIL:           return simple_instruction("OP_NIL", offset);
    case OP_TRUE:          return simple_instruction("OP_TRUE", offset);
    case OP_FALSE:         return simple_instruction("OP_FALSE", offset);
    case OP_POP:           return simple_instruction("OP_POP", offset);

    case OP_GET_LOCAL:     return byte_instruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:     return byte_instruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_UPVALUE:   return byte_instruction("OP_GET_UPVALUE", chunk, offset);
    case OP_SET_UPVALUE:   return byte_instruction("OP_SET_UPVALUE", chunk, offset);
    case OP_GET_GLOBAL:    return constant_instruction("OP_GET_GLOBAL", chunk, offset);
    case OP_DEF_GLOBAL:    return constant_instruction("OP_DEF_GLOBAL", chunk, offset);

    case OP_ADD:           return simple_instruction("OP_ADD", offset);
    case OP_SUB:           return simple_instruction("OP_SUB", offset);
    case OP_MUL:           return simple_instruction("OP_MUL", offset);
    case OP_DIV:           return simple_instruction("OP_DIV", offset);
    case OP_MOD:           return simple_instruction("OP_MOD", offset);
    case OP_NEGATE:        return simple_instruction("OP_NEGATE", offset);

    case OP_EQUAL:         return simple_instruction("OP_EQUAL", offset);
    case OP_NOT_EQUAL:     return simple_instruction("OP_NOT_EQUAL", offset);
    case OP_LESS:          return simple_instruction("OP_LESS", offset);
    case OP_LESS_EQUAL:    return simple_instruction("OP_LESS_EQUAL", offset);
    case OP_GREATER:       return simple_instruction("OP_GREATER", offset);
    case OP_GREATER_EQUAL: return simple_instruction("OP_GREATER_EQUAL", offset);

    case OP_NOT:           return simple_instruction("OP_NOT", offset);

    case OP_JUMP:          return jump_instruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE: return jump_instruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:          return jump_instruction("OP_LOOP", -1, chunk, offset);

    case OP_CLOSURE: {
        offset++;
        uint8_t idx = chunk->code[offset++];
        printf("%-20s %4d ", "OP_CLOSURE", idx);
        lisa_print_value(chunk->constants.values[idx]);
        printf("\n");

        lisa_obj_function *fn = AS_FUNCTION(chunk->constants.values[idx]);
        for (int j = 0; j < fn->upvalue_count; j++) {
            int is_local = chunk->code[offset++];
            int index = chunk->code[offset++];
            printf("%04d    |                     %s %d\n",
                   offset - 2, is_local ? "local" : "upvalue", index);
        }
        return offset;
    }

    case OP_CALL:          return byte_instruction("OP_CALL", chunk, offset);
    case OP_TAIL_CALL:     return byte_instruction("OP_TAIL_CALL", chunk, offset);
    case OP_RETURN:        return simple_instruction("OP_RETURN", offset);
    case OP_CLOSE_UPVALUE: return simple_instruction("OP_CLOSE_UPVALUE", offset);

    case OP_CONS:          return simple_instruction("OP_CONS", offset);
    case OP_CAR:           return simple_instruction("OP_CAR", offset);
    case OP_CDR:           return simple_instruction("OP_CDR", offset);
    case OP_LIST:          return byte_instruction("OP_LIST", chunk, offset);

    case OP_PRINTLN:       return byte_instruction("OP_PRINTLN", chunk, offset);

    default:
        printf("Unknown opcode %d\n", instruction);
        return offset + 1;
    }
}
