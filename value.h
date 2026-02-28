#ifndef LISA_VALUE_H
#define LISA_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * NaN-boxed value representation.
 *
 * IEEE 754 double: if bits [62:52] are all 1 and bit 51 is 1, it's a quiet NaN.
 * We use the remaining payload bits for non-double values.
 *
 * Layout:
 *   Double: any bit pattern that is NOT a quiet NaN with our tag prefix
 *   Tagged: [sign=1][exp=0x7FF][quiet=1][tag 50:48][payload 47:0]
 *
 * Tags (bits 50:48):
 *   000 = nil
 *   001 = bool (payload bit 0)
 *   010 = int  (48-bit sign-extended integer)
 *   011 = object pointer (48-bit)
 */

typedef uint64_t lisa_value;

/* The quiet NaN mask: sign(1) + exponent(0x7FF) + quiet(1) = bits 63,62:52,51 */
#define QNAN    ((uint64_t)0x7FFC000000000000)
#define SIGN_BIT ((uint64_t)0x8000000000000000)

/* Tag values shifted into bits 50:48 */
#define TAG_NIL    ((uint64_t)0x0000000000000000)  /* 000 */
#define TAG_BOOL   ((uint64_t)0x0001000000000000)  /* 001 */
#define TAG_INT    ((uint64_t)0x0002000000000000)  /* 010 */
#define TAG_OBJ    ((uint64_t)0x0003000000000000)  /* 011 */

#define TAG_MASK   ((uint64_t)0x0003000000000000)
#define PAYLOAD_MASK ((uint64_t)0x0000FFFFFFFFFFFF) /* 48 bits */

/* Construct values */
#define LISA_NIL        (QNAN | TAG_NIL)
#define LISA_TRUE       (QNAN | TAG_BOOL | 1)
#define LISA_FALSE      (QNAN | TAG_BOOL | 0)
#define LISA_BOOL(b)    ((b) ? LISA_TRUE : LISA_FALSE)
#define LISA_INT(i)     (QNAN | TAG_INT | ((uint64_t)(i) & PAYLOAD_MASK))
#define LISA_OBJ(ptr)   (QNAN | TAG_OBJ | ((uint64_t)(uintptr_t)(ptr) & PAYLOAD_MASK))

static inline lisa_value lisa_double(double d) {
    union { double d; uint64_t u; } conv;
    conv.d = d;
    return conv.u;
}

/* Type checks */
#define IS_NIL(v)    ((v) == LISA_NIL)
#define IS_BOOL(v)   (((v) & (QNAN | TAG_MASK)) == (QNAN | TAG_BOOL))
#define IS_INT(v)    (((v) & (QNAN | TAG_MASK)) == (QNAN | TAG_INT))
#define IS_OBJ(v)    (((v) & (QNAN | TAG_MASK)) == (QNAN | TAG_OBJ))
#define IS_DOUBLE(v) (((v) & QNAN) != QNAN)

/* Extract values */
#define AS_BOOL(v)   ((v) & 1)

static inline int64_t AS_INT(lisa_value v) {
    /* Sign-extend the 48-bit payload */
    uint64_t raw = v & PAYLOAD_MASK;
    if (raw & ((uint64_t)1 << 47)) {
        raw |= (uint64_t)0xFFFF000000000000;
    }
    return (int64_t)raw;
}

static inline double AS_DOUBLE(lisa_value v) {
    union { uint64_t u; double d; } conv;
    conv.u = v;
    return conv.d;
}

#define AS_OBJ(v)    ((lisa_obj*)(uintptr_t)((v) & PAYLOAD_MASK))

/* Forward declaration */
typedef struct lisa_obj lisa_obj;

/* Functions */
void lisa_print_value(lisa_value value);
void lisa_fprint_value(FILE *f, lisa_value value);
bool lisa_values_equal(lisa_value a, lisa_value b);
bool lisa_is_falsey(lisa_value value);

/* Numeric coercion: returns double for arithmetic on mixed int/double */
static inline double lisa_as_number(lisa_value v) {
    if (IS_INT(v)) return (double)AS_INT(v);
    return AS_DOUBLE(v);
}

static inline bool lisa_is_number(lisa_value v) {
    return IS_INT(v) || IS_DOUBLE(v);
}

#endif
