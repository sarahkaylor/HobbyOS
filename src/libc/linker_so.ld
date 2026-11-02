/* src/libc/linker_so.ld — shared-object layout for HobbyOS's LIBC.SO.
 *
 * Lane C (so-toolchain), docs/shared-libs.md §4 lane C.  Linked by
 * tools/build_libs.sh <arch> as:
 *
 *   ld.lld -shared --soname=LIBC.SO -T src/libc/linker_so.ld \
 *           --version-script src/libc/libc.map -z relro -z now -z defs \
 *           -z noexecstack -o obj/<arch>/libc.so <pic objects...>
 *
 * Scope: this script owns the *intra-object* section order and the TLS
 * bracket symbols.  It deliberately says nothing about load addresses: a DSO
 * is linked at VA 0 and placed at runtime by the interpreter, so every
 * absolute address in the image is an R_*_RELATIVE relocation (there is no
 * fixed base, and there must never be a TEXTREL — see the evidence file).
 *
 * What we rely on lld for (verified per arch in obj/<arch>/libc.so.evidence.txt):
 *   - the four-segment RWX split (R / R+X / R / RW) produced by grouping
 *     consecutive output sections with equal p_flags; no PHDRS statement is
 *     needed because the order below never interleaves flags;
 *   - PT_DYNAMIC over the .dynamic section, PT_TLS over .tdata/.tbss,
 *     PT_GNU_RELRO over .data.rel.ro+.dynamic+.got (with -z relro -z now),
 *     PT_GNU_STACK RW (noexec: inputs carry .note.GNU-stack).
 * If a future lld changes that grouping, add an explicit PHDRS block here and
 * re-check p_flags in the evidence file — do not reorder the sections.
 *
 * Section order mirrors the executable images (src/user/linker.ld) closely
 * enough that the same TLS bootstrap code works, but a DSO's TLS image is a
 * *template*: the interpreter copies .tdata into its static block and the
 * process never executes out of .tdata.  The symbol names and their meaning
 * are nevertheless identical to the executable's, because libc code
 * (src/user/libc.c ho_tls_setup_initial, src/libc/src/pthread.c) reads them:
 *
 *   __tls_start      first byte of .tdata            (== p_vaddr of PT_TLS)
 *   __tls_data_end   first byte past .tdata          (bytes to memcpy)
 *   __tls_end        first byte past .tbss           (memsz of PT_TLS)
 *   __tls_align      ABSOLUTE: max alignment of .tdata/.tbss (a number)
 *   __tls_meta_align ADDRESS of a word holding __tls_align.  The executable
 *                    scripts carry this because an absolute symbol cannot be
 *                    PC-relative-reached from the 64 GiB image base; the same
 *                    reasoning applies to any DSO the interpreter maps high.
 *
 * All of the above are localized by src/libc/libc.map (`local: *`) — they
 * describe *this module's* TLS image, so they must never be interposable and
 * must never leak into another module's scope.
 *
 * There is no ENTRY(): a DSO has no entry point (e_entry stays 0) and no
 * .text._start; src/libc/crt0.c is excluded from the link by build_libs.sh
 * precisely because _start belongs to the executable.
 */

SECTIONS
{
    . = SIZEOF_HEADERS;

    /* --- read-only, non-executable: hash, dynsym/dynstr, relocs, rodata ---
     * .hash and .gnu.hash are both requested at link time
     * (--hash-style=both) so lane D can exercise either lookup path, and the
     * fixtures in tools/elf-dyn cover both. */
    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.version : { *(.gnu.version) }
    .gnu.version_d : { *(.gnu.version_d) }
    .gnu.version_r : { *(.gnu.version_r) }

    .rela.dyn : {
        *(.rela.init)
        *(.rela.text .rela.text.* .rela.ltext .rela.ltext.*)
        *(.rela.rodata .rela.rodata.* .rela.lrodata .rela.lrodata.*)
        *(.rela.data.rel.ro .rela.data.rel.ro.*)
        *(.rela.data .rela.data.* .rela.ldata .rela.ldata.*)
        *(.rela.tdata .rela.tdata.*)
        *(.rela.tbss .rela.tbss.*)
        *(.rela.got .rela.got.*)
        *(.rela.bss .rela.bss.*)
        *(.rela.dyn)
    }
    .rela.plt : { *(.rela.plt) }

    .rodata : {
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    }

    /* Unwind tables: clang emits none for these targets today (checked in the
     * evidence file), but claiming .eh_frame keeps a future
     * -funwind-tables build inside the R segment instead of letting it become
     * an orphan after .bss. */
    .eh_frame_hdr : { *(.eh_frame_hdr) }
    .eh_frame : { KEEP(*(.eh_frame)) }

    /* --- read-only, executable --- */
    .text : {
        *(.text .text.*)
        *(.ltext .ltext.*)
    }
    .init : { KEEP(*(.init)) }
    .fini : { KEEP(*(.fini)) }
    .plt : { *(.plt) *(.plt.*) *(.iplt) }

    /* --- writable, RELRO (made read-only by the interpreter) --- */
    .data.rel.ro : ALIGN(8) {
        *(.data.rel.ro .data.rel.ro.*)
        *(.ldata.rel.ro .ldata.rel.ro.*)
    }
    .dynamic : { *(.dynamic) }
    .got : { *(.got) *(.got.plt) *(.igot) }

    /* --- writable --- */
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

    .data : ALIGN(16) {
        *(.data .data.*)
        *(.ldata .ldata.*)
        *(.gnu.linkonce.d.*)
    }

    /* TLS template.  Same construction as src/user/linker.ld: a 16-byte
     * variant-I TCB head, then .tdata, then .tbss, then 16 bytes of slack for
     * the x86_64 variant-II self-pointer.  Alignment > 16 for a __thread
     * variable stays out of contract (the TPREL bias moves with the block
     * alignment) — no libc TU has one (int and pointer only). */
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
    . += 16;                        /* x86_64 self-pointer slack */

    .bss : {
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        *(COMMON)
    }

    /* TLS alignment as DATA, for the same reason as the executable script:
     * libc reads __tls_meta_align[0] rather than taking the value of the
     * absolute __tls_align. */
    .tls_meta : ALIGN(8) {
        __tls_meta_align = .;
        QUAD(__tls_align);
    }

    /* src/user/malloc.c tests `_end >= USER_VA_BASE` to choose the v2 heap
     * slot.  In libc.so this resolves (via .got, base-adjusted) to the end of
     * libc.so's .bss, which lives in USER_LIB (>= USER_VA_BASE), so the v2
     * branch is taken exactly as before.  Defining it keeps the DSO free of
     * undefined data symbols; see the note in the lane C report. */
    . = ALIGN(16);
    _end = .;
    PROVIDE(end = .);
}
