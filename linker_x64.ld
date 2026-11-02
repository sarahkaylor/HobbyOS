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

    /* Const tables of pointers need their relocations at load time.  Without
     * these output sections the linker drops them and a PIE kernel reads a
     * table of NULLs (measured: the DYN_TEST name list on aarch64). */
    .data.rel.ro : ALIGN(4096)
    {
        *(.data.rel.ro)
        *(.data.rel.ro.*)
        *(.data.rel.local)
        *(.data.rel.local.*)
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
