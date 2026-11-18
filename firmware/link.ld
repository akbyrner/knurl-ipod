/* The Rockbox bootloader loads the image at 0x10000000 and jumps there; crt0.S then maps
 * that SDRAM to logical 0 (the only cacheable range) and continues at the linked address.
 * The iPod Video has 32 or 64 MB; this map stays well inside 32 MB. */
ENTRY(_start)

MEMORY
{
    DRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 30M   /* SDRAM, remapped to 0 by crt0.S so it's cached */
    IRAM (rwx) : ORIGIN = 0x40000100, LENGTH = 96K - 0x100   /* on-chip RAM: fast, never cached (the first 256 bytes hold the boot remap stub) */
}

SECTIONS
{
    .text : {
        KEEP(*(.init))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > DRAM

    .data : {
        *(.data*)
        . = ALIGN(4);
    } > DRAM

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DRAM

    /* Stacks are real sections: a bare ". = . + size" here does NOT move the next section
     * placed in DRAM, and in Milestone 2's first build the memory pool landed on the stack. */
    .stack (NOLOAD) : {
        . = ALIGN(8);
        . = . + 0x10000;       /* 64 KB main stack */
        _stack_top = .;
        . = . + 0x1000;        /* 4 KB for the crash handler */
        _crash_stack_top = .;
        . = . + 0x400;         /* 1 KB for the audio FIQ */
        _fiq_stack_top = .;
        . = . + 0x1000;        /* 4 KB for a crash on the COP */
        _cop_crash_stack_top = .;
    } > DRAM

    .noinit (NOLOAD) : {       /* big buffers (the memory pool); not cleared at boot */
        . = ALIGN(8);
        *(.noinit*)
    } > DRAM

    /* Decoder-core (COP) data and stack, and the CPU->COP handoff word: IRAM, never cached,
     * so nothing in it shares a cache line with the CPU's data (the cores' caches aren't
     * coherent). Zeroed by crt0.S after the COP has remapped. */
    .ibss (NOLOAD) : {
        _ibss_start = .;
        *(.ibss*)
        . = ALIGN(8);
        . = . + 0x2000;        /* 8 KB COP stack */
        _cop_stack_top = .;
        _ibss_end = .;
    } > IRAM

    ASSERT(ADDR(.noinit) >= _cop_crash_stack_top, "memory pool overlaps the stacks")
    ASSERT(_ibss_end <= 0x40018000, "IRAM is full")
    ASSERT(ADDR(.stack) >= __bss_end, "stack overlaps .bss")
    ASSERT(ADDR(.noinit) + SIZEOF(.noinit) <= 0x02000000, "image does not fit the 32 MB cached window")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
