.section .text.boot
.global _start

_start:
    // Read the CPU ID (MPIDR_EL1)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    // If not CPU 0, branch to halt (we only want one core to run this)
    cbnz    x0, halt

    // Set stack pointer
    ldr     x0, =__stack_top
    mov     sp, x0

    // Clear the BSS section
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
clear_bss:
    cmp     x1, x2
    b.ge    run_main
    str     xzr, [x1], #8
    b       clear_bss

run_main:
    // Call the C main function
    bl      main

halt:
    // Infinite loop, wait for interrupt
    wfi
    b       halt
