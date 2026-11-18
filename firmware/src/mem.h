#pragma once
#include <stddef.h>

/* One-way allocator over a big block of RAM that isn't zeroed at boot (zeroing megabytes
 * before the cache is on would cost seconds). Nothing is ever freed. 8-byte aligned. */
void *mem_alloc(size_t n);
size_t mem_used(void);
