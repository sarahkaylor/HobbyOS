ENTRY(_start)

SECTIONS
{
    . = 0x44000000;
    
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
    
    .bss : {
        *(.bss*)
        *(COMMON)
    }

    _end = . ;
}
