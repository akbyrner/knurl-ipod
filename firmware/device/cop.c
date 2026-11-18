/*
 * The second core (COP) runs the decoder: engine_step() whenever there's room in the audio
 * ring and data to decode, and sleeps otherwise. The CPU wakes it from its main loop
 * (platform_cop_kick); a wake that arrives just as it goes to sleep is simply picked up by
 * the next kick, a few milliseconds later, well inside the ring's 372 ms.
 */
#include "hw.h"
#include "platform.h"
#include "shared.h"
#include "engine.h"

void cache_init(uint32_t priority);

/* Read by crt0.S's COP idle loop. In IRAM, so the CPU's write is seen without cache flushes. */
void (*volatile cop_entry)(void) IRAM_DATA;

static void cop_main(void)
{
    cache_init(0x20);                     /* the COP's own cache (Rockbox: priority 0x20) */
    for (;;) {
        if (!engine_step()) {
            COP_CTL = PROC_SLEEP;
            __asm__ volatile("nop\n nop\n nop");
        }
    }
}

void cop_start(void)
{
    cop_entry = cop_main;
    COP_CTL = 0;                          /* wake */
}

void platform_cop_kick(void) { COP_CTL = 0; }
