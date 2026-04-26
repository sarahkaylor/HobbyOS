#include "trap.h"
#include <stdint.h>
#include "setjmp.h"
#include "process.h"
#include "timer.h"
#include "fat16.h"

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void gic_enable_interrupt(uint32_t intid);
extern uint32_t gic_acknowledge_interrupt(void);
extern void gic_end_interrupt(uint32_t intid);
extern void virtio_blk_handle_irq(void);
extern uint32_t virtio_blk_irq;

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT          (2)
#define SYS_FORK          (3)
#define SYS_OPEN          (4)
#define SYS_CLOSE         (5)
#define SYS_READ          (6)
#define SYS_WRITE         (7)
#define SYS_SPAWN         (8)

// Timer PPI interrupt ID on QEMU virt (non-secure physical timer)
#define TIMER_PPI_INTID 30

extern void uart_print_hex(uint64_t val);

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[0];
  if (ptr >= USER_VIRT_BASE && ptr < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    uart_puts((const char *)ptr);
  } else {
    uart_puts("Kernel: Blocked SYS_WRITE_CONSOLE pointer out of bounds\n");
  }
  tf->regs[0] = 0; // Return success
}

static void sys_exit(struct trap_frame *tf) {
  struct process *cur = current_process();
  if (cur && cur->state == PROC_STATE_RUNNING) {
    process_exit(tf);
    timer_reload();
  } else {
    longjmp(user_exit_context, 1);
  }
}

static void sys_fork(struct trap_frame *tf) {
  int child_pid = process_fork(tf);
  tf->regs[0] = (uint64_t)child_pid;
}

static void sys_open(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[0];
  if ((uint64_t)filename >= USER_VIRT_BASE && (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = file_open(filename);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_close(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  tf->regs[0] = file_close(fd);
}

static void sys_read(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  void *buf = (void *)tf->regs[1];
  int size = (int)tf->regs[2];
  if ((uint64_t)buf >= USER_VIRT_BASE && (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = file_read(fd, buf, size);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_write(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  const void *buf = (const void *)tf->regs[1];
  int size = (int)tf->regs[2];
  if ((uint64_t)buf >= USER_VIRT_BASE && (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = file_write(fd, buf, size);
  } else {
    tf->regs[0] = -1;
  }
}

extern int load_and_run_program_in_scheduler(const char* filename);

static void sys_spawn(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[0];
  if ((uint64_t)filename >= USER_VIRT_BASE && (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = load_and_run_program_in_scheduler(filename);
  } else {
    tf->regs[0] = -1;
  }
}

void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

  uint64_t ec = (esr >> 26) & 0x3F;

  // EC == 0x15 indicates SVC instruction generated the exception in AArch64
  // state
  if (ec == 0x15) {
    uint64_t syscall_num = tf->regs[8]; // x8 standard

    if (syscall_num == SYS_WRITE_CONSOLE) {
      sys_write_console(tf);
    } else if (syscall_num == SYS_EXIT) {
      sys_exit(tf);
    } else if (syscall_num == SYS_FORK) {
      sys_fork(tf);
    } else if (syscall_num == SYS_OPEN) {
      sys_open(tf);
    } else if (syscall_num == SYS_CLOSE) {
      sys_close(tf);
    } else if (syscall_num == SYS_READ) {
      sys_read(tf);
    } else if (syscall_num == SYS_WRITE) {
      sys_write(tf);
    } else if (syscall_num == SYS_SPAWN) {
      sys_spawn(tf);
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
