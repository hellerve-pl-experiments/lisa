#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file '%s'.\n", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *buffer = malloc((size_t)size + 1);
    size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    buffer[bytes_read] = '\0';

    fclose(file);
    return buffer;
}

static int run_string(const char *source, bool jit) {
    lisa_vm vm;
    lisa_vm_init(&vm);
    vm.jit_enabled = jit;
    lisa_interpret_result result = lisa_interpret(&vm, source);
    lisa_vm_free(&vm);

    if (result == INTERPRET_COMPILE_ERROR) return 65;
    if (result == INTERPRET_RUNTIME_ERROR) return 70;
    return 0;
}

static int run_file(const char *path, bool jit) {
    char *source = read_file(path);
    if (source == NULL) return 74;
    int result = run_string(source, jit);
    free(source);
    return result;
}

int main(int argc, char *argv[]) {
    bool jit = true;
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "--no-jit") == 0) {
        jit = false;
        argi++;
    }

    if (argi < argc && argi == argc - 1 && strcmp(argv[argi], "-e") != 0) {
        return run_file(argv[argi], jit);
    }

    if (argi + 1 < argc && strcmp(argv[argi], "-e") == 0) {
        return run_string(argv[argi + 1], jit);
    }

    fprintf(stderr, "Usage: lisa [--no-jit] <file.lisa>\n");
    fprintf(stderr, "       lisa [--no-jit] -e \"<expression>\"\n");
    return 64;
}
