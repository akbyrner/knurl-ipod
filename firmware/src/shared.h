/*
 * Memory shared between the iPod's two cores. Each core has its own write-back cache and
 * they are not coherent, so:
 *   SHARED    data both cores touch: lives in SDRAM, always accessed through UNCACHED()
 *   COP_DATA  data only the decoder core touches: on-chip IRAM, never cached, so it can't
 *             share a cache line with anything the CPU writes.
 *   IRAM_DATA the CPU->COP handoff word (also IRAM).
 * Measured on the iPod (decoder load on the COP): code cached in SDRAM + data in IRAM 66%;
 * code and data in IRAM 76%; code and data cached in SDRAM 72%.
 * In the simulator (one core) these are ordinary variables.
 */
#pragma once
#include <stdint.h>

#ifdef __arm__
#define UNCACHED(p) ((void *)((uintptr_t)(p) | 0x10000000u))   /* SDRAM's physical alias */
#define SHARED      __attribute__((section(".noinit.shared"), aligned(32)))
#define COP_DATA    __attribute__((section(".ibss"), aligned(8)))
#define IRAM_DATA   __attribute__((section(".ibss"), aligned(8)))
#else
#define UNCACHED(p) ((void *)(p))
#define SHARED
#define COP_DATA
#define IRAM_DATA
#endif
