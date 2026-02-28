#include "value.h"
#include "object.h"
#include <stdio.h>
#include <string.h>

void lisa_fprint_value(FILE *f, lisa_value value) {
    if (IS_NIL(value)) {
        fprintf(f, "nil");
    } else if (IS_BOOL(value)) {
        fprintf(f, AS_BOOL(value) ? "true" : "false");
    } else if (IS_INT(value)) {
        fprintf(f, "%lld", (long long)AS_INT(value));
    } else if (IS_DOUBLE(value)) {
        double d = AS_DOUBLE(value);
        /* Print integers without decimal point */
        if (d == (int64_t)d && d >= -1e15 && d <= 1e15) {
            fprintf(f, "%.1f", d);
        } else {
            fprintf(f, "%g", d);
        }
    } else if (IS_OBJ(value)) {
        lisa_print_object(f, value);
    } else {
        fprintf(f, "<unknown>");
    }
}

void lisa_print_value(lisa_value value) {
    lisa_fprint_value(stdout, value);
}

bool lisa_values_equal(lisa_value a, lisa_value b) {
    if (IS_DOUBLE(a) && IS_DOUBLE(b)) {
        return AS_DOUBLE(a) == AS_DOUBLE(b);
    }
    if (IS_INT(a) && IS_DOUBLE(b)) {
        return (double)AS_INT(a) == AS_DOUBLE(b);
    }
    if (IS_DOUBLE(a) && IS_INT(b)) {
        return AS_DOUBLE(a) == (double)AS_INT(b);
    }
    /* For NaN-boxed values, bit equality works for nil, bool, int, and
     * interned strings (same pointer = same string). */
    return a == b;
}

bool lisa_is_falsey(lisa_value value) {
    if (IS_NIL(value)) return true;
    if (IS_BOOL(value)) return !AS_BOOL(value);
    return false;
}
