.section .text.boot
.global _start

_start:
    // Read the CPU ID (MPIDR_EL1)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    
    // Set stack pointer: __stack_top - (cpu_id * 64KB)
    mov     x1, #0x10000             // 64KB per CPU
    mul     x1, x0, x1
    ldr     x2, =__stack_top
    sub     sp, x2, x1

    // Set Vector Base Address Register for EL1
    ldr     x1, =vectors
    msr     vbar_el1, x1

    // If not CPU 0, branch to halt (secondary cores start here only if not using PSCI)
    cbnz    x0, halt

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
    // Shut down QEMU using semihosting SYS_EXIT call.
    // On AArch64, the semihosting trap instruction is HLT #0xF000.
    // x0 = operation number: 0x18 = SYS_EXIT_EXTENDED
    // x1 = parameter block pointer (exit reason)
    // We use a two-word block on the stack:
    //   [sp]     = exit reason code 0x20026 (ADP_ExitReason_ApplicationExit + ADP_Stop_NoError)
    //   [sp + 8] = sub-code 0
    sub     sp, sp, #16
    mov     x1, #0x0026              // ADP_ExitReason_ApplicationExit
    orr     x1, x1, #0x20000         // Combined with ADP_Stop_NoError
    str     x1, [sp, #0]
    str     xzr, [sp, #8]
    ldr     x0, =0x18                // SYS_EXIT_EXTENDED operation
    mov     x1, sp                   // Parameter block pointer
    hlt     #0xF000                  // AArch64 semihosting trap
    
    // If semihosting is not enabled, fall back to PSCI SYSTEM_OFF
    ldr     x0, =0xc4000008          // PSCI SYSTEM_OFF function ID (SMC64)
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    smc     #0                       // Secure monitor call
    
    // Last resort: infinite loop
    wfi
    b       halt

.global secondary_entry
secondary_entry:
    // Read the CPU ID (MPIDR_EL1)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF

    // Set stack pointer: __stack_top - (cpu_id * 64KB)
    mov     x1, #0x10000             // 64KB per CPU
    mul     x1, x0, x1
    ldr     x2, =__stack_top
    sub     sp, x2, x1

    // Set Vector Base Address Register for EL1
    ldr     x1, =vectors
    msr     vbar_el1, x1

    // Call the C secondary_main function
    bl      secondary_main

secondary_halt:
    wfi
    b       secondary_halt

// The Vector Base Address Register (VBAR_EL1) requires a 2KB aligned address.
// This `.align 11` ensures the vector_table symbol sits strictly on a 2048-byte boundary.
.align 11
.global vectors
vectors:
    // Current EL with SP0
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .

    // Current EL with SPx (EL1 taking an exception from EL1)
    // Offset 0x200: Synchronous Exceptions (e.g. Unaligned Data Abort, Instruction Abort)
    .align 7
    b kernel_sync_wrapper
    // Offset 0x280: IRQ (Hardware Interrupts)
    .align 7
    b irq_wrapper
    // Offset 0x300: FIQ
    .align 7
    b .
    // Offset 0x380: SError
    .align 7
    b .
 
    // Lower EL AArch64 (EL0 to EL1 traps)
    // Offset 0x400: Synchronous
    .align 7
    b sync_lower_wrapper
    // Offset 0x480: IRQ
    .align 7
    b irq_lower_wrapper
    .align 7
    b .
    .align 7
    b .

    // Lower EL AArch32
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .

irq_wrapper:
    // EL1 -> EL1 Interrupt Tracking
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    
    // Call architecture independent C handler to handle GIC & hardware ack
    bl irq_handler_c

    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #256
    eret

sync_lower_wrapper:
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    // Capture ELR and SPSR into Trap Frame
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #248]

    // Set Argument 0 (w0 / x0) to the top of Trap Frame structure securely mapped 
    mov x0, sp
    bl sync_lower_handler_c

    // Read mutated context vectors mapping
    ldp x0, x1, [sp, #248]
    msr elr_el1, x0
    msr spsr_el1, x1

    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #272
    eret

irq_lower_wrapper:
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    // Capture ELR and SPSR into Trap Frame
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #248]

    mov x0, sp
    bl irq_lower_handler_c

    ldp x0, x1, [sp, #248]
    msr elr_el1, x0
    msr spsr_el1, x1

    ldr x30, [sp, #240]
    ldp x28, x29, [sp, #224]
    ldp x26, x27, [sp, #208]
    ldp x24, x25, [sp, #192]
    ldp x22, x23, [sp, #176]
    ldp x20, x21, [sp, #160]
    ldp x18, x19, [sp, #144]
    ldp x16, x17, [sp, #128]
    ldp x14, x15, [sp, #112]
    ldp x12, x13, [sp, #96]
    ldp x10, x11, [sp, #80]
    ldp x8, x9, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #272
    eret

    // Lower EL A64 SError
    .align 7
    b .

    // Lower EL AArch32 (offset 0x600)
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .
    .align 7
    b .

kernel_sync_wrapper:
    // Simple hang with message to diagnose kernel-level crashes
    ldr x0, =kernel_fault_msg
    bl uart_puts
    mrs x0, esr_el1
    bl uart_print_hex
    ldr x0, =kernel_far_msg
    bl uart_puts
    mrs x0, far_el1
    bl uart_print_hex
    b .

kernel_fault_msg: 
    .string "\n[KERNEL] FATAL: Synchronous Exception in EL1! ESR: "
    .align 3
kernel_far_msg:
    .string ", FAR: "
    .align 3


