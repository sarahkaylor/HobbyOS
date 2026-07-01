#include "trap.h"
#include "fs.h"
#include "process.h"
#include "setjmp.h"
#include "timer.h"
#include "lock.h"
#include "arch/cpu.h"
#include "arch/mmu.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

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
#define SYS_GET_ARGS (20)
#define SYS_SYSINFO (21)
#define SYS_UNLINK (22)
#define SYS_RENAME (23)

#include "fat16.h"

struct cpu_local {
    uint64_t kernel_stack;
    uint64_t user_rsp;
    uint64_t temp_rax;
    uint64_t user_sp_temp;
    uint64_t cpu_id;
    struct process *current_proc;
} __attribute__((packed));

struct cpu_local cpu_locals[MAX_CPUS];

void save_user_sp_helper(void) {
    uint32_t cpu = get_cpuid();
    arch_set_user_sp(cpu_locals[cpu].user_sp_temp);
}

void restore_user_sp_helper(void) {
    uint32_t cpu = get_cpuid();
    cpu_locals[cpu].user_sp_temp = arch_get_user_sp();
}

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[5]; // rdi
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
  const char *filename = (const char *)tf->regs[5]; // rdi
  struct process *caller = current_process();
  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = file_open(caller, filename);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_close(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  uint32_t cpu = get_cpuid();
  struct process *caller = current_process();
  
  uart_puts("[sys_close Debug] CPU=");
  print_int(cpu);
  uart_puts(", PIDs=[");
  print_int(cpu_current_pids[0]);
  uart_puts(",");
  print_int(cpu_current_pids[1]);
  uart_puts(",");
  print_int(cpu_current_pids[2]);
  uart_puts(",");
  print_int(cpu_current_pids[3]);
  uart_puts("]");
  if (caller) {
      uart_puts(", caller_pid=");
      print_int(caller->pid);
      uart_puts(", fd=");
      print_int(fd);
      uart_puts(", g_fd=");
      print_int(caller->open_fds[fd]);
  } else {
      uart_puts(", caller is NULL");
  }
  uart_puts("\n");

  int ret = file_close(caller, fd);
  uart_puts("[KERNEL Debug] sys_close: fd=");
  print_int(fd);
  uart_puts(", ret=");
  print_int(ret);
  uart_puts("\n");
  tf->regs[0] = ret;
}

