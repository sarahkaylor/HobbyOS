/* P2.5 (S5 flip, design section 8): user programs link at the v2 image
 * base USER_IMG_BASE (64 GiB, docs/browser/p2-vm-design.md section 1.2).
 * The kernel's loader v2 reads the flat .bin into fresh frames and maps
 * them at USER_IMG_BASE, so .text._start still lands first at the image
 * base.  The legacy v1 window at 0x44000000 is gone for user programs
 * (kernel tasks keep their 32 MiB blocks; the v1 loader stays as the
 * rollback lever).  linker_v2.ld is the historical twin of this file --
 * both now share the same base.
 *
 * (The TLS layout probes in the comments below apply verbatim to both.)
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

    /* F2.4 (browser.md §6): C++ static initializers.  The compiler puts the
     * address of every TU's static-ctor thunk (and constructor-attribute
     * handlers) in .init_array; crt0's _start walks the array between the
     * two hidden brackets below before calling main().  Being part of the
     * contiguous image, the array rides inside the flat .bin the kernel
     * loader copies wholesale — no loader change is needed.  .fini_array
     * is collected for shape symmetry only: static destructors run through
     * __cxa_atexit at exit(), not through .fini_array (see cxxrt.h). */
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

    /* P1 (p1-threads-design.md section 4): TLS image.  .tdata/.tbss are
     * placed contiguously AFTER .data (so alignment slack and the 16-byte
     * variant-I TCB head live in writable memory) and BEFORE .bss (so the
     * x86_64 self-pointer at FS:[0] = align_up(__tls_end, __tls_align) has
     * writable slack below .bss too).
     *
     * Probe-verified layout (llvm 21 + ld.lld):
     *   aarch64 (variant I):  TPIDR_EL0 = &__tls_start - 16;
     *                         TPREL(v) = offset(v) + 0x10 (lld bakes the head).
     *   x86_64  (variant II): FS base = align_up(&__tls_end, __tls_align);
     *                         TPREL(v) = offset(v) - align_up(size, align).
     * Constraint: __thread variables with alignment > 16 are out of contract
     * for P1 -- the lld TPREL bias moves with the image alignment. */
    . = ALIGN(16);
    __tls_tcb_head = .;              /* variant-I head: [TP, TP+16) */
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
    . += 16;                         /* x86_64 self-pointer slack */

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /* P2.5 (S5): TLS alignment as DATA.  __tls_align is an ABSOLUTE
     * symbol (its "address" is the number itself), so a PC-relative
     * reference to it cannot reach from a 64 GiB image base -- llvm/ld.lld
     * reject it with R_AARCH64_ADR_PREL_PG_HI21 out of range, which is why
     * the crt0/libc.a TLS bootstrap broke at the v2 base.  This word
     * carries the same number at an in-image address; crt0 and pthread.c
     * load it instead.  Placed LAST, after .bss, on purpose: objcopy
     * -O binary pads the flat .bin through .bss to reach it, so the
     * kernel's v2 loader maps (and zero-fills) the full image span,
     * .bss included. */
    .tls_meta : ALIGN(8) {
        __tls_meta_align = .;
        QUAD(__tls_align);
    }

    _end = . ;
}
