/* knurlboot: runs from IRAM 0x40004000-0x4000c000 (start.S explains why there). */
ENTRY(_start)

MEMORY
{
    IRAM (rwx) : ORIGIN = 0x40004000, LENGTH = 0x8000
}

SECTIONS
{
    .text : {
        KEEP(*(.init))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > IRAM

    .data : {
        *(.data*)
        . = ALIGN(4);
        _image_end = .;          /* the file ends here: start.S copies up to this */
    } > IRAM

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > IRAM

    .stack (NOLOAD) : {
        . = ALIGN(8);
        . = . + 0x1000;
        _stack_top = .;
    } > IRAM

    ASSERT(_stack_top <= 0x4000c000, "loader doesn't fit below 0x4000c000")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
