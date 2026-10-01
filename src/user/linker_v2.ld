/* P2.2 (S2): linker script for v2 (AS_V2) user programs.
 *
 * Byte-for-byte the layout of src/user/linker.ld except for the image
 * base: v2 images link at USER_IMG_BASE (64 GiB, docs/browser/
 * p2-vm-design.md section 1.2), not the v1 window at 0x44000000.  The
 * kernel's loader v2 reads the flat .bin into fresh frames and maps them
 * at USER_IMG_BASE, so .text._start still lands first at the image base.
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

    _end = . ;
}
