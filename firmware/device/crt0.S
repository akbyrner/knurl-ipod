/*
 * Entry point. The Rockbox bootloader loads this image into SDRAM at 0x10000000 and
 * jumps BOTH cores here (CPU and COP).
 *
 *  - Each core has its own memory map and cache, so each remaps SDRAM to logical address 0
 *    itself (the PP502x cache only covers 0x00000000-0x03ffffff; code at 0x10000000 is never
 *    cached). The image is linked for address 0, so until the remap only PC-relative code
 *    runs. The remap is done from a tiny stub copied into IRAM, as Rockbox's crt0-pp.S does.
 *  - Order: the COP sleeps at once. The CPU remaps, wakes the COP, which remaps too, says so
 *    in IRAM and sleeps again in its idle loop. Only then does the CPU clear IRAM (.ibss),
 *    because the stub lives there, and start main(). Later cop_start() hands the COP a
 *    function through cop_entry and wakes it (device/cop.c).
 *  - Only one interrupt is ever taken: DMA completion for audio, as FIQ on the CPU
 *    (device/audio.c). Everything else is polled.
 */
    .syntax unified
    .arm
    .section .init, "ax"
    .global _start
_start:                              @ exception vectors (address 0 after the remap)
    b       reset
    b       vec_undef
    b       vec_swi
    b       vec_pabt
    b       vec_dabt
    b       vec_rsvd
    b       vec_irq
    b       audio_fiq                @ DMA (audio) completion, device/audio.c

    .equ    COP_READY, 0x4001fe00    @ IRAM word outside .ibss: "COP has remapped"

reset:
    msr     cpsr_c, #0xd3            @ supervisor mode, IRQ and FIQ off

    ldr     r0, =0x60000000          @ PROC_ID
    ldrb    r0, [r0]
    cmp     r0, #0x55                @ CPU?
    bne     cop_reset

    ldr     r1, =0x60004028          @ CPU_INT_DIS: mask every interrupt source
    mvn     r2, #0
    str     r2, [r1]

    ldr     r2, =0x60007004          @ wait (bounded) until the COP is asleep
    ldr     r3, =0x00100000
1:  ldr     r1, [r2]
    tst     r1, #0x80000000
    bne     2f
    subs    r3, r3, #1
    bne     1b
2:
    ldr     r2, =0x6000c000          @ CACHE_CTL: cache off while remapping
    mov     r1, #0
    str     r1, [r2]

    mov     r2, #0x40000000          @ copy the remap stub to IRAM
    adr     r3, remap_start
    adr     r4, remap_end
3:  ldr     r5, [r3], #4
    str     r5, [r2], #4
    cmp     r3, r4
    blo     3b

    ldr     r5, =remapped
    b       do_remap

cop_reset:
    ldr     r1, =0x60004038          @ COP_INT_DIS
    mvn     r2, #0
    str     r2, [r1]
    ldr     r1, =0x60007004          @ COP_CTL: sleep until the CPU has remapped and wakes us
    mov     r2, #0x80000000
    str     r2, [r1]
    nop
    nop
    nop
    ldr     r2, =0x6000c000          @ our cache off while remapping
    mov     r1, #0
    str     r1, [r2]
    ldr     r5, =cop_remapped

do_remap:                            @ r5 = where to continue, at the linked address
    ldr     r1, =0xf000f000          @ MMAP0_LOGICAL
    ldr     r2, =0x00003e00          @ 32 MB window at logical 0
    ldr     r3, =0xf000f004          @ MMAP0_PHYSICAL
    and     r6, pc, #0xff000000      @ where we really are: 0x10000000
    ldr     r4, =0x00000f84
    orr     r4, r4, r6
    mov     pc, #0x40000000          @ run the stub from IRAM

remap_start:
    str     r2, [r1]
    str     r4, [r3]
    mov     pc, r5
remap_end:

remapped:                            @ CPU, now at linked addresses
    ldr     r1, =COP_READY           @ let the COP remap itself (the stub is still in IRAM)
    mov     r2, #0
    str     r2, [r1]
    ldr     r3, =0x60007004          @ COP_CTL = wake
    str     r2, [r3]
    ldr     r4, =0x00400000          @ wait (bounded) for it to report back ...
4:  ldr     r2, [r1]
    cmp     r2, #1
    beq     5f
    subs    r4, r4, #1
    bne     4b
5:  ldr     r4, =0x00100000          @ ... and to be asleep again
6:  ldr     r2, [r3]
    tst     r2, #0x80000000
    bne     7f
    subs    r4, r4, #1
    bne     6b
7:
    ldr     r1, =_ibss_start         @ zero IRAM data (.ibss), now the stub is no longer needed
    ldr     r2, =_ibss_end
    mov     r3, #0
8:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     8b

    msr     cpsr_c, #0xd1            @ FIQ mode: its own small stack
    ldr     sp, =_fiq_stack_top
    msr     cpsr_c, #0xd3
    ldr     sp, =_stack_top
    ldr     r1, =__bss_start         @ zero .bss
    ldr     r2, =__bss_end
    mov     r3, #0
9:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     9b

    bl      main
10: b       10b                      @ main never returns

cop_remapped:                        @ COP, now at linked addresses
    ldr     r1, =COP_READY
    mov     r2, #1
    str     r2, [r1]
cop_idle:
    ldr     r1, =0x60007004          @ sleep until woken
    mov     r2, #0x80000000
    str     r2, [r1]
    nop
    nop
    nop
    ldr     r0, =cop_entry           @ (IRAM, written by cop_start on the CPU)
    ldr     r0, [r0]
    cmp     r0, #0
    beq     cop_idle
    ldr     sp, =_cop_stack_top
    mov     lr, pc
    bx      r0
    b       cop_idle

/* Any exception: show what happened on screen (crash() in device.c) instead of silently
 * re-running the start-up code. r0 = kind, r1 = address of the faulting instruction,
 * r2 = lr of the interrupted code. */
vec_undef: mov  r0, #1
           sub  r1, lr, #4
           b    crash_common
vec_swi:   mov  r0, #2
           sub  r1, lr, #4
           b    crash_common
vec_pabt:  mov  r0, #3
           sub  r1, lr, #4
           b    crash_common
vec_dabt:  mov  r0, #4
           sub  r1, lr, #8
           b    crash_common
vec_rsvd:  mov  r0, #5
           mov  r1, lr
           b    crash_common
vec_irq:   mov  r0, #6
           sub  r1, lr, #4
crash_common:
    msr     cpsr_c, #0xd3            @ back to supervisor mode, interrupts off
    mov     r2, lr
    ldr     r3, =0x60000000          @ the COP has its own crash stack
    ldrb    r3, [r3]
    cmp     r3, #0x55
    ldreq   sp, =_crash_stack_top
    ldrne   sp, =_cop_crash_stack_top
    bl      crash
11: b       11b

    .ltorg