static void sys_read(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  void *buf = (void *)tf->regs[4]; // rsi
  int size = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int ret = file_read(caller, fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 2; // Restart syscall (syscall instruction is 2 bytes on x86_64, whereas svc is 4 bytes on ARM)
      schedule(tf, 0);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_get_args(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[5]; // rdi
  int size = (int)tf->regs[4]; // rsi
  struct process *cur = current_process();
  if (cur && buf && (uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int i = 0;
    while (cur->args[i] && i < size - 1) {
      buf[i] = cur->args[i];
      i++;
    }
    buf[i] = '\0';
    tf->regs[0] = 0;
  } else {
    tf->regs[0] = -1;
  }
}

struct sys_meminfo {
    uint64_t total_bytes;
    uint64_t free_bytes;
};

struct sys_netinfo {
    uint32_t ip;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint8_t mac[6];
};

static void sys_sysinfo(struct trap_frame *tf) {
  int cmd = (int)tf->regs[5]; // rdi
  void *buf = (void *)tf->regs[4]; // rsi
  int size = (int)tf->regs[3]; // rdx

  if (cmd == 1) { // Uptime
    extern uint64_t timer_get_ms(void);
    tf->regs[0] = timer_get_ms();
    return;
  }

  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    if (cmd == 2) { // Memory usage
      if (size >= (int)sizeof(struct sys_meminfo)) {
        struct sys_meminfo *info = (struct sys_meminfo *)buf;
        int total_blocks = process_get_total_blocks();
        int used_blocks = process_get_used_blocks();
        info->total_bytes = (uint64_t)total_blocks * USER_REGION_SIZE;
        info->free_bytes = (uint64_t)(total_blocks - used_blocks) * USER_REGION_SIZE;
        tf->regs[0] = 0;
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 3) { // Process list
      int count = size / sizeof(struct sys_procinfo);
      tf->regs[0] = process_get_info_list((struct sys_procinfo *)buf, count);
    } else if (cmd == 4) { // Network interface config
      if (size >= (int)sizeof(struct sys_netinfo)) {
        struct sys_netinfo *info = (struct sys_netinfo *)buf;
        extern uint32_t net_get_ip(void);
        extern uint32_t net_get_netmask(void);
        extern uint32_t net_get_gateway(void);
        extern void net_get_mac(uint8_t mac[6]);
        info->ip = net_get_ip();
        info->subnet_mask = net_get_netmask();
        info->gateway = net_get_gateway();
        net_get_mac(info->mac);
        tf->regs[0] = 0;
      } else {
        tf->regs[0] = -1;
      }
    } else {
      tf->regs[0] = -1;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_unlink(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; // rdi
  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = fat16_unlink(filename);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_rename(struct trap_frame *tf) {
  const char *oldname = (const char *)tf->regs[5]; // rdi
  const char *newname = (const char *)tf->regs[4]; // rsi
  if ((uint64_t)oldname >= USER_VIRT_BASE &&
      (uint64_t)oldname < (USER_VIRT_BASE + USER_REGION_SIZE) &&
      (uint64_t)newname >= USER_VIRT_BASE &&
      (uint64_t)newname < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = fat16_rename(oldname, newname);
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_connect(struct trap_frame *tf) {
  uint32_t ip = (uint32_t)tf->regs[5]; // rdi
  uint16_t port = (uint16_t)tf->regs[4]; // rsi
  int protocol = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();
  
  extern int file_connect(struct process *caller, uint32_t ip, uint16_t port, int protocol);
  tf->regs[0] = file_connect(caller, ip, port, protocol);
}

static void sys_sleep(struct trap_frame *tf) {
  int ms = (int)tf->regs[5]; // rdi
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
  int fd = (int)tf->regs[5]; // rdi
  const void *buf = (const void *)tf->regs[4]; // rsi
  int size = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int ret = file_write(caller, fd, buf, size, tf);
    if (ret == -2) {
      tf->elr -= 2; // Restart syscall
      schedule(tf, 0);
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -1;
  }
}

extern int load_and_run_program_in_scheduler(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid);
extern struct process *process_get_pcb(int pid);

struct sys_spawn_args {
    char filename[32];
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int caller_pid;
    char args[256];
};

static struct sys_spawn_args spawn_args_pool[64];
extern void kernel_exit(void);

static void sys_spawn_worker(void *arg) {
    struct sys_spawn_args *args = (struct sys_spawn_args *)arg;
    int child_pid = load_and_run_program_in_scheduler(args->filename, args->stdin_fd, args->stdout_fd, args->stderr_fd, args->caller_pid);
    
    struct process *child = process_get_pcb(child_pid);
    if (child) {
        int k = 0;
        while (args->args[k] && k < 255) {
            child->args[k] = args->args[k];
            k++;
        }
        child->args[k] = '\0';
    }

    struct process *caller = process_get_pcb(args->caller_pid);
    uart_puts("[KERNEL Debug] sys_spawn_worker: child_pid=");
    print_int(child_pid);
    uart_puts(", caller=");
    print_int(args->caller_pid);
    uart_puts("\n");
    if (caller) {
        extern spinlock_t proc_lock;
        uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
        if (caller->state == PROC_STATE_WAIT_SPAWN) {
            caller->context[0] = child_pid; // return value in rax / x0
            caller->state = PROC_STATE_READY;
        }
        spinlock_release_irqrestore(&proc_lock, flags);
    }
    kernel_exit();
}

static void sys_spawn(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; // rdi
  int stdin_fd = (int)tf->regs[4]; // rsi
  int stdout_fd = (int)tf->regs[3]; // rdx
  int stderr_fd = (int)tf->regs[2]; // rcx
  const char *args_ptr = (const char *)tf->regs[7]; // r8

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
      args->stderr_fd = stderr_fd;
      args->caller_pid = caller->pid;

      if (args_ptr && (uint64_t)args_ptr >= USER_VIRT_BASE &&
          (uint64_t)args_ptr < (USER_VIRT_BASE + USER_REGION_SIZE)) {
          int k = 0;
          while (args_ptr[k] && k < 255) {
              args->args[k] = args_ptr[k];
              k++;
          }
          args->args[k] = '\0';
      } else {
          args->args[0] = '\0';
      }
      
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
  int *fds = (int *)tf->regs[5]; // rdi
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
  void *buf = (void *)tf->regs[5]; // rdi
  int max_events = (int)tf->regs[4]; // rsi
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

static void sys_available(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  struct process *caller = current_process();
  int ret = file_available(caller, fd);
  tf->regs[0] = ret;
}

extern int fat16_read_dir(int index, char *out_name);
static void sys_read_dir(struct trap_frame *tf) {
  int index = (int)tf->regs[5]; // rdi
  char *buf = (char *)tf->regs[4]; // rsi
  if ((uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + 12 <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    tf->regs[0] = fat16_read_dir(index, buf);
  } else {
    tf->regs[0] = -1;
  }
}

extern int process_kill(int pid);
extern struct process *process_get_pcb(int pid);
static void sys_kill(struct trap_frame *tf) {
  int pid = (int)tf->regs[5]; // rdi
  int sig = (int)tf->regs[4]; // rsi
  if (sig == 0) {
    struct process *p = process_get_pcb(pid);
    if (p && p->state != PROC_STATE_FREE && p->state != PROC_STATE_EXITED) {
      tf->regs[0] = 0; // rax
    } else {
      tf->regs[0] = -1;
    }
  } else {
    tf->regs[0] = process_kill(pid);
  }
}

void sync_lower_handler_c(struct trap_frame *tf) {
    uint64_t syscall_num = tf->regs[0]; // rax
    
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
    } else if (syscall_num == SYS_GET_ARGS) {
      sys_get_args(tf);
    } else if (syscall_num == SYS_SYSINFO) {
      sys_sysinfo(tf);
    } else if (syscall_num == SYS_UNLINK) {
      sys_unlink(tf);
    } else if (syscall_num == SYS_RENAME) {
      sys_rename(tf);
    } else if (syscall_num == 0xFF) {
      schedule(tf, 1);
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -1;
    }
}

static void safe_print_int(int val) {
    if (val < 0) {
        uart_putc('-');
        val = -val;
    }
    if (val == 0) {
        uart_putc('0');
        return;
    }
    char buf[16];
    int idx = 0;
    while (val > 0) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (idx > 0)
        uart_putc(buf[--idx]);
}

static void safe_print_hex(uint64_t val) {
    char hex_chars[] = "0123456789ABCDEF";
    uart_putc('0');
    uart_putc('x');
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex_chars[(val >> i) & 0xF]);
    }
}

void general_interrupt_handler(struct trap_frame *tf) {
    extern void gic_set_current_vector(uint32_t cpu, uint32_t vector);
    extern uint32_t gic_acknowledge_interrupt(void);
    extern void gic_end_interrupt(uint32_t intid);
    extern int virtio_net_irq;
    
    uint32_t cpu = get_cpuid();
    gic_set_current_vector(cpu, tf->vector);
    
    if (tf->vector == 32) {
        // PIT timer interrupt
        uint32_t intid = gic_acknowledge_interrupt();
        timer_reload();
        
        // Broadcast rescheduling IPI (vector 0x81) to all other cores
        if (get_cpuid() == 0) {
            *(volatile uint32_t*)(0xFEE00300) = 0x000C4081;
        }
        
        struct process *cur = current_process();
        if (cur && (cur->is_kernel_process || (tf->cs & 3) == 3)) {
            gic_end_interrupt(intid);
            schedule(tf, 0);
            return;
        }
        gic_end_interrupt(intid);
    } else if (tf->vector == 33 || tf->vector == 44) {
        uint32_t intid = gic_acknowledge_interrupt();
        extern void virtio_input_handle_irq(int irq);
        virtio_input_handle_irq(intid);
        gic_end_interrupt(intid);
    } else if (virtio_net_irq != -1 && tf->vector == (uint32_t)virtio_net_irq) {
        uint32_t intid = gic_acknowledge_interrupt();
        extern void virtio_net_handle_irq(void);
        virtio_net_handle_irq();
        gic_end_interrupt(intid);
    } else if (tf->vector >= 32 && tf->vector <= 47) {
        uint32_t intid = gic_acknowledge_interrupt();
        gic_end_interrupt(intid);
    } else if (tf->vector == 0x80) {
        // Syscall software interrupt / instruction trap
        sync_lower_handler_c(tf);
    } else if (tf->vector == 0x81) {
        // Yield software interrupt
        schedule(tf, 1);
    } else if (tf->vector < 32) {
        // Exception
        struct process *cur = current_process();
        if (cur && !cur->is_kernel_process && (tf->cs & 3) == 3) {
            uart_puts("[KERNEL] User process fault! Vector: ");
            safe_print_int(tf->vector);
            uart_puts(" RIP: ");
            safe_print_hex(tf->elr);
            uart_puts("\n");
            process_exit(tf);
        } else {
            uart_puts("[KERNEL] FATAL: Exception in Kernel Mode! Vector: ");
            safe_print_int(tf->vector);
            uart_puts(" Error Code: ");
            safe_print_hex(tf->error_code);
            uart_puts("\n");
            uart_puts("  RIP: ");
            safe_print_hex(tf->elr);
            uart_puts("  RSP: ");
            safe_print_hex(((uint64_t*)tf)[38]);
            uart_puts("\n");
            uart_puts("  CS:  ");
            safe_print_hex(tf->cs);
            uart_puts("  SS:  ");
            safe_print_hex(tf->ss);
            uart_puts("  RFLAGS: ");
            safe_print_hex(tf->spsr);
            uart_puts("\n");
            uart_puts("  RAX: ");
            safe_print_hex(tf->regs[0]);
            uart_puts("  RBX: ");
            safe_print_hex(tf->regs[1]);
            uart_puts("  RCX: ");
            safe_print_hex(tf->regs[2]);
            uart_puts("  RDX: ");
            safe_print_hex(tf->regs[3]);
            uart_puts("\n");
            uart_puts("  RDI: ");
            safe_print_hex(tf->regs[5]);
            uart_puts("  RSI: ");
            safe_print_hex(tf->regs[4]);
            uart_puts("  RBP: ");
            safe_print_hex(tf->regs[6]);
            uart_puts("\n");
            while (1);
        }
    }
}

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct idt_entry idt[256];

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[vector].base_low = handler & 0xFFFF;
    idt[vector].selector = selector;
    idt[vector].ist = 0;
    idt[vector].flags = flags;
    idt[vector].base_mid = (handler >> 16) & 0xFFFF;
    idt[vector].base_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

struct tss_entry tss_entries[MAX_CPUS];
extern uint64_t gdt_start[];

void tss_init_core_with_id(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return;
    
    // Clear the TSS entry
    for (int i = 0; i < (int)sizeof(struct tss_entry); i++) {
        ((char*)&tss_entries[cpu])[i] = 0;
    }
    
    // Set RSP0 to the kernel stack for this CPU
    extern uint64_t __stack_top;
    uint64_t kstack = (uint64_t)&__stack_top - (cpu * 64 * 1024) - 4096;
    tss_entries[cpu].rsp0 = kstack;
    tss_entries[cpu].io_map_base = sizeof(struct tss_entry);
    
    // Set up the 16-byte GDT descriptor for this CPU's TSS
    uint64_t tss_addr = (uint64_t)&tss_entries[cpu];
    uint32_t limit = sizeof(struct tss_entry) - 1;
    
    // Core 0 TSS index is 5, Core 1 index is 7, etc.
    int gdt_idx = 5 + cpu * 2;
    
    // Low 8 bytes of the descriptor
    uint64_t desc_low = 0;
    desc_low |= (limit & 0xFFFF);
    desc_low |= ((tss_addr & 0xFFFF) << 16);
    desc_low |= (((tss_addr >> 16) & 0xFF) << 32);
    desc_low |= (0x89ULL << 40); // Type: 0x89 (Present, DPL 0, 64-bit TSS available)
    desc_low |= ((uint64_t)((limit >> 16) & 0xF) << 48);
    desc_low |= (((tss_addr >> 24) & 0xFF) << 56);
    
    // High 8 bytes of the descriptor
    uint64_t desc_high = (tss_addr >> 32) & 0xFFFFFFFF;
    
    gdt_start[gdt_idx] = desc_low;
    gdt_start[gdt_idx + 1] = desc_high;
    
    // Load TSS selector (selector is gdt_idx * 8)
    uint16_t selector = gdt_idx * 8;
    __asm__ volatile("ltr %0" : : "r"(selector));
}

void tss_init_core(void) {
    tss_init_core_with_id(get_cpuid());
}

void syscall_init(void);

void trap_init_core_with_id(uint32_t cpu) {
    // Set up kernel stack for swapgs
    extern uint64_t __stack_top;
    cpu_locals[cpu].kernel_stack = (uint64_t)&__stack_top - (cpu * 64 * 1024) - 4096;
    cpu_locals[cpu].cpu_id = cpu;
    cpu_locals[cpu].current_proc = 0;
    
    // Write &cpu_locals[cpu] to Kernel GS base MSR (0xC0000102)
    uint64_t gs_base = (uint64_t)&cpu_locals[cpu];
    uint32_t low = (uint32_t)gs_base;
    uint32_t high = (uint32_t)(gs_base >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000102));
    
    // Also write it to Active GS base MSR (0xC0000101) so we are safe
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000101));

    // Load IDT for this CPU
    struct idt_ptr ptr;
    ptr.limit = sizeof(idt) - 1;
    ptr.base = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(ptr));

    // Initialize TSS for this core
    tss_init_core_with_id(cpu);

    // Initialize syscall instruction extensions
    syscall_init();
}

void trap_init_core(void) {
    trap_init_core_with_id(get_cpuid());
}

void syscall_init(void) {
    // 1. Enable SCE in EFER (bit 0)
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    low |= 1;
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080));
    
    // 2. Set STAR MSR (0xC0000081)
    uint32_t star_low = 0;
    uint32_t star_high = (0x08ULL << 0) | (0x18ULL << 16);
    __asm__ volatile("wrmsr" : : "a"(star_low), "d"(star_high), "c"(0xC0000081));
    
    // 3. Set LSTAR MSR (0xC0000082)
    extern void syscall_entry(void);
    uint64_t handler_addr = (uint64_t)syscall_entry;
    uint32_t lstar_low = (uint32_t)handler_addr;
    uint32_t lstar_high = (uint32_t)(handler_addr >> 32);
    __asm__ volatile("wrmsr" : : "a"(lstar_low), "d"(lstar_high), "c"(0xC0000082));
    
    // 4. Set SFMASK MSR (0xC0000084) (mask IF 0x200)
    __asm__ volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(0xC0000084));
}

// Exception assembly entries
#define EXCEPTION_NO_ERR(num) \
    extern void exception_##num(void); \
    __asm__( \
        ".intel_syntax noprefix\n" \
        ".global exception_" #num "\n" \
        "exception_" #num ":\n" \
        "    push 0\n" \
        "    push " #num "\n" \
        "    jmp common_trap_wrapper\n" \
        ".att_syntax prefix\n" \
    )

#define EXCEPTION_ERR(num) \
    extern void exception_##num(void); \
    __asm__( \
        ".intel_syntax noprefix\n" \
        ".global exception_" #num "\n" \
        "exception_" #num ":\n" \
        "    push " #num "\n" \
        "    jmp common_trap_wrapper\n" \
        ".att_syntax prefix\n" \
    )

EXCEPTION_NO_ERR(0); EXCEPTION_NO_ERR(1); EXCEPTION_NO_ERR(2); EXCEPTION_NO_ERR(3);
EXCEPTION_NO_ERR(4); EXCEPTION_NO_ERR(5); EXCEPTION_NO_ERR(6); EXCEPTION_NO_ERR(7);
EXCEPTION_ERR(8);    EXCEPTION_NO_ERR(9); EXCEPTION_ERR(10);   EXCEPTION_ERR(11);
EXCEPTION_ERR(12);   EXCEPTION_ERR(13);   EXCEPTION_ERR(14);   EXCEPTION_NO_ERR(15);
EXCEPTION_NO_ERR(16);EXCEPTION_ERR(17);   EXCEPTION_NO_ERR(18);EXCEPTION_NO_ERR(19);
EXCEPTION_NO_ERR(20);EXCEPTION_NO_ERR(21);EXCEPTION_NO_ERR(22);EXCEPTION_NO_ERR(23);
EXCEPTION_NO_ERR(24);EXCEPTION_NO_ERR(25);EXCEPTION_NO_ERR(26);EXCEPTION_NO_ERR(27);
EXCEPTION_NO_ERR(28);EXCEPTION_NO_ERR(29);EXCEPTION_NO_ERR(30);EXCEPTION_NO_ERR(31);

// PIT timer
EXCEPTION_NO_ERR(32);
// Keyboard
EXCEPTION_NO_ERR(33);
EXCEPTION_NO_ERR(34);
EXCEPTION_NO_ERR(35);
EXCEPTION_NO_ERR(36);
EXCEPTION_NO_ERR(37);
EXCEPTION_NO_ERR(38);
EXCEPTION_NO_ERR(39);
EXCEPTION_NO_ERR(40);
EXCEPTION_NO_ERR(41);
EXCEPTION_NO_ERR(42);
EXCEPTION_NO_ERR(43);
// Mouse
EXCEPTION_NO_ERR(44);
EXCEPTION_NO_ERR(45);
EXCEPTION_NO_ERR(46);
EXCEPTION_NO_ERR(47);
// Yield
EXCEPTION_NO_ERR(129); // 0x81

__asm__(
".intel_syntax noprefix\n"
".global syscall_entry\n"
"syscall_entry:\n"
"    mov gs:[8], rsp\n"       /* Save User RSP */
"    mov rsp, gs:[0]\n"       /* Load Kernel Stack Pointer */
"    \n"
"    /* Push hardware fields manually */\n"
"    push 0x23\n"             /* SS */
"    push qword ptr gs:[8]\n" /* User RSP */
"    push r11\n"              /* RFLAGS */
"    push 0x1B\n"             /* CS */
"    push rcx\n"              /* RIP */
"    push 0\n"                /* Error Code */
"    push 0x80\n"             /* Vector = 0x80 */
"    \n"
"    jmp common_trap_wrapper\n"
".att_syntax prefix\n"
);

__asm__(
".intel_syntax noprefix\n"
"common_trap_wrapper:\n"
"    /* Save original rax */\n"
"    mov gs:[16], rax\n"
"    \n"
"    /* Allocate trap_frame space */\n"
"    sub rsp, 264\n"
"    \n"
"    /* Save registers */\n"
"    mov rax, gs:[16]\n"
"    mov [rsp + 0], rax\n"
"    mov [rsp + 8], rbx\n"
"    mov [rsp + 16], rcx\n"
"    mov [rsp + 24], rdx\n"
"    mov [rsp + 32], rsi\n"
"    mov [rsp + 40], rdi\n"
"    mov [rsp + 48], rbp\n"
"    mov [rsp + 56], r8\n"
"    mov [rsp + 64], r9\n"
"    mov [rsp + 72], r10\n"
"    mov [rsp + 80], r11\n"
"    mov [rsp + 88], r12\n"
"    mov [rsp + 96], r13\n"
"    mov [rsp + 104], r14\n"
"    mov [rsp + 112], r15\n"
"    \n"
"    /* Clear remaining regs[15..29] and lr */\n"
"    mov qword ptr [rsp + 120], 0\n"
"    mov qword ptr [rsp + 128], 0\n"
"    mov qword ptr [rsp + 136], 0\n"
"    mov qword ptr [rsp + 144], 0\n"
"    mov qword ptr [rsp + 152], 0\n"
"    mov qword ptr [rsp + 160], 0\n"
"    mov qword ptr [rsp + 168], 0\n"
"    mov qword ptr [rsp + 176], 0\n"
"    mov qword ptr [rsp + 184], 0\n"
"    mov qword ptr [rsp + 192], 0\n"
"    mov qword ptr [rsp + 200], 0\n"
"    mov qword ptr [rsp + 208], 0\n"
"    mov qword ptr [rsp + 216], 0\n"
"    mov qword ptr [rsp + 224], 0\n"
"    mov qword ptr [rsp + 232], 0\n"
"    mov qword ptr [rsp + 240], 0\n" /* lr */
"    \n"
"    /* Save User RSP to user_sp_temp only if coming from user mode (CS bottom 2 bits are 3) */\n"
"    mov rax, [rsp + 288]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    jne 1f\n"
"    mov rax, [rsp + 304]\n"
"    mov gs:[24], rax\n"
"    call save_user_sp_helper\n"
"1:\n"
"    \n"
"    /* Rearrange fields in trap_frame */\n"
"    /* 1. RIP ([rsp + 280]) -> elr ([rsp + 248]) */\n"
"    mov rax, [rsp + 280]\n"
"    mov [rsp + 248], rax\n"
"    \n"
"    /* 2. RFLAGS ([rsp + 296]) -> spsr ([rsp + 256]) */\n"
"    mov rax, [rsp + 296]\n"
"    mov [rsp + 256], rax\n"
"    \n"
"    /* 3. CS ([rsp + 288]) -> cs ([rsp + 280]) */\n"
"    mov rax, [rsp + 288]\n"
"    mov [rsp + 280], rax\n"
"    \n"
"    /* 4. SS ([rsp + 312]) -> ss ([rsp + 288]) */\n"
"    mov rax, [rsp + 312]\n"
"    mov [rsp + 288], rax\n"
"    \n"
"    /* Call C Handler */\n"
"    mov rdi, rsp\n"
"    call general_interrupt_handler\n"
"    \n"
"    /* Restore user_sp only if we are returning to user mode */\n"
"    mov rax, [rsp + 280]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    jne 2f\n"
"    call restore_user_sp_helper\n"
"2:\n"
"    \n"
".global common_trap_exit\n"
"common_trap_exit:\n"
"    /* Restore registers */\n"
"    mov rbx, [rsp + 8]\n"
"    mov rcx, [rsp + 16]\n"
"    mov rdx, [rsp + 24]\n"
"    mov rsi, [rsp + 32]\n"
"    mov rdi, [rsp + 40]\n"
"    mov rbp, [rsp + 48]\n"
"    mov r8,  [rsp + 56]\n"
"    mov r9,  [rsp + 64]\n"
"    mov r10, [rsp + 72]\n"
"    mov r11, [rsp + 80]\n"
"    mov r12, [rsp + 88]\n"
"    mov r13, [rsp + 96]\n"
"    mov r14, [rsp + 104]\n"
"    mov r15, [rsp + 112]\n"
"    \n"
"    /* 1. SS ([rsp + 288]) -> [rsp + 312] */\n"
"    mov rax, [rsp + 288]\n"
"    mov [rsp + 312], rax\n"
"    \n"
"    /* 2. RSP (user_sp_temp) -> [rsp + 304] only if returning to user mode */\n"
"    mov rax, [rsp + 280]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    jne 3f\n"
"    mov rax, gs:[24]\n"
"    mov [rsp + 304], rax\n"
"3:\n"
"    \n"
"    /* 3. RFLAGS ([rsp + 256]) -> [rsp + 296] */\n"
"    mov rax, [rsp + 256]\n"
"    mov [rsp + 296], rax\n"
"    \n"
"    /* 4. CS ([rsp + 280]) -> [rsp + 288] */\n"
"    mov rax, [rsp + 280]\n"
"    mov [rsp + 288], rax\n"
"    \n"
"    /* 5. RIP ([rsp + 248]) -> [rsp + 280] */\n"
"    mov rax, [rsp + 248]\n"
"    mov [rsp + 280], rax\n"
"    \n"
"    /* Restore original rax */\n"
"    mov rax, [rsp + 0]\n"
"    \n"
"    /* Pop regs, lr, elr, spsr */\n"
"    add rsp, 264\n"
"    \n"
"    /* Pop Vector and Error Code (16 bytes) */\n"
"    add rsp, 16\n"
"    \n"
"    iretq\n"
".att_syntax prefix\n"
);

__asm__(
".intel_syntax noprefix\n"
".global enter_user_space\n"
"enter_user_space:\n"
"    /* rdi = tf, rsi = target_sp */\n"
"    cli\n"
"    sub rsi, 296\n"
"    mov r8, rsi\n"
"    \n"
"    /* Copy 37 qwords (296 bytes) from rdi to rsi */\n"
"    mov rdx, rdi\n"
"    mov rdi, rsi\n"
"    mov rsi, rdx\n"
"    mov rcx, 37\n"
"    rep movsq\n"
"    \n"
"    mov rsp, r8\n"
"    call restore_user_sp_helper\n"
"    mov rax, gs:[24]\n"
"    mov [rsp + 304], rax\n"
"    jmp common_trap_exit\n"
".att_syntax prefix\n"
);

void trap_init(void) {
    // Statically initialize cpu_id in cpu_locals for all cores so tests pass
    // even when SMP secondary cores are not initialized in KERNEL_MODE_UNIT_TEST.
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_locals[i].cpu_id = i;
    }

    // 1. Setup Exception vectors in IDT (using Ring 0 interrupt gate 0x8E)
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, 0, 0x08, 0x8E); // default null
    }
    
    // Register individual exceptions
    #define SET_EXCEPTION(num) idt_set_gate(num, (uint64_t)exception_##num, 0x08, 0x8E)
    
    SET_EXCEPTION(0); SET_EXCEPTION(1); SET_EXCEPTION(2); SET_EXCEPTION(3);
    SET_EXCEPTION(4); SET_EXCEPTION(5); SET_EXCEPTION(6); SET_EXCEPTION(7);
    SET_EXCEPTION(8); SET_EXCEPTION(9); SET_EXCEPTION(10); SET_EXCEPTION(11);
    SET_EXCEPTION(12); SET_EXCEPTION(13); SET_EXCEPTION(14); SET_EXCEPTION(15);
    SET_EXCEPTION(16); SET_EXCEPTION(17); SET_EXCEPTION(18); SET_EXCEPTION(19);
    SET_EXCEPTION(20); SET_EXCEPTION(21); SET_EXCEPTION(22); SET_EXCEPTION(23);
    SET_EXCEPTION(24); SET_EXCEPTION(25); SET_EXCEPTION(26); SET_EXCEPTION(27);
    SET_EXCEPTION(28); SET_EXCEPTION(29); SET_EXCEPTION(30); SET_EXCEPTION(31);
    
    // Register PIC interrupts (vectors 32-47)
    idt_set_gate(32, (uint64_t)exception_32, 0x08, 0x8E);
    idt_set_gate(33, (uint64_t)exception_33, 0x08, 0x8E);
    idt_set_gate(34, (uint64_t)exception_34, 0x08, 0x8E);
    idt_set_gate(35, (uint64_t)exception_35, 0x08, 0x8E);
    idt_set_gate(36, (uint64_t)exception_36, 0x08, 0x8E);
    idt_set_gate(37, (uint64_t)exception_37, 0x08, 0x8E);
    idt_set_gate(38, (uint64_t)exception_38, 0x08, 0x8E);
    idt_set_gate(39, (uint64_t)exception_39, 0x08, 0x8E);
    idt_set_gate(40, (uint64_t)exception_40, 0x08, 0x8E);
    idt_set_gate(41, (uint64_t)exception_41, 0x08, 0x8E);
    idt_set_gate(42, (uint64_t)exception_42, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)exception_43, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)exception_44, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)exception_45, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)exception_46, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)exception_47, 0x08, 0x8E);
    
    // Register software yield interrupt on vector 0x81 (with Ring 3 permissions 0xEE)
    idt_set_gate(129, (uint64_t)exception_129, 0x08, 0xEE);
}
