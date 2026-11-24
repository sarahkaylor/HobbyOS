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
        /* First byte of the image.  `arch_reloc_self()` (arch/arm/reloc.c)
         * derives the Limine slide as (runtime &__image_start) minus the link
         * address 0x40080000; keep the ASSERT at the bottom of this file and
         * KIMAGE_LINK_BASE in that file in step with the line above. */
        __image_start = .;
        /* Ensure the boot assembly is placed first */
        *(.text.boot)
        *(.text)
        *(.text.*)
    }

    /* Load-time relocation table (AArch64 `-pie` link): one
     * R_AARCH64_RELATIVE record per pointer word in static data, applied by
     * arch/arm/reloc.c from boot.s before the first C consumer reads the
     * table.  Placed explicitly so __rela_start/__rela_end bracket exactly
     * this table and nothing else.  The x86_64 image is not slid and carries
     * no relocations, so this section is empty there. */
    .rela.dyn : ALIGN(8)
    {
        __rela_start = .;
        KEEP(*(.rela.dyn))
        __rela_end = .;
    }

    /* Read-only data section */
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* Initialized data section */
    /* Const tables of pointers need their relocations at load time.  Without
     * these output sections the linker drops them and a PIE kernel reads a
     * table of NULLs (measured: the DYN_TEST name list on aarch64). */
    .data.rel.ro :
    {
        *(.data.rel.ro)
        *(.data.rel.ro.*)
        *(.data.rel.local)
        *(.data.rel.local.*)
    }

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
        /* Most-aligned input sections first: the 2 MiB-aligned objects
           (framebuffer, the blk area) otherwise sit wherever the link order
           puts them and waste up to 2 MiB of padding each whenever anything
           before them grows (cp-x3's 22 KB shm table cost exactly 2 MiB). */
        *(SORT_BY_ALIGNMENT(.bss) SORT_BY_ALIGNMENT(.bss.*) SORT_BY_ALIGNMENT(COMMON))
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

/* The AArch64 image is linked -pie and slid by Limine, so arch/arm/reloc.c has
   to know the LINK base to compute the slide.  It carries 0x40080000 as
   KIMAGE_LINK_BASE; keep the two in step, or every relocated pointer lands one
   constant away from where it belongs. */
ASSERT(__image_start == 0x40080000,
       "kernel link base moved: update KIMAGE_LINK_BASE in src/kernel/arch/arm/reloc.c");
