/* P2.2 (S2): linker script for v2 (AS_V2) user programs.
 *
 * Byte-for-byte the layout of src/user/linker.ld.  Both scripts link at
 * the same v2 base (USER_IMG_BASE, 64 GiB, docs/browser/p2-vm-design.md
 * section 1.2) since the S5 flip; this file is kept for the programs
 * whose build rules name it (MMTEST).
 *
 * (Keep this file in sync with linker.ld: the TLS layout probes in that
 * file's comments apply verbatim to both.)
 */

ENTRY(_start)

SECTIONS
{
    . = 0x1000000000;

    .text : {
        *(.text._start)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }

    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

    .data : {
        *(.data*)
    }

    . = ALIGN(16);
    __tls_tcb_head = .;
    . += 16;
    .tdata : ALIGN(16) {
        __tls_start = .;
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        __tls_data_end = .;
    }
    .tbss : {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        . = ALIGN(16);
        __tls_end = .;
    }
    __tls_size = __tls_end - __tls_start;
    __tls_align = ALIGNOF(.tdata) > ALIGNOF(.tbss) ? ALIGNOF(.tdata) : ALIGNOF(.tbss);
    . += 16;

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /* P2.5 (S5): TLS alignment as DATA (see linker.ld for the full note).
     * objcopy -O binary pads the flat .bin through .bss to reach this
     * word, so the v2 loader maps the full image span including .bss. */
    .tls_meta : ALIGN(8) {
        __tls_meta_align = .;
        QUAD(__tls_align);
    }

    _end = . ;
}
