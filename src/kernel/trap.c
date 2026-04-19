#include "trap.h"
#include <stdint.h>
#include "setjmp.h"

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void gic_enable_interrupt(uint32_t intid);
extern uint32_t gic_acknowledge_interrupt(void);
extern void virtio_blk_handle_irq(void);
extern uint32_t virtio_blk_irq;

#define SYS_WRITE (1)
#define SYS_EXIT (2)

extern void uart_print_hex(uint64_t val);

void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

  uint64_t ec = (esr >> 26) & 0x3F;

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
      tf->elr += 4;   // Skip the svc instruction
      tf->regs[0] = 0; // Return success
    } else if (syscall_num == SYS_EXIT) {
      // SYS_EXIT
      longjmp(user_exit_context, 1);
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -1; // Return error
    }
  } else if (ec == 0x20 || ec == 0x24) {
    // EC = 0x20: Instruction Abort from a lower Exception Level
    // EC = 0x24: Data Abort from a lower Exception Level
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    
    uart_puts("\n[KERNEL] Memory Protection Violation Detected!\n");
    uart_puts("Fault Address: ");
    uart_print_hex(far);
    
    if (ec == 0x20) {
        uart_puts("\nFault Type: Instruction Abort\n");
    } else {
        uart_puts("\nFault Type: Data Abort\n");
    }
    
    uart_puts("ESR: ");
    uart_print_hex(esr);
    uart_puts("\n");

    // Terminate the user program by longjumping back to the loader
    uart_puts("[KERNEL] Terminating user program due to memory protection violation.\n");
    longjmp(user_exit_context, 1);
  } else {
    // Unhandled Synchronous exception from EL0
    uart_puts("\n[KERNEL] FATAL: Unhandled EL0 Synchronous Exception!\n");
    uart_puts("EC: ");
    uart_print_hex(ec);
    uart_puts("\nELR: ");
    uart_print_hex(tf->elr);
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
