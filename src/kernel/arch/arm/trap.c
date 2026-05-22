#include "trap.h"

#include "fs.h"
#include "process.h"
#include "setjmp.h"
#include "timer.h"
#include "lock.h"
#include "arch/cpu.h"
#include "arch/timer.h"
#include <stdint.h>

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

/**
 * Prints the state of a trap frame for debugging purposes.
 * Triggers specifically when a certain syscall (like SYS_FORK) is invoked to
 * inspect register states before returning to user space.
 *
 * @param tf Pointer to the trap frame to inspect and print.
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

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT (2)
#define SYS_FORK (3)
#define SYS_OPEN (4)
#define SYS_CLOSE (5)
#define SYS_READ (6)
#define SYS_WRITE (7)
#define SYS_SPAWN (8)
#define SYS_MAP_FB (9)
#define SYS_FLUSH_FB (10)
#define SYS_GET_CPUID (11)
#define SYS_PIPE (12)
#define SYS_GET_EVENTS (13)
#define SYS_AVAILABLE (14)
#define SYS_READ_DIR (15)
#define SYS_KILL (16)
#define SYS_YIELD (17)
#define SYS_CONNECT (18)
#define SYS_SLEEP (19)

// Timer PPI interrupt ID on QEMU virt (non-secure physical timer)
#define TIMER_PPI_INTID 30

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[0];
  if (ptr >= USER_VIRT_BASE && ptr < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    uart_puts("[CONSOLE] ");
    uart_puts((const char *)ptr);
  }
  tf->regs[0] = 0;
}

static void sys_exit(struct trap_frame *tf) { process_exit(tf); }

static void sys_fork(struct trap_frame *tf) {
  int pid = process_fork(tf);
  tf->regs[0] = (uint64_t)pid;
  uart_puts("[KERNEL] sys_fork: tf->regs[0] is now ");
  print_int((int)tf->regs[0]);
  uart_puts("\n");
}

static void sys_open(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[0];
  struct process *caller = current_process();
  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = file_open(caller, filename);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_close(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  struct process *caller = current_process();
  tf->regs[0] = file_close(caller, fd);
}

static void sys_read(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  void *buf = (void *)tf->regs[1];
  int size = (int)tf->regs[2];
  struct process *caller = current_process();
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int ret = file_read(caller, fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 4; // Restart syscall
      schedule(tf, 0);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_connect(struct trap_frame *tf) {
  uint32_t ip = (uint32_t)tf->regs[0];
  uint16_t port = (uint16_t)tf->regs[1];
  int protocol = (int)tf->regs[2];
  struct process *caller = current_process();
  
  extern int file_connect(struct process *caller, uint32_t ip, uint16_t port, int protocol);
  tf->regs[0] = file_connect(caller, ip, port, protocol);
}

static void sys_sleep(struct trap_frame *tf) {
  int ms = (int)tf->regs[0];
  struct process *cur = current_process();
  if (cur && ms > 0) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    cur->wake_ms = timer_get_ms() + ms;
    cur->state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
  } else {
    tf->regs[0] = 0;
  }
}

static void sys_write(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  const void *buf = (const void *)tf->regs[1];
  int size = (int)tf->regs[2];
  struct process *caller = current_process();
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int ret = file_write(caller, fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 4; // Restart syscall
      schedule(tf, 0);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

extern int load_and_run_program_in_scheduler(const char *filename, int stdin_fd, int stdout_fd, int caller_pid);
extern struct process *process_get_pcb(int pid);

struct sys_spawn_args {
    char filename[32];
    int stdin_fd;
    int stdout_fd;
    int caller_pid;
};

static struct sys_spawn_args spawn_args_pool[64];

extern void kernel_exit(void);

static void sys_spawn_worker(void *arg) {
    struct sys_spawn_args *args = (struct sys_spawn_args *)arg;
    
    int child_pid = load_and_run_program_in_scheduler(args->filename, args->stdin_fd, args->stdout_fd, args->caller_pid);
    
    struct process *caller = process_get_pcb(args->caller_pid);
    if (caller) {
        extern spinlock_t proc_lock;
        uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
        if (caller->state == PROC_STATE_WAIT_SPAWN) {
            caller->context[0] = child_pid; // return value in x0
            caller->state = PROC_STATE_READY;
        }
        spinlock_release_irqrestore(&proc_lock, flags);
    }
    
    kernel_exit();
}

static void sys_spawn(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[0];
  int stdin_fd = (int)tf->regs[1];
  int stdout_fd = (int)tf->regs[2];

  struct process *caller = current_process();
  if (!caller) {
      tf->regs[0] = -1;
      return;
  }

  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
      
      struct sys_spawn_args *args = &spawn_args_pool[caller->pid];
      
      int i = 0;
      while (filename[i] && i < 31) {
          args->filename[i] = filename[i];
          i++;
      }
      args->filename[i] = '\0';
      
      args->stdin_fd = stdin_fd;
      args->stdout_fd = stdout_fd;
      args->caller_pid = caller->pid;
      
      extern spinlock_t proc_lock;
      uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
      save_context(caller, tf);
      caller->state = PROC_STATE_WAIT_SPAWN;
      spinlock_release_irqrestore(&proc_lock, flags);
      
      extern int process_create_kernel(void (*entry)(void*), void *arg);
      process_create_kernel(sys_spawn_worker, args);
      
      schedule(tf, 0);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_pipe(struct trap_frame *tf) {
  int *fds = (int *)tf->regs[0];
  struct process *caller = current_process();
  uint64_t fds_addr = (uint64_t)fds;
  if (fds_addr >= USER_VIRT_BASE &&
      fds_addr + 8 <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int kernel_fds[2];
    int res = file_pipe(caller, kernel_fds);
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

extern uint32_t *virtio_gpu_get_framebuffer(void);
extern void virtio_gpu_flush(void);
extern void mmu_map_user_framebuffer(uint64_t phys_addr);

static void sys_map_fb(struct trap_frame *tf) {
  uint64_t phys_addr = (uint64_t)virtio_gpu_get_framebuffer();
  mmu_map_user_framebuffer(phys_addr);
  tf->regs[0] = USER_FB_VIRT_BASE; // Return user virtual address
}

static void sys_flush_fb(struct trap_frame *tf) {
  virtio_gpu_flush();
  tf->regs[0] = 0;
}

extern int virtio_input_get_events(void *buf, int max_events);

static void sys_get_events(struct trap_frame *tf) {
  void *buf = (void *)tf->regs[0];
  int max_events = (int)tf->regs[1];
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + max_events * 8 <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = virtio_input_get_events(buf, max_events);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_get_cpuid(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)get_cpuid();
}

extern int file_available(struct process *caller, int fd);
static void sys_available(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  struct process *caller = current_process();
  tf->regs[0] = file_available(caller, fd);
}

extern int fat16_read_dir(int index, char *out_name);
static void sys_read_dir(struct trap_frame *tf) {
  int index = (int)tf->regs[0];
  char *buf = (char *)tf->regs[1];
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + 12 <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = fat16_read_dir(index, buf);
  } else {
    tf->regs[0] = -1;
  }
}

extern int process_kill(int pid);
static void sys_kill(struct trap_frame *tf) {
  int pid = (int)tf->regs[0];
  tf->regs[0] = process_kill(pid);
}

/**
 * High-level handler for synchronous exceptions occurring in the kernel (EL1).
 * Typically handles fatal errors like alignment faults or kernel-level page
 * faults. If the exception is a specific yield SVC from EL1, it triggers a
 * scheduler context switch.
 *
 * @param tf Pointer to the trap frame representing the kernel state when the
 * exception occurred.
 */
