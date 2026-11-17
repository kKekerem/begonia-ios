ENTRY(_start)

SECTIONS
{
    /* MediaTek LK decompresses and enters Image.gz at BOARD_KERNEL_BASE + BOARD_KERNEL_OFFSET */
    . = 0x40080000;

    .text.boot : {
        *(.text.boot)
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
        . += 0x10000; /* 64 KiB loader stack inside BSS to avoid clobbering below 0x40080000 */
        . = ALIGN(16);
        _stack_top = .;
    }

    . = ALIGN(16);
    __loader_end = .;
}
