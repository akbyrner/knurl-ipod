#include "mem.h"
#include <stdint.h>

#define POOL_SIZE (24u << 20)

/* .noinit isn't cleared by crt0.S (the simulator just gets it from its BSS). */
#ifdef __arm__
static uint8_t pool[POOL_SIZE] __attribute__((section(".noinit"), aligned(8)));
#else
static uint8_t pool[POOL_SIZE] __attribute__((aligned(8)));
#endif
static size_t used;

void *mem_alloc(size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (n > POOL_SIZE - used) return 0;
    void *p = pool + used;
    used += n;
    return p;
}

size_t mem_used(void) { return used; }
