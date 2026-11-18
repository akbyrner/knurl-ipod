/*
 * The polyphase synthesis inner loop for ARMv4 (ARM7TDMI), called from synth() in
 * mp3dec.c: 8 taps, each folding two window values into two 64-bit sums. GCC spilled most
 * of this to the stack (the decoder spent over half its time here); in assembly both sums,
 * the window pair and the two samples stay in registers. Same products, same order as the
 * C version, so the output is bit-identical (tools/mp3check.sh runs both under qemu).
 *
 *   void mp3_synth_lane(const int32_t *z, const int32_t *w, int32_t out[2])
 *     a = sum_k (-1)^k (z[-64k] w[2k] - z[-64(15-k)] w[2k+1])
 *     b = sum_k        (z[-64k] w[2k+1] + z[-64(15-k)] w[2k])
 *     out[0] = (a + 2^23) >> 24, out[1] = (b + 2^23) >> 24   (MP3_FRAC = 24; caller clips)
 *
 * Multiplies put the window value in Rs: it is the short operand (|w| < 2^17), so the
 * ARM7's early-terminating multiplier finishes sooner.
 */
    .syntax unified
    .arm
    .text
    .global mp3_synth_lane
    .type   mp3_synth_lane, %function

/* r0 z, r1 w (advances), r2:r3 a, r4:r5 b, r6 w0, r7 w1, r8 vz, r9 vy, r10 scratch, r12 out */
.macro TAP k, even
    ldr     r8, [r0, #-(\k * 256)]
    ldr     r9, [r0, #-((15 - \k) * 256)]
    ldmia   r1!, {r6, r7}
    smlal   r4, r5, r8, r7          @ b += vz * w1
    smlal   r4, r5, r9, r6          @ b += vy * w0
.if \even
    smlal   r2, r3, r8, r6          @ a += vz * w0
    rsb     r10, r9, #0
    smlal   r2, r3, r10, r7         @ a -= vy * w1
.else
    smlal   r2, r3, r9, r7          @ a += vy * w1
    rsb     r10, r8, #0
    smlal   r2, r3, r10, r6         @ a -= vz * w0
.endif
.endm

mp3_synth_lane:
    push    {r4-r10}
    mov     r12, r2
    mov     r2, #0
    mov     r3, #0
    mov     r4, #0
    mov     r5, #0
    TAP     0, 1
    TAP     1, 0
    TAP     2, 1
    TAP     3, 0
    TAP     4, 1
    TAP     5, 0
    TAP     6, 1
    TAP     7, 0
    adds    r2, r2, #0x800000       @ round, then >> 24 of the 64-bit sums
    adc     r3, r3, #0
    lsr     r2, r2, #24
    orr     r2, r2, r3, lsl #8
    adds    r4, r4, #0x800000
    adc     r5, r5, #0
    lsr     r4, r4, #24
    orr     r4, r4, r5, lsl #8
    stmia   r12, {r2, r4}
    pop     {r4-r10}
    bx      lr
    .size   mp3_synth_lane, . - mp3_synth_lane
#ifdef __linux__
    .section .note.GNU-stack, "", %progbits    @ (qemu test build only)
#endif
