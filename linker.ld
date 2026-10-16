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

    /* Allocate 512KB for the stack (64KB per core for 8 cores) */
    . = ALIGN(16);
    . = . + 0x80000;
    __stack_top = .;
}

/* Limine placement smoke alarm (docs/blk-cache.md section 11): Limine
   loads the ARM kernel near the TOP of guest RAM, so at the standard
   16 GiB config the link-space ceiling is ~0x45A00000 (slide 0x3FA600000
   → RAM top 0x440000000 runtime).  A .bss that crosses the line makes
   the boot zeroing loop (and the fault handler's own .bss) touch
   unbacked RAM: data-abort cascade with zero serial output after
   "linux: Loading kernel".  The pre-cache kernel ended at ~0x4503EC58
   (~10 MB of headroom); 12+ MiB of new .bss blows through it.  Keep at
   least 4 MB clear under the line: move big tables to the frame pool
   instead of .bss. */
ASSERT(__bss_end <= 0x45600000,
       "kernel .bss exceeds the limine placement window (docs/blk-cache.md section 11)");