void sync_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
  uint32_t ec = (esr >> 26) & 0x3F;
  uint32_t iss = esr & 0xFFFFFF;

  if (ec == 0x15 && iss == 0xFF) {
    // Yield SVC from EL1
    schedule(tf, 0);
    return;
  }

  uart_puts("[KERNEL] FATAL: CPU ");
  uart_print_hex(get_cpuid());
  uart_puts(" Synchronous Exception in EL1! ESR: ");
  uart_print_hex(esr);
  uint64_t far;
  __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  uart_puts(", FAR: ");
  uart_print_hex(far);
  uart_puts(", ELR: ");
  uart_print_hex(tf->elr);
  uart_puts("\n");
  while (1)
    ;
}

/**
 * High-level handler for synchronous exceptions occurring in user space (EL0).
 * This function dispatches system calls based on the SVC instruction's
 * immediate value and the syscall number in x8. It also catches memory
 * protection violations (Data/Instruction Aborts) and safely terminates the
 * offending process.
 *
 * @param tf Pointer to the trap frame representing the user state when the
 * exception occurred.
 */
void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t esr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

  uint64_t ec = (esr >> 26) & 0x3F;
  uint32_t iss = esr & 0xFFFFFF;

  // EC == 0x15 indicates SVC instruction generated the exception in AArch64
  // state
  if (ec == 0x15) {
    if (iss == 0xFF) {
      // Yield SVC
      int prev_pid = current_process() ? current_process()->pid : -1;
      schedule(tf, 0);
      int next_pid = current_process() ? current_process()->pid : -1;
      if (prev_pid == next_pid && prev_pid != -1) {
          safe_wfi();
      }
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
    } else if (syscall_num == SYS_GET_EVENTS) {
      sys_get_events(tf);
    } else if (syscall_num == SYS_AVAILABLE) {
      sys_available(tf);
    } else if (syscall_num == SYS_READ_DIR) {
      sys_read_dir(tf);
    } else if (syscall_num == SYS_KILL) {
      sys_kill(tf);
    } else if (syscall_num == SYS_YIELD) {
      schedule(tf, 1);
    } else if (syscall_num == SYS_CONNECT) {
      sys_connect(tf);
    } else if (syscall_num == SYS_SLEEP) {
      sys_sleep(tf);
    } else if (syscall_num == 0xFF) {
      schedule(tf, 1);
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -1; // Return error
    }
  } else if (ec == 0x20 || ec == 0x24 || ec == 0x00) {
    // EC = 0x20: Instruction Abort from a lower Exception Level
    // EC = 0x24: Data Abort from a lower Exception Level
    // EC = 0x00: Unknown Reason (e.g. executing zeroes)

    // Terminate the user program
    struct process *cur = current_process();
    if (cur) {
      uart_puts("[KERNEL] User process ");
      print_int(cur->pid);
      if (cur->name[0] != '\0') {
        uart_puts(" (");
        uart_puts(cur->name);
        uart_puts(")");
      }
      uart_puts(" fault! EC: ");
      uart_print_hex(ec);
      uart_puts(" ELR: ");
      uart_print_hex(tf->elr);
      uart_puts("\n");
      process_exit(tf);
    } else {
      uart_puts("\n[KERNEL] FATAL: EL0 Synchronous Exception with no running process!\n");
      uart_puts("EC: ");
      uart_print_hex(ec);
      uart_puts("\nELR: ");
      uart_print_hex(tf->elr);
      uart_puts("\n");
      while (1) {
        safe_wfi();
      }
    }
  } else {
    // Unhandled Synchronous exception from EL0
    uart_puts("\n[KERNEL] FATAL: Unhandled EL0 Synchronous Exception!\n");
    uart_puts("EC: ");
    uart_print_hex(ec);
    uart_puts("\nELR: ");
    uart_print_hex(tf->elr);
    uart_puts("\n");

    struct process *cur = current_process();
    if (cur) {
      process_exit(tf);
    } else {
      while (1) {
        safe_wfi();
      }
    }
  }
}

