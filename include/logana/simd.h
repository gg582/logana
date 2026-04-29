#ifndef LOGANA_SIMD_H
#define LOGANA_SIMD_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOGANA_SIMD_SCALAR = 0,
    LOGANA_SIMD_AVX2,
    LOGANA_SIMD_AVX512,
    LOGANA_SIMD_NEON,
    LOGANA_SIMD_RVV
} logana_simd_isa_t;

static inline logana_simd_isa_t logana_simd_detect(void) {
#if defined(__AVX512F__)
    return LOGANA_SIMD_AVX512;
#elif defined(__AVX2__)
    return LOGANA_SIMD_AVX2;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return LOGANA_SIMD_NEON;
#elif defined(__riscv_vector)
    return LOGANA_SIMD_RVV;
#else
    return LOGANA_SIMD_SCALAR;
#endif
}

static inline size_t logana_simd_count_byte(const uint8_t *data, size_t len, uint8_t needle) {
    size_t matches = 0;
    for (size_t i = 0; i < len; ++i) {
        matches += data[i] == needle;
    }
    return matches;
}

static inline double logana_simd_sum_f32(const float *values, size_t len) {
    double total = 0.0;
    for (size_t i = 0; i < len; ++i) {
        total += values[i];
    }
    return total;
}

#endif
