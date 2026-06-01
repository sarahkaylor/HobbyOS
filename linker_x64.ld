ENTRY(_start)

SECTIONS
{
    /* Multiboot expects the kernel to be loaded at 1MB or higher */
    . = 0x70000000;

    .text : ALIGN(4096)
    {
        /* Multiboot header MUST be in the first 8KB */
        KEEP(*(.text.boot))
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Stack top */
    . = ALIGN(16);
    . = . + 0x80000; /* Allocate 512KB stack (64KB per core for 8 cores) */
    __stack_top = .;
}
