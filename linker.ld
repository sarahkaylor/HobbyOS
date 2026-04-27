ENTRY(_start)

SECTIONS
{
    /* 
     * Typical load address for the QEMU virt machine kernel.
     * We don't use 0x40000000 exactly because QEMU places the device tree there.
     */
    . = 0x40080000;

    /* Text section: code */
    .text :
    {
        /* Ensure the boot assembly is placed first */
        *(.text.boot)
        *(.text)
        *(.text.*)
    }

    /* Read-only data section */
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialized data section */
    .data :
    {
        *(.data)
        *(.data.*)
    }

    /* Uninitialized data section */
    .bss (NOLOAD) :
    {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Allocate 256KB for the stack (64KB per core for 4 cores) */
    . = ALIGN(16);
    . = . + 0x40000;
    __stack_top = .;
}
