#include "trap.h"
#include <stdint.h>

// External functions we need
extern void uart_puts(const char *s);
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
