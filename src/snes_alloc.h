/*
 * murmsnes - Memory allocation wrapper for SNES9x
 * Redirects large allocations to PSRAM
 */
#ifndef SNES_ALLOC_H
#define SNES_ALLOC_H

#include <stddef.h>
#include <string.h>

#ifdef PICO_ON_DEVICE
#include "psram_allocator.h"

// Use PSRAM for all allocations in snes9x
static inline void *snes_malloc(size_t size) {
    return psram_malloc(size);
}

static inline void *snes_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = psram_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static inline void snes_free(void *ptr) {
    psram_free(ptr);
}

static inline void *snes_realloc(void *ptr, size_t size) {
    return psram_realloc(ptr, size);
}

#else
// On non-device (PC), use standard malloc
#include <stdlib.h>
#define snes_malloc malloc
#define snes_calloc calloc
#define snes_free free
#define snes_realloc realloc
#endif

#endif // SNES_ALLOC_H
