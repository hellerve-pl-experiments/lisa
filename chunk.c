#include "chunk.h"
#include <stdlib.h>

void lisa_value_array_init(lisa_value_array *arr) {
    arr->count = 0;
    arr->capacity = 0;
    arr->values = NULL;
}

void lisa_value_array_write(lisa_value_array *arr, lisa_value value) {
    if (arr->count >= arr->capacity) {
        arr->capacity = arr->capacity < 8 ? 8 : arr->capacity * 2;
        arr->values = realloc(arr->values, sizeof(lisa_value) * (size_t)arr->capacity);
    }
    arr->values[arr->count++] = value;
}

void lisa_value_array_free(lisa_value_array *arr) {
    free(arr->values);
    lisa_value_array_init(arr);
}

void lisa_chunk_init(lisa_chunk *chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    lisa_value_array_init(&chunk->constants);
}

void lisa_chunk_write(lisa_chunk *chunk, uint8_t byte, int line) {
    if (chunk->count >= chunk->capacity) {
        chunk->capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code = realloc(chunk->code, sizeof(uint8_t) * (size_t)chunk->capacity);
        chunk->lines = realloc(chunk->lines, sizeof(int) * (size_t)chunk->capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void lisa_chunk_free(lisa_chunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    lisa_value_array_free(&chunk->constants);
    lisa_chunk_init(chunk);
}

int lisa_chunk_add_constant(lisa_chunk *chunk, lisa_value value) {
    lisa_value_array_write(&chunk->constants, value);
    return chunk->constants.count - 1;
}
