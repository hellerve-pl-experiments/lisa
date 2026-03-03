CC ?= cc
CJ_DIR = ../src
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wno-gnu -O2 -I$(CJ_DIR)
DEVFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wno-gnu -Werror -g -O0 -I$(CJ_DIR)
SOURCES = main.c value.c object.c lexer.c parser.c ast.c chunk.c compiler.c vm.c debug.c jit.c fiber.c $(CJ_DIR)/ctx.c
TARGET = bin/lisa

.PHONY: all dev clean

all:
	mkdir -p bin
	$(CC) $(SOURCES) -o $(TARGET) $(CFLAGS) -lm

dev:
	mkdir -p bin
	$(CC) $(SOURCES) -o $(TARGET) $(DEVFLAGS) -lm

clean:
	rm -rf bin
