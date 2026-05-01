#include "trap.h"

#include <stdint.h>
#include "setjmp.h"
#include "process.h"
#include "timer.h"
#include "fs.h"

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);
extern void timer_reload(void);

/**
 * Prints the state of a trap frame for debugging purposes.
 */
void debug_print_tf(struct trap_frame *tf) {
    if (tf->regs[8] == 3) { // SYS_FORK is 3
        uart_puts("[DEBUG] Before eret, tf->regs[0] = ");
        print_int((int)tf->regs[0]);
        uart_puts(" ELR = ");
        uart_print_hex(tf->elr);
        uart_puts("\n");
    }
}

extern void gic_enable_interrupt(uint32_t intid);
extern uint32_t gic_acknowledge_interrupt(void);
extern void gic_end_interrupt(uint32_t intid);
extern void virtio_blk_handle_irq(void);
extern uint32_t virtio_blk_irq;
extern uint32_t get_cpuid(void);

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT          (2)
#define SYS_FORK          (3)
#define SYS_OPEN          (4)
#define SYS_CLOSE         (5)
#define SYS_READ          (6)
#define SYS_WRITE         (7)
#define SYS_SPAWN         (8)
#define SYS_MAP_FB        (9)
#define SYS_FLUSH_FB      (10)
#define SYS_GET_CPUID     (11)
#define SYS_PIPE          (12)

// Timer PPI interrupt ID on QEMU virt (non-secure physical timer)
#define TIMER_PPI_INTID 30

extern void uart_print_hex(uint64_t val);

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[0];
  if (ptr >= USER_VIRT_BASE && ptr < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    uart_puts("[CONSOLE] "); uart_puts((const char *)ptr);
  }
  tf->regs[0] = 0;
}

static void sys_exit(struct trap_frame *tf) {
  process_exit(tf);
}

static void sys_fork(struct trap_frame *tf) {
    int pid = process_fork(tf);
    tf->regs[0] = (uint64_t)pid;
    uart_puts("[KERNEL] sys_fork: tf->regs[0] is now "); print_int((int)tf->regs[0]); uart_puts("\n");
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
    int ret = file_read(fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 4; // Restart syscall
      schedule(tf);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_write(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  const void *buf = (const void *)tf->regs[1];
  int size = (int)tf->regs[2];
  if ((uint64_t)buf >= USER_VIRT_BASE && (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int ret = file_write(fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 4; // Restart syscall
      schedule(tf);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

extern int load_and_run_program_in_scheduler(const char* filename);

static void sys_spawn(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[0];
  if ((uint64_t)filename >= USER_VIRT_BASE && (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int child_pid = load_and_run_program_in_scheduler(filename);
    tf->regs[0] = child_pid;
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_pipe(struct trap_frame *tf) {
  int *fds = (int *)tf->regs[0];
  uint64_t fds_addr = (uint64_t)fds;
  if (fds_addr >= USER_VIRT_BASE && fds_addr + 8 <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int kernel_fds[2];
    int res = file_pipe(kernel_fds);
    if (res == 0) {
      fds[0] = kernel_fds[0];
      fds[1] = kernel_fds[1];
      tf->regs[0] = 0;
    } else {
      tf->regs[0] = -1;
    }
  } else {
    tf->regs[0] = -1;
  }
}

extern uint32_t* virtio_gpu_get_framebuffer(void);
extern void virtio_gpu_flush(void);
extern void mmu_map_user_framebuffer(uint64_t phys_addr);

static void sys_map_fb(struct trap_frame *tf) {
  uint64_t phys_addr = (uint64_t)virtio_gpu_get_framebuffer();
  mmu_map_user_framebuffer(phys_addr);
  tf->regs[0] = 0x50000000; // Return user virtual address
}

static void sys_flush_fb(struct trap_frame *tf) {
  virtio_gpu_flush();
  tf->regs[0] = 0;
}

static void sys_get_cpuid(struct trap_frame *tf) {
    tf->regs[0] = (uint64_t)get_cpuid();
}

/**
 * High-level handler for synchronous exceptions occurring in the kernel (EL1).
 * Typically handles fatal errors like alignment faults or kernel-level page faults.
 */
void sync_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
  uint32_t ec = (esr >> 26) & 0x3F;
  uint32_t iss = esr & 0xFFFFFF;

  if (ec == 0x15 && iss == 0xFF) {
    // Yield SVC from EL1
    schedule(tf);
    return;
  }

  uart_puts("[KERNEL] FATAL: Synchronous Exception in EL1! ESR: ");
  uart_print_hex(esr);
  uint64_t far;
  __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  uart_puts(", FAR: ");
  uart_print_hex(far);
  uart_puts("\n");
  while (1)
    ;
}

/**
 * High-level handler for synchronous exceptions occurring in user space (EL0).
 * This function dispatches system calls based on the SVC instruction's immediate value
 * and the syscall number in x8.
 */
void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

  uint64_t ec = (esr >> 26) & 0x3F;
  uint32_t iss = esr & 0xFFFFFF;

  // EC == 0x15 indicates SVC instruction generated the exception in AArch64 state
  if (ec == 0x15) {
    if (iss == 0xFF) {
        // Yield SVC
        schedule(tf);
        return;
    }

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
    } else if (syscall_num == SYS_MAP_FB) {
      sys_map_fb(tf);
    } else if (syscall_num == SYS_FLUSH_FB) {
      sys_flush_fb(tf);
    } else if (syscall_num == SYS_GET_CPUID) {
      sys_get_cpuid(tf);
    } else if (syscall_num == SYS_PIPE) {
      sys_pipe(tf);
    } else if (syscall_num == 0xFF) {
      schedule(tf);
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -1; // Return error
    }
  } else if (ec == 0x20 || ec == 0x24) {
    // EC = 0x20: Instruction Abort from a lower Exception Level
    // EC = 0x24: Data Abort from a lower Exception Level

    // Terminate the user program
    struct process *cur = current_process();
    if (cur && cur->state == PROC_STATE_RUNNING) {
      uart_puts("[KERNEL] User process "); print_int(cur->pid); uart_puts(" fault! EC: "); uart_print_hex(ec); uart_puts(" ELR: "); uart_print_hex(tf->elr); uart_puts("\n");
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

/**
 * High-level handler for hardware interrupts (IRQs) occurring in user space (EL0).
 * Handles timer interrupts for preemption and VirtIO interrupts for I/O.
 */
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
