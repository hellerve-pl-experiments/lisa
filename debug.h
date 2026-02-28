#ifndef LISA_DEBUG_H
#define LISA_DEBUG_H

#include "chunk.h"

void lisa_disassemble_chunk(lisa_chunk *chunk, const char *name);
int lisa_disassemble_instruction(lisa_chunk *chunk, int offset);

#endif
