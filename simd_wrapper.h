#ifndef SIMD_WRAPPER_H
#define SIMD_WRAPPER_H

#include "logana/simd.h"
#include <stdlib.h>

// Safe aligned allocation helper for SIMD buffers
static inline void *simd_aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

#ifndef SAFE_FREE
#define SAFE_FREE(ptr) do { free(ptr); ptr = NULL; } while(0)
#endif

#endif
