/* Pre-rendered images, built at build time (see tools/). Assembles for the iPod and the simulator. */
    .section .rodata
    .balign 4
    .global plane_pixels
plane_pixels:
    .incbin "build/plane.bin"
