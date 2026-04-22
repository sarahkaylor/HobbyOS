#include "trap.h"
#include <stdint.h>
#include "setjmp.h"
#include "process.h"
#include "timer.h"

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void gic_enable_interrupt(uint32_t intid);
extern uint32_t gic_acknowledge_interrupt(void);
extern void gic_end_interrupt(uint32_t intid);
extern void virtio_blk_handle_irq(void);
extern uint32_t virtio_blk_irq;

#define SYS_WRITE (1)
#define SYS_EXIT  (2)
#define SYS_FORK  (3)

// Timer PPI interrupt ID on QEMU virt (non-secure physical timer)
#define TIMER_PPI_INTID 30

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
      tf->regs[0] = 0; // Return success
    } else if (syscall_num == SYS_EXIT) {
      // SYS_EXIT — check if we're running under the scheduler
      struct process *cur = current_process();
      if (cur && cur->state == PROC_STATE_RUNNING) {
        // Running under the scheduler: mark exited and schedule next
        process_exit(tf);
        // tf has been updated by schedule() — the eret path will switch to
        // the next process. We need to reload the timer too.
        timer_reload();
      } else {
        // Legacy sequential mode: longjmp back to load_and_run_program
        longjmp(user_exit_context, 1);
      }
    } else if (syscall_num == SYS_FORK) {
      // SYS_FORK — duplicate the current process
      int child_pid = process_fork(tf);
      tf->regs[0] = (uint64_t)child_pid; // Return child PID to parent
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

    // Terminate the user program
    struct process *cur = current_process();
    if (cur && cur->state == PROC_STATE_RUNNING) {
      uart_puts("[KERNEL] Terminating process PID=");
      extern void print_int(int val);
      print_int(cur->pid);
      uart_puts(" due to memory protection violation.\n");
      process_exit(tf);
      timer_reload();
    } else {
      uart_puts("[KERNEL] Terminating user program due to memory protection violation.\n");
      longjmp(user_exit_context, 1);
    }
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
  uint32_t intid = gic_acknowledge_interrupt();

  if (intid == TIMER_PPI_INTID) {
    // Timer tick — perform a context switch if the scheduler is active
    struct process *cur = current_process();
    if (cur) {
      timer_reload();
      gic_end_interrupt(intid);
      schedule(tf);
      return;
    }
    // Not under scheduler — just reload and continue
    timer_reload();
  } else if (intid == virtio_blk_irq) {
    virtio_blk_handle_irq();
  }

  gic_end_interrupt(intid);
}