/**
 * High-level handler for hardware interrupts (IRQs) occurring in user space
 * (EL0). Handles timer interrupts for preemption (yielding to the scheduler)
 * and routes hardware device interrupts (like VirtIO block and input devices)
 * to their respective handlers.
 *
 * @param tf Pointer to the trap frame representing the user state when the
 * interrupt occurred.
 */
void irq_lower_handler_c(struct trap_frame *tf) {
  uint32_t intid = gic_acknowledge_interrupt();

  if (intid == TIMER_PPI_INTID) {
    // Timer tick — perform a context switch if the scheduler is active
    struct process *cur = current_process();
    if (cur) {
      timer_reload();
      gic_end_interrupt(intid);
      schedule(tf, 0);
      return;
    }
    // Not under scheduler — just reload and continue
    timer_reload();
  } else if (intid == virtio_blk_irq) {
    virtio_blk_handle_irq();
  } else {
    extern int virtio_net_irq;
    extern void virtio_net_handle_irq(void);
    if (intid == (uint32_t)virtio_net_irq) {
      virtio_net_handle_irq();
    } else if (intid >= 48 && intid <= 79) {
      extern void virtio_input_handle_irq(int irq);
      virtio_input_handle_irq(intid);
    }
  }

  gic_end_interrupt(intid);
}
