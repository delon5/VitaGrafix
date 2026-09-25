#include <string.h>

#include "interpreter.h"
#include "parser.h"
#include "op.h"

#define __datatype_raw_cast_op(lhs, tp, sz) {\
    value_cast(lhs, tp);\
    value_raw(lhs, sz);\
}

bool op_datatype_raw_int8(value_t *lhs)   { __datatype_raw_cast_op(lhs, DATA_TYPE_SIGNED, 1); return true; }
bool op_datatype_raw_int16(value_t *lhs)  { __datatype_raw_cast_op(lhs, DATA_TYPE_SIGNED, 2); return true; }
bool op_datatype_raw_int32(value_t *lhs)  { __datatype_raw_cast_op(lhs, DATA_TYPE_SIGNED, 4); return true; }
bool op_datatype_raw_uint8(value_t *lhs)  { __datatype_raw_cast_op(lhs, DATA_TYPE_UNSIGNED, 1); return true; }
bool op_datatype_raw_uint16(value_t *lhs) { __datatype_raw_cast_op(lhs, DATA_TYPE_UNSIGNED, 2); return true; }
bool op_datatype_raw_uint32(value_t *lhs) { __datatype_raw_cast_op(lhs, DATA_TYPE_UNSIGNED, 4); return true; }
bool op_datatype_raw_fl32(value_t *lhs)   { __datatype_raw_cast_op(lhs, DATA_TYPE_FLOAT, 4); return true; }

/*
 * IEEE 754 binary16, round to nearest even, subnormals included. Fails for
 * NaN, infinity and values that round past the largest half (65504). Bit
 * arithmetic only: no libm, no compiler half type.
 */
static bool fl32_to_fl16_bits(float f, uint16_t *out) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    uint16_t sign = (uint16_t)((u >> 16) & 0x8000);
    int32_t exp = (int32_t)((u >> 23) & 0xFF);
    uint32_t man = u & 0x7FFFFF;

    if (exp == 0xFF)
        return false;                       // NaN or infinity
    if (exp == 0 && man == 0) {
        *out = sign;                        // signed zero (f32 subnormals round to 0 as well)
        return true;
    }

    int32_t e = exp - 127 + 15;             // biased half exponent
    uint32_t mant, shift;
    if (e >= 1) {
        mant = man;                         // normal half: keep 10 of 23 bits
        shift = 13;
    } else {
        if (e < -10) {                      // below half the smallest subnormal
            *out = sign;
            return true;
        }
        mant = man | 0x800000;              // subnormal half: implicit bit becomes explicit
        shift = (uint32_t)(14 - e);
        e = 0;
    }

    uint32_t half = mant >> shift;
    uint32_t rest = mant & ((1u << shift) - 1);
    uint32_t mid = 1u << (shift - 1);
    if (rest > mid || (rest == mid && (half & 1)))
        half++;                             // may carry into the exponent, which is correct

    uint32_t bits = ((uint32_t)e << 10) + half;
    if (bits >= 0x7C00)
        return false;                       // rounds to infinity
    *out = (uint16_t)(sign | bits);
    return true;
}

static float fl16_bits_to_fl32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t u;
    if (exp == 0) {
        if (man == 0) {
            u = sign;
        } else {                            // subnormal half -> normal float
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                exp--;
            }
            u = sign | (exp << 23) | ((man & 0x3FF) << 13);
        }
    } else {
        u = sign | ((exp - 15 + 127) << 23) | (man << 13);   // exp 31 cannot occur here
    }
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

bool op_datatype_raw_fl16(value_t *lhs) {
    value_cast(lhs, DATA_TYPE_FLOAT);
    uint16_t bits;
    if (!fl32_to_fl16_bits(lhs->data.fl32, &bits))
        return false;
    value_raw(lhs, 2);
    lhs->data.raw[0] = (byte_t)(bits & 0xFF);
    lhs->data.raw[1] = (byte_t)(bits >> 8);
    memset(lhs->unk, 0, MAX_VALUE_SIZE * sizeof(bool));
    return true;
}
bool op_datatype_raw_bytes(value_t *lhs)  { __datatype_raw_cast_op(lhs, DATA_TYPE_RAW, lhs->size); return true; }
bool op_datatype_raw_bytes_n(value_t *lhs, value_t *rhs) {
    if ((rhs->type != DATA_TYPE_UNSIGNED && rhs->type != DATA_TYPE_SIGNED)
            || (rhs->type == DATA_TYPE_SIGNED && rhs->data.int32 <= 0)
            || rhs->data.uint32 == 0
            || rhs->data.uint32 > MAX_VALUE_SIZE)
        return false;

    __datatype_raw_cast_op(lhs, DATA_TYPE_RAW, rhs->data.uint32);
    return true;
}

bool op_datatype_raw_concat(value_t *lhs, value_t *rhs) {
    if (lhs->size + rhs->size > MAX_VALUE_SIZE) {
        // Won't fit :(
        return false;
    }

    value_raw(lhs, lhs->size);
    value_raw(rhs, rhs->size);

    memcpy(&lhs->data.raw[lhs->size], rhs->data.raw, rhs->size);
    memcpy(&lhs->unk[lhs->size], rhs->unk, rhs->size * sizeof(bool));

    lhs->size += rhs->size;
    lhs->approximated |= rhs->approximated;
    return true;
}

bool op_datatype_cast_int(value_t *lhs)   { value_cast(lhs, DATA_TYPE_SIGNED); return true; }
bool op_datatype_cast_uint(value_t *lhs)  { value_cast(lhs, DATA_TYPE_UNSIGNED); return true; }
bool op_datatype_cast_float(value_t *lhs) { value_cast(lhs, DATA_TYPE_FLOAT); return true; }

// The nearest half-precision value, as a float (the value a shader compiler stores for a half constant)
bool op_datatype_cast_half(value_t *lhs) {
    value_cast(lhs, DATA_TYPE_FLOAT);
    uint16_t bits;
    if (!fl32_to_fl16_bits(lhs->data.fl32, &bits))
        return false;
    lhs->data.fl32 = fl16_bits_to_fl32(bits);
    return true;
}
