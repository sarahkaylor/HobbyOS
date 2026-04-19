#include "trap.h"
#include <stdint.h>

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void gic_enable_interrupt(uint32_t intid);
extern uint32_t gic_acknowledge_interrupt(void);
extern void virtio_blk_handle_irq(void);
extern uint32_t virtio_blk_irq;

#define SYS_WRITE (1)
#define SYS_EXIT (2)

void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

  uint32_t ec = (esr >> 26) & 0x3F; // Exception Class

  // EC == 0x15 indicates SVC instruction generated the exception in AArch64
  // state
  if (ec == 0x15) {
    uint64_t syscall_num = tf->regs[8]; // x8 standard

    if (syscall_num == SYS_WRITE) {
      // SYS_WRITE
      // tf->regs[0] contains the pointer to the string
      // We should ensure the pointer is within User space (e.g. 0x44000000
      // range)
      uint64_t ptr = tf->regs[0];
      if (ptr >= 0x44000000 && ptr < 0x48000000) {
        uart_puts((const char *)ptr);
      } else {
        uart_puts("Kernel: Blocked SYS_WRITE pointer out of bounds\n");
      }
      tf->regs[0] = 0; // Return success
    } else if (syscall_num == SYS_EXIT) {
      // SYS_EXIT
      uart_puts("\n[KERNEL] User application terminated gracefully.\n");
      uart_puts("System halt.\n");

      // Loop natively instead of returning to user code
      while (1) {
        __asm__ volatile("wfi");
      }
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -1; // Return error
    }
  } else if (ec == 0x24 || ec == 0x25) {
    // EC = 0x24: Instruction Abort from a lower Exception Level
    // EC = 0x25: Data Abort from a lower Exception Level
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    
    uint32_t iss = esr & 0x1FFFFFF; // Instruction Specific Syndrome
    uint32_t fault_code = (iss >> 0) & 0x3F; // Fault Status Code
    
    uart_puts("\n[KERNEL] Memory Protection Violation Detected!\n");
    uart_puts("Fault Address: 0x");
    void print_int(int val); // temp decl
    
    // Print fault address in hex format
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (far >> shift) & 0xF;
        if (nibble < 10)
            uart_putc('0' + nibble);
        else
            uart_putc('a' + (nibble - 10));
    }
    if (ec == 0x24) {
        uart_puts("\nFault Type: Instruction Abort\n");
    } else {
        uart_puts("\nFault Type: Data Abort\n");
    }
uart_puts("Fault Details: Code 0x");
for (int shift = 4; shift >= 0; shift -= 4) {
    uint64_t nibble = (fault_code >> shift) & 0xF;
    if (nibble < 10)
        uart_putc('0' + nibble);
    else
        uart_putc('a' + (nibble - 10));
}
uart_puts(" ");

// Decode fault type for both instruction and data aborts
if (fault_code == 0x7) { // Address size fault, level 0
    uart_puts("(Address Size Fault)\n");
} else if (fault_code == 0x9 || fault_code == 0xB) { // Translation/Permission faults, level 1
    uart_puts("(Permission/Translation Fault - Level 1)\n");
} else if (fault_code == 0x15 || fault_code == 0x17) { // Translation/Permission faults, level 2
    uart_puts("(Permission/Translation Fault - Level 2)\n");
} else {
    uart_puts("(Unknown Fault Type)\n");
    }
    
    // Terminate the user program by setting a return value of -1
    uart_puts("[KERNEL] Terminating user program due to memory protection violation.\n");
    tf->regs[0] = -1; // Return error code
    
    // Skip the faulting instruction by advancing ELR_EL1 to avoid infinite loop
    tf->elr += 4;
  } else {
    // Unhandled Synchronous exception from EL0
    uart_puts("FATAL: Unhandled EL0 Synchronous Exception! EC: ");
    void print_int(int val); // temp decl
    print_int(ec);
    uart_puts("\n");
    while (1) {
      __asm__ volatile("wfi");
    }
  }
}

void irq_lower_handler_c(struct trap_frame *tf) {
  (void)tf; // We don't dynamically alter trap state on an IRQ natively

  uint32_t intid = gic_acknowledge_interrupt();
  if (intid == virtio_blk_irq) {
    virtio_blk_handle_irq();
  }
}
