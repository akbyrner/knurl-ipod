/*
 * knurlboot entry. The boot ROM loads the whole osos image (Apple's firmware with this loader
 * appended) into SDRAM at 0x10000000 and starts BOTH cores at this loader. During the test
 * install (as .rockbox/rockbox.ipod) the old loader puts us at 0x10000000 instead. Either way:
 *
 *  1. both cores copy the loader (position-independent code) to its link address in IRAM,
 *     0x40004000, out of the way of whatever gets loaded to 0x10000000, and of the IRAM stub
 *     Knurl's crt0 copies to 0x40000000, and below 0x4000c000 (Apple's firmware keeps a flag
 *     in the top of IRAM);
 *  2. the COP sleeps; the CPU waits (bounded) for that, runs boot_main(), which returns the
 *     address to start (always 0x10000000: Knurl, or Apple's firmware);
 *  3. the CPU flushes its cache if the ROM left it on, wakes the COP, and both jump there,
 *     the COP after invalidating its cache. This is the hand-over Rockbox's loader does
 *     (firmware/target/arm/pp/crt0-pp-bl.S), so Knurl and Apple's firmware see the same start.
 */
    .syntax unified
    .arm
    .section .init, "ax"
    .global _start
_start:
    msr     cpsr_c, #0xd3            @ supervisor mode, IRQ and FIQ off

    adr     r0, _start               @ where we are
    ldr     r1, =_start              @ where we are linked
    ldr     r2, =_image_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     pc, =relocated

relocated:
    ldr     r0, =0x60000000          @ PROC_ID: 0x55 = CPU
    ldrb    r0, [r0]
    cmp     r0, #0x55
    bne     cop

    ldr     r0, =0x60007004          @ COP_CTL: wait (bounded) until the COP sleeps
    ldr     r3, =0x00100000
2:  ldr     r1, [r0]
    tst     r1, #0x80000000
    bne     3f
    subs    r3, r3, #1
    bne     2b
3:
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    mov     r2, #0
4:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     4b

    ldr     sp, =_stack_top
    bl      boot_main
    ldr     r1, =start_addr
    str     r0, [r1]

    mov     r0, #0                   @ flush
    bl      cache_op
    ldr     r0, =0x60007004          @ wake the COP
    mov     r1, #0
    str     r1, [r0]
    ldr     r1, =start_addr
    ldr     pc, [r1]

cop:
    ldr     r0, =0x60007004          @ sleep until the CPU has decided
    mov     r1, #0x80000000
    str     r1, [r0]
    nop
    nop
    mov     r0, #1                   @ invalidate
    bl      cache_op
    ldr     r0, =start_addr
    ldr     pc, [r0]

@ r0 = 0: flush, 1: flush and invalidate. Only if the cache is on. No stack used.
cache_op:
    ldr     r2, =0x6000c000          @ CACHE_CTL
    ldr     r1, [r2]
    tst     r1, #1
    bxeq    lr
    ldr     r3, =0xf000f044
    ldr     r1, [r3]
    cmp     r0, #0
    orrne   r1, r1, #6
    orreq   r1, r1, #2
    str     r1, [r3]
5:  ldr     r1, [r2]
    tst     r1, #0x8000
    bne     5b
    bx      lr

    .ltorg

    .section .data
    .align  2
start_addr:
    .word   0x10000000
