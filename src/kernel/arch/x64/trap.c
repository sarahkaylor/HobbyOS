#include "trap.h"
#include "fs.h"
#include "net.h"
#include "process.h"
#include "setjmp.h"
#include "timer.h"
#include "lock.h"
#include "arch/cpu.h"
#include "arch/mmu.h"
#include "syscall.h"
#include "errno.h"
#include "vfs.h"
#include "vm.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

/* Defined below; declared here for the early syscall helpers
   that are v2-aware since P2.2 S2. */
static int sys_user_range(uint64_t ptr, uint64_t len, int write);
static int sys_user_range_ok(uint64_t ptr, uint64_t len);

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

/* --- raw debugcon (0xE9) diagnostics: IRQ-safe, no uart/spinlock --- */
static void dbg_putc(char c) {
  __asm__ volatile("outb %0, %1" : : "a"(c), "Nd"((uint16_t)0xe9) : "memory");
}
static void dbg_hex(uint64_t v, int nibs) {
  for (int i = nibs - 1; i >= 0; i--) {
    int d = (int)((v >> (4 * i)) & 0xf);
    dbg_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
  }
}
static void dbg_resume(const char *tag, uint64_t v) {
  dbg_putc('['); while (*tag) dbg_putc(*tag++); dbg_putc(']');
  dbg_putc('0'); dbg_putc('x'); dbg_hex(v, 16); dbg_putc('\n');
}

void restore_user_sp_helper(void) {
  uint32_t cpu = get_cpuid();
  uint64_t v = arch_get_user_sp();
  cpu_locals[cpu].user_sp_temp = v;
}

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[5]; // rdi
  struct process *p = current_process();
  int ok;
  if (p && p->as)
    ok = (vm_range_ok(p, ptr, 1, 0) == 0); /* P2.2 (S2): v2 pointer */
  else
    ok = (ptr >= USER_VIRT_BASE && ptr < (USER_VIRT_BASE + USER_REGION_SIZE));
  if (ok) {
    uart_puts("[CONSOLE] ");
    uart_puts((const char *)ptr);
  }
  tf->regs[0] = 0;
}

static void sys_exit(struct trap_frame *tf) { process_exit(tf); }

static void sys_fork(struct trap_frame *tf) {
  int pid = process_fork(tf);
  tf->regs[0] = pid < 0 ? (uint64_t)-EAGAIN : (uint64_t)pid;
  uart_puts("[KERNEL] sys_fork: tf->regs[0] is now ");
  print_int((int)tf->regs[0]);
  uart_puts("\n");
}

static void sys_open(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; // rdi
  int flags = (int)tf->regs[4];                     // rsi (syscall arg 2)
  struct process *caller = current_process();
  if (sys_user_range_ok((uint64_t)filename, 1)) {
    /* file_open returns >= 0 (fd) or a negative errno it picked itself
       (ENOENT/EEXIST/EMFILE), which errno_ret decodes in libc. */
    tf->regs[0] = file_open(caller, filename, flags);
  } else {
    tf->regs[0] = -EFAULT;
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
    print_int(process_group(caller)->open_fds[fd]); /* P1 (D7) */
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
  tf->regs[0] = ret < 0 ? -EBADF : ret;
}

static void sys_dup(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_dup(caller, (int)tf->regs[5]);
}

static void sys_dup2(struct trap_frame *tf) {
  struct process *caller = current_process();
  int r = file_dup2(caller, (int)tf->regs[5], (int)tf->regs[4]);
  tf->regs[0] = (uint64_t)(r < 0 ? -EBADF : r);
}

static void sys_read(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  void *buf = (void *)tf->regs[4]; // rsi
  int size = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();
  if (sys_user_range((uint64_t)buf, size > 0 ? (uint64_t)size : 0, 1)) {
    int ret = file_read(caller, fd, buf, size, tf);
    if (ret == -2) {
      /* Restart the syscall on wake: rewind elr over the 2-byte `syscall`
         instruction, and save our own resume frame BEFORE switching out —
         a writer on another CPU can wake us in the window between the pipe
         marking us BLOCKED and this schedule() call, and schedule() does
         not re-save a process that is already READY.  Only rewind when the
         frame returns to USER mode: rewinding a kernel-mode frame's elr
         would make the kernel execute the user `syscall` instruction at
         CPL0, which is a #GP (silent triple-fault -> reboot loop). */
      if ((tf->cs & 3) == 3) {
        tf->elr -= 2;
      }
      save_context(caller, tf);
      schedule(tf, 0);
    } else if (ret < 0) {
      tf->regs[0] = -EBADF;
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_get_args(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[5]; // rdi
  int size = (int)tf->regs[4]; // rsi
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  if (cur && buf && sys_user_range((uint64_t)buf, (uint64_t)size, 1)) {
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
  /* F1.7 (DHCP → DNS hand-off): appended; only written when the caller's
   * buffer covers the extended size, so pre-extension callers still work. */
  uint32_t dns;
};

struct sys_cpuinfo {
  uint64_t uptime_ms;
  uint64_t total_idle_ms;
  int num_cpus;
};

struct sys_time {
  uint64_t epoch;
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int weekday;
};

struct sys_fsinfo {
  uint64_t total_bytes;
  uint64_t free_bytes;
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

  if (sys_user_range((uint64_t)buf, size > 0 ? (uint64_t)size : 0, 1)) {
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
      /* The dns field (F1.7) extends the F0 layout: a caller whose buffer
         covers only the old size still gets ip/mask/gw/mac and succeeds. */
      int base_size = (int)(sizeof(struct sys_netinfo) - sizeof(uint32_t));
      if (size >= base_size) {
        struct sys_netinfo *info = (struct sys_netinfo *)buf;
        extern uint32_t net_get_ip(void);
        extern uint32_t net_get_netmask(void);
        extern uint32_t net_get_gateway(void);
        extern void net_get_mac(uint8_t mac[6]);
        info->ip = net_get_ip();
        info->subnet_mask = net_get_netmask();
        info->gateway = net_get_gateway();
        net_get_mac(info->mac);
        if (size >= (int)sizeof(struct sys_netinfo)) {
          extern uint32_t net_get_dns(void);
          info->dns = net_get_dns();
        }
        tf->regs[0] = 0;
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 5) { // CPU usage info
      if (size >= (int)sizeof(struct sys_cpuinfo)) {
        struct sys_cpuinfo *info = (struct sys_cpuinfo *)buf;
        info->uptime_ms = timer_get_ms();
        info->total_idle_ms = process_get_total_idle_ms();
        info->num_cpus = process_get_num_cpus();
        tf->regs[0] = 0;
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 6) { // Wall-clock time (CMOS RTC)
      if (size >= (int)sizeof(struct sys_time)) {
        extern uint64_t rtc_read_epoch(void);
        extern void rtc_epoch_to_time(uint64_t epoch, int *year, int *month,
                                      int *day, int *hour, int *minute,
                                      int *second, int *weekday);
        struct sys_time *info = (struct sys_time *)buf;
        uint64_t epoch = rtc_read_epoch();
        if (epoch == 0) {
          tf->regs[0] = -1;
        } else {
          rtc_epoch_to_time(epoch, &info->year, &info->month, &info->day,
                            &info->hour, &info->minute, &info->second,
                            &info->weekday);
          info->epoch = epoch;
          tf->regs[0] = 0;
        }
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 7) { // Filesystem statistics for the cwd's filesystem
      if (size >= (int)sizeof(struct sys_fsinfo)) {
        struct sys_fsinfo *info = (struct sys_fsinfo *)buf;
        extern int vfs_stats(uint64_t *total, uint64_t *free_bytes);
        tf->regs[0] = vfs_stats(&info->total_bytes, &info->free_bytes);
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 8) { // Mount table snapshot (NFS mounts)
      int max = size / (int)sizeof(struct vfs_mountinfo);
      if (max < 0) max = 0;
      int count = vfs_mount_count();
      int n = count < max ? count : max;
      struct vfs_mountinfo *out = (struct vfs_mountinfo *)buf;
      int written = 0;
      for (int i = 0; i < count && written < n; i++) {
        if (vfs_mount_info(i, &out[written]) == 0) written++;
      }
      tf->regs[0] = written;
    } else {
      tf->regs[0] = -1;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_unlink(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; // rdi
  if (sys_user_range_ok((uint64_t)filename, 1)) {
    extern int vfs_unlink(const char *path);
    int r = vfs_unlink(filename);
    tf->regs[0] = r < 0 ? -ENOENT : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_rename(struct trap_frame *tf) {
  const char *oldname = (const char *)tf->regs[5]; // rdi
  const char *newname = (const char *)tf->regs[4]; // rsi
  if (sys_user_range_ok((uint64_t)oldname, 1) &&
      sys_user_range_ok((uint64_t)newname, 1)) {
    extern int vfs_rename(const char *oldp, const char *newp);
    int r = vfs_rename(oldname, newname);
    tf->regs[0] = r < 0 ? -ENOENT : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_connect(struct trap_frame *tf) {
  uint32_t ip = (uint32_t)tf->regs[5]; // rdi
  uint16_t port = (uint16_t)tf->regs[4]; // rsi
  int protocol = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();

  extern int file_connect(struct process *caller, uint32_t ip, uint16_t port, int protocol);
  int r = file_connect(caller, ip, port, protocol);
  tf->regs[0] = r < 0 ? -EIO : r;
}

/* ---- Phase F1 (browser.md A.1a, frozen): sockets + select (65-71) ------
 * x86_64 argument mapping (see syscall5 in user libc.c): a0..a4 live in
 * rdi, rsi, rdx, r10, r8.  The ISR saves [rsp+40]=rdi -> regs[5],
 * [rsp+32]=rsi -> regs[4], [rsp+24]=rdx -> regs[3], [rsp+72]=r10 ->
 * regs[9], [rsp+56]=r8 -> regs[7] (regs[2] holds rcx, the clobbered
 * return address — never an argument).  SYS_MMAP reads its r10 flags from
 * regs[9] the same way.  The select restart follows the pipe_read -2
 * convention (rewind ELR over the 2-byte `syscall` instruction, save our
 * own frame, schedule away). */

/* True when [ptr, ptr+len) lies inside the caller's user region: the v2
 * address-space walk for a v2 process (vm_touch demand-commits the pages),
 * the legacy 32 MiB block window for v1/kernel tasks.  `write` selects the
 * access mode: out-buffers must pass write=1 so a not-yet-touched user
 * page is materialized writable before the kernel stores into it (an EL1
 * store to an absent v2 page is a fatal kernel fault; the trap keeps
 * CR3 = the process AS, so once resident the store itself is legal). */
static int sys_user_range(uint64_t ptr, uint64_t len, int write) {
  struct process *p = current_process();
  if (p && p->as) {
    /* P2.2 (S2): a v2 process answers to the address-space walk instead
       of the legacy 32 MiB range. */
    if (len == 0) len = 1;
    return vm_range_ok(p, ptr, len, write) == 0;
  }
  if (ptr < USER_VIRT_BASE) return 0;
  if (len > USER_REGION_SIZE) return 0;
  return ptr - USER_VIRT_BASE <= USER_REGION_SIZE - len;
}

/* Read-side sugar (in-buffers, strings, structures the kernel only reads). */
static int sys_user_range_ok(uint64_t ptr, uint64_t len) {
  return sys_user_range(ptr, len, 0);
}

static void sys_socket(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_socket(caller, (int)tf->regs[5],
                                      (int)tf->regs[4], (int)tf->regs[3]);
}

static void sys_connect_fd(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_socket_connect(caller, (int)tf->regs[5],
                                              (uint32_t)tf->regs[4],
                                              (uint16_t)tf->regs[3]);
}

static void sys_fcntl(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_fcntl(caller, (int)tf->regs[5],
                                     (int)tf->regs[4], (int)tf->regs[3]);
}

static void sys_select(struct trap_frame *tf) {
  int nfds = (int)tf->regs[5]; // rdi
  struct fd_set_k *rd = (struct fd_set_k *)tf->regs[4]; // rsi
  struct fd_set_k *wr = (struct fd_set_k *)tf->regs[3]; // rdx
  struct fd_set_k *ex = (struct fd_set_k *)tf->regs[9]; // r10
  int timeout_ms = (int)tf->regs[7]; // r8
  struct process *caller = current_process();

  if ((rd && !sys_user_range_ok((uint64_t)rd, sizeof(struct fd_set_k))) ||
      (wr && !sys_user_range_ok((uint64_t)wr, sizeof(struct fd_set_k))) ||
      (ex && !sys_user_range_ok((uint64_t)ex, sizeof(struct fd_set_k)))) {
    tf->regs[0] = -EFAULT;
    return;
  }

  int r = file_select(caller, nfds, rd, wr, ex, timeout_ms);
  if (r == -2) {
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2; // rewind over the `syscall` instruction
    }
    save_context(caller, tf);
    schedule(tf, 0);
  } else {
    tf->regs[0] = (uint64_t)r;
  }
}

/* --- P1 (browser.md A.1b): threads, futex-lite, TLS (72-75) -------------
 * One else-if per syscall in the dispatch chain above (never a table).
 * Args: rdi/rsi/rdx/r10 = regs[5]/[4]/[3]/[9] (as SYS_SELECT). */

/* SYS_THREAD_CREATE (72): (entry, arg, stack, flags) -> tid | -errno. */
static void sys_thread_create(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_thread_create(
      caller, tf->regs[5], tf->regs[4], tf->regs[3], tf->regs[9]);
}

/* SYS_FUTEX (73): (uaddr, op, val, timeout_ms) -> 0/count | -errno.  A WAIT
 * park never returns here: the resume re-enters user space with
 * context[0]. */
static void sys_futex(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_futex(
      caller, tf, tf->regs[5], (int)tf->regs[4], (int64_t)tf->regs[3],
      (int64_t)tf->regs[9]);
}

/* SYS_THREAD_EXIT (74): noreturn -- exits only the calling thread. */
static void sys_thread_exit(struct trap_frame *tf) {
  process_thread_exit(tf, tf->regs[5]);
}

/* SYS_SET_TLS (75): (tls) -> 0 | -EINVAL. */
static void sys_set_tls(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_set_tls(caller, tf->regs[5]);
}

static void sys_getsockopt(struct trap_frame *tf) {
  void *val = (void *)tf->regs[9]; // r10
  int *len = (int *)tf->regs[7]; // r8
  struct process *caller = current_process();

  if (!val || !len || !sys_user_range((uint64_t)val, sizeof(int), 1) ||
      !sys_user_range((uint64_t)len, sizeof(int), 1)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)file_socket_getopt(caller, (int)tf->regs[5],
                                             (int)tf->regs[4],
                                             (int)tf->regs[3], val, len);
}

static void sys_setsockopt(struct trap_frame *tf) {
  const void *val = (const void *)tf->regs[9]; // r10
  int len = (int)tf->regs[7]; // r8
  struct process *caller = current_process();

  if (len < 0) {
    tf->regs[0] = -EINVAL;
    return;
  }
  if (val && len > 0 && !sys_user_range_ok((uint64_t)val, (uint64_t)len)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)file_socket_setopt(caller, (int)tf->regs[5],
                                             (int)tf->regs[4],
                                             (int)tf->regs[3], val, len);
}

/* SYS_GETRANDOM (F1.4): mixed kernel entropy, see net.c's entropy pool. */
static void sys_getrandom(struct trap_frame *tf) {
  void *buf = (void *)tf->regs[5]; // rdi
  uint32_t len = (uint32_t)tf->regs[4]; // rsi
  unsigned int flags = (unsigned int)tf->regs[3]; // rdx

  if (flags != 0 || len > (1u << 20)) { // GRND_* unsupported / v1 cap
    tf->regs[0] = -EINVAL;
    return;
  }
  if (len == 0) {
    tf->regs[0] = 0;
    return;
  }
  if (!sys_user_range((uint64_t)buf, len, 1)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)net_get_random_bytes(buf, len);
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
  if (sys_user_range((uint64_t)buf, size > 0 ? (uint64_t)size : 0, 0)) {
    int ret = file_write(caller, fd, buf, size, tf);
    if (ret == -2) {
      /* See sys_read: 2-byte `syscall` rewind + own-frame save.  Rewind
         only for a user-mode return (kernel-mode CPL0 `syscall` = #GP). */
      if ((tf->cs & 3) == 3) {
        tf->elr -= 2;
      }
      save_context(caller, tf);
      schedule(tf, 0);
    } else if (ret < 0) {
      tf->regs[0] = -EBADF;
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -EFAULT;
  }
}

extern int load_and_run_program_in_scheduler(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid);
extern int load_and_run_program_in_scheduler_args(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, int caller_pid, const char *args);
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
  int child_pid = load_and_run_program_in_scheduler_args(args->filename, args->stdin_fd, args->stdout_fd, args->stderr_fd, args->caller_pid, args->args);

  struct process *caller = process_get_pcb(args->caller_pid);
  if (caller) {
    extern spinlock_t proc_lock;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (caller->state == PROC_STATE_WAIT_SPAWN) {
      caller->spawn_retval = child_pid;
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

  if (sys_user_range_ok((uint64_t)filename, 1)) {
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

    if (args_ptr && sys_user_range_ok((uint64_t)args_ptr, 1)) {
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
    caller->spawn_retval = -1;  /* failure default until the worker reports */
    caller->state = PROC_STATE_WAIT_SPAWN;
    spinlock_release_irqrestore(&proc_lock, flags);

    extern int process_create_kernel_nowait(void (*entry)(void*), void *arg);
    extern uint64_t timer_get_ms(void);
    int wpid = process_create_kernel_nowait(sys_spawn_worker, args);
    /* The physical pool can be momentarily full (the boot wave, sibling
       spawns).  The caller is parked in WAIT_SPAWN and shells/tests block
       on the spawn result, so wait (bounded, ~30 min; the same poll the
       loader uses) for the worker slot instead of failing the spawn —
       a failed spawn deadlocks the caller's protocol. */
    for (int attempt = 0; attempt < 18000 && wpid < 0; attempt++) {
      uint64_t t0 = timer_get_ms();
      for (volatile int spin = 0; spin < 4000000; spin++) {
        if (timer_get_ms() - t0 >= 100u) break;
      }
      wpid = process_create_kernel_nowait(sys_spawn_worker, args);
    }
    if (wpid < 0) {
      /* no free process slot for the spawn worker: the caller must not
         sit in WAIT_SPAWN forever waiting for a worker that can never
         run — release it with a failure return instead.  The resume path
         restores rax from context[0], so the failure value must be stored
         there (the saved context still holds the spawn arguments, not a
         return value). */
      flags = spinlock_acquire_irqsave(&proc_lock);
      caller->spawn_retval = -1;
      caller->context[0] = (uint64_t)-1;
      caller->state = PROC_STATE_READY;
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)-1;
    }

    schedule(tf, 0);

    /* Deliver the child pid (or -1) once the caller is running again —
       possibly on another core.  The worker may have finished before this
       core even reached schedule(), so read the value from its dedicated
       field and write it into both the live trap frame and the saved
       context: either resume path then returns the right value. */
    caller->context[0] = (uint64_t)caller->spawn_retval;
    tf->regs[0] = (uint64_t)caller->spawn_retval;
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_pipe(struct trap_frame *tf) {
  int *fds = (int *)tf->regs[5]; // rdi
  struct process *caller = current_process();
  uint64_t fds_addr = (uint64_t)fds;
  if (sys_user_range(fds_addr, 8, 1)) {
    int kernel_fds[2];
    int res = file_pipe(caller, kernel_fds);
    if (res == 0) {
      fds[0] = kernel_fds[0];
      fds[1] = kernel_fds[1];
      tf->regs[0] = 0;
    } else {
      tf->regs[0] = -EMFILE;
    }
  } else {
    tf->regs[0] = -EFAULT;
  }
}

extern uint32_t *virtio_gpu_get_framebuffer(void);
extern void virtio_gpu_flush(void);

static void sys_map_fb(struct trap_frame *tf) {
  uint64_t phys_addr = (uint64_t)virtio_gpu_get_framebuffer();
  struct process *cur = current_process();
  if (cur)
    cur = process_group(cur);
  if (cur && cur->as) {
    /* P2.4 (S4, design 7.3): a v2 process maps the fb into its own AS at
       the reserved USER_FB_OFF slot (same physical frames, VMK_FB);
       idempotent per process. */
    tf->regs[0] = (uint64_t)vm_map_fb(cur, phys_addr);
    return;
  }
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
  if (sys_user_range((uint64_t)buf,
                     max_events > 0 ? (uint64_t)max_events * 8 : 8, 1)) {
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

extern int vfs_read_dir(const char *path, int index, char *name, int ncap, uint8_t *attr, uint32_t *size);
static void sys_read_dir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  int index = (int)tf->regs[4]; // rsi

  struct local_dirent {
    char name[32];
    uint8_t attr;
    uint32_t size;
  } __attribute__((packed));

  struct local_dirent *ud = (struct local_dirent *)tf->regs[3]; // rdx

  if (sys_user_range_ok((uint64_t)path, 1) &&
      sys_user_range((uint64_t)ud, sizeof(struct local_dirent), 1)) {

    char name_buf[32];
    uint8_t attr_val = 0;
    uint32_t size_val = 0;

    int ret = vfs_read_dir(path, index, name_buf, sizeof name_buf, &attr_val, &size_val);
    if (ret == 0) {
      int k = 0;
      while (name_buf[k] && k < 31) {
        ud->name[k] = name_buf[k];
        k++;
      }
      ud->name[k] = '\0';
      ud->attr = attr_val;
      ud->size = size_val;
      tf->regs[0] = 0;
    } else {
      tf->regs[0] = -1;
    }
  } else {
    tf->regs[0] = -1;
  }
}

static void sys_mkdir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  struct process *caller = current_process();
  if (sys_user_range_ok((uint64_t)path, 1)) {
    extern int file_mkdir(struct process *cur, const char *path);
    int r = file_mkdir(caller, path);
    tf->regs[0] = r < 0 ? -EEXIST : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* mount(source, target): mount an NFS export ("A.B.C.D:/export") at a
 * directory of the FAT volume.  The mount point is created when missing. */
static void sys_mount(struct trap_frame *tf) {
  const char *source = (const char *)tf->regs[5]; // rdi
  const char *target = (const char *)tf->regs[4]; // rsi
  if (sys_user_range_ok((uint64_t)source, 1) &&
      sys_user_range_ok((uint64_t)target, 1)) {
    extern int vfs_mount(const char *source, const char *target);
    int r = vfs_mount(source, target);
    tf->regs[0] = r < 0 ? -EINVAL : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* umount(target): unmount the NFS export mounted exactly at `target`. */
static void sys_umount(struct trap_frame *tf) {
  const char *target = (const char *)tf->regs[5]; // rdi
  if (sys_user_range_ok((uint64_t)target, 1)) {
    extern int vfs_umount(const char *target);
    int r = vfs_umount(target);
    tf->regs[0] = r < 0 ? -EINVAL : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_getcwd(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[5]; // rdi
  int size = (int)tf->regs[4]; // rsi
  struct process *caller = process_group(current_process()); /* P1 (D7) */
  if (caller && sys_user_range((uint64_t)buf, size > 0 ? (uint64_t)size : 0, 1)) {
    int len = 0;
    while (caller->cwd[len]) len++;
    if (len + 1 > size) {
      tf->regs[0] = -ERANGE;
      return;
    }
    for (int i = 0; i <= len; i++) {
      buf[i] = caller->cwd[i];
    }
    tf->regs[0] = (long)buf;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_lseek(struct trap_frame *tf) {
  struct process *caller = current_process();
  int fd = (int)tf->regs[5];     // rdi
  int64_t offset = (int64_t)tf->regs[4];  // rsi
  int whence = (int)tf->regs[3]; // rdx
  int err = 0;
  extern int64_t file_seek(struct process *p, int fd, int64_t offset,
                           int whence, int *errp);
  int64_t r = file_seek(caller, fd, offset, whence, &err);
  tf->regs[0] = r < 0 ? (uint64_t)(-err) : (uint64_t)r;
}

static void sys_stat(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5];  // rdi
  struct k_stat *st = (struct k_stat *)tf->regs[4];  // rsi
  if (sys_user_range_ok((uint64_t)path, 1) &&
      sys_user_range((uint64_t)st, sizeof(struct k_stat), 1)) {
    struct process *caller = current_process();
    int err = 0;
    extern int file_stat_path(struct process *p, const char *path,
                              struct k_stat *st, int *errp);
    if (file_stat_path(caller, path, st, &err) == 0)
      tf->regs[0] = 0;
    else
      tf->regs[0] = (uint64_t)(-err);
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_fstat(struct trap_frame *tf) {
  int fd = (int)tf->regs[5];     // rdi
  struct k_stat *st = (struct k_stat *)tf->regs[4]; // rsi
  if (sys_user_range((uint64_t)st, sizeof(struct k_stat), 1)) {
    struct process *caller = current_process();
    int err = 0;
    extern int file_stat_fd(struct process *p, int fd, struct k_stat *st,
                            int *errp);
    if (file_stat_fd(caller, fd, st, &err) == 0)
      tf->regs[0] = 0;
    else
      tf->regs[0] = (uint64_t)(-err);
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_chdir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  struct process *caller = process_group(current_process()); /* P1 (D7) */
  if (caller && sys_user_range_ok((uint64_t)path, 1)) {
    extern int vfs_chdir(const char *path, char *out_new_cwd, int cap);
    char new_cwd[128];
    if (vfs_chdir(path, new_cwd, sizeof new_cwd) == 0) {
      int k = 0;
      while (new_cwd[k] && k < 127) {
        caller->cwd[k] = new_cwd[k];
        k++;
      }
      caller->cwd[k] = '\0';
      tf->regs[0] = 0;
    } else {
      tf->regs[0] = -ENOENT;
    }
  } else {
    tf->regs[0] = -EFAULT;
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
      tf->regs[0] = -ESRCH;
    }
  } else {
    int r = process_kill(pid);
    tf->regs[0] = r < 0 ? -ESRCH : r;
  }
}

/* SYS_GETPROGNAME: copy the process's binary name into the user buffer.
 * Used by crt0 to build a correct argv[0] for main() programs. Native
 * extension: 0 on success, -EFAULT on a bad pointer, -1 if no process. */
static void sys_get_progname(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[5]; // rdi
  int size = (int)tf->regs[4]; // rsi
  struct process *caller = current_process();
  if (!caller) {
    tf->regs[0] = -1;
  } else if (buf && sys_user_range((uint64_t)buf, (uint64_t)size, 1)) {
    int i = 0;
    while (caller->name[i] && i < size - 1) {
      buf[i] = caller->name[i];
      i++;
    }
    buf[i] = '\0';
    tf->regs[0] = 0;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* SYS_GETPID / SYS_GETPPID: trivial process-identity queries. */
static void sys_getpid(struct trap_frame *tf) {
  struct process *cur = current_process();
  tf->regs[0] = cur ? (uint64_t)cur->pid : (uint64_t)-1;
}
static void sys_getppid(struct trap_frame *tf) {
  struct process *cur = current_process();
  tf->regs[0] = cur ? (uint64_t)cur->parent_pid : (uint64_t)-1;
}

/* SYS_WAITPID: shared implementation in process.c (blocks in
 * PROC_STATE_WAIT_CHILD; result delivered into the saved context). */
static void sys_waitpid(struct trap_frame *tf) {
  tf->regs[0] = process_waitpid(tf);
}

/* SYS_EXEC: replace the current image. Shared loader helper in
 * program_loader.c; here we only marshal the user path/argv.
 * P2.5 (S5 flip): v2-aware via sys_user_range_ok (the old window test
 * rejected every v2 string pointer). */
static int u_strcpy(const char *src, char *dst, int cap) {
  if (!src || cap <= 1)
    return 0;
  if (!sys_user_range_ok((uint64_t)src, (uint64_t)cap))
    return 0;
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return 1;
}

static void sys_exec(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  char *const *argv = (char *const *)tf->regs[4]; // rsi
  struct process *cur = current_process();
  if (!cur) {
    tf->regs[0] = -EINVAL;
    return;
  }

  char pathbuf[32];
  if (!u_strcpy(path, pathbuf, sizeof pathbuf)) {
    tf->regs[0] = -EFAULT;
    return;
  }

  /* P2.5 (S5): every argv access below is validated (v2 AS walk
     demand-commits / v1 window test) so a bogus array degrades instead
     of faulting the kernel.  The eargv blob MUST be captured before
     process_exec_current: the v2 exec tears the caller's AS down and the
     old array pages are dead afterwards (raw reads aborted in EL1 and
     parked the CPU). */
  proc_set_argv_array(cur, argv);

  char namebuf[32];
  int named = 0;
  if (argv && sys_user_range_ok((uint64_t)argv, sizeof(char *)) &&
      u_strcpy((const char *)argv[0], namebuf, sizeof namebuf)
      && namebuf[0])
    named = 1;
  if (!named) {
    const char *b = pathbuf;
    for (int i = 0; pathbuf[i]; i++)
      if (pathbuf[i] == '/')
        b = &pathbuf[i + 1];
    int i = 0;
    for (; b[i] && i < 31; i++)
      namebuf[i] = b[i];
    namebuf[i] = '\0';
  }

  char argbuf[256];
  int alen = 0;
  argbuf[0] = '\0';
  if (argv) {
    for (int ai = 1; alen < 251 &&
                     sys_user_range_ok((uint64_t)(argv + ai), sizeof(char *)) &&
                     argv[ai] != 0; ai++) {
      char one[64];
      if (!u_strcpy((const char *)argv[ai], one, sizeof one))
        break;
      if (alen)
        argbuf[alen++] = ' ';
      for (int k = 0; one[k] && alen < 255; k++)
        argbuf[alen++] = one[k];
    }
  }
  argbuf[alen] = '\0';

  int r = process_exec_current(tf, pathbuf, argbuf, namebuf);
  if (r < 0)
    tf->regs[0] = (uint64_t)r;  /* success redirects elr + sets regs[0]=0 */
}

/* --- Syscall dispatch ------------------------------------------------
 * Keyed off the shared SYS_* constants from syscall.h (single source of
 * truth — the numbers can never drift between libc.c and the trap
 * handlers).  Deliberately an if/else chain, NOT a table of function
 * pointers: the UEFI bootloader loads the kernel at a base that differs
 * from the link-time address (code stays position-independent via
 * RIP-relative addressing, but a table stores ABSOLUTE link-time
 * addresses and calling them jumps to unloaded RAM — executing zeroes on
 * the first syscall).  Any added x64 syscall must append an else-if. */

/* Last syscall each core entered; 0 = not in a syscall.  A core wedged
   inside a syscall leaves this set, which the stall watchdog prints. */
static volatile int core_in_syscall[MAX_CPUS];

void sync_lower_handler_c(struct trap_frame *tf) {
  uint64_t syscall_num = tf->regs[0]; // rax
  core_in_syscall[get_cpuid()] = (int)syscall_num;

  if (syscall_num == 0xFF) {
    schedule(tf, 1);
    return;
  }
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
  } else if (syscall_num == SYS_DUP) {
    sys_dup(tf);
  } else if (syscall_num == SYS_DUP2) {
    sys_dup2(tf);
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
  } else if (syscall_num == SYS_MKDIR) {
    sys_mkdir(tf);
  } else if (syscall_num == SYS_GETCWD) {
    sys_getcwd(tf);
  } else if (syscall_num == SYS_CHDIR) {
    sys_chdir(tf);
  } else if (syscall_num == SYS_LSEEK) {
    sys_lseek(tf);
  } else if (syscall_num == SYS_STAT) {
    sys_stat(tf);
  } else if (syscall_num == SYS_FSTAT) {
    sys_fstat(tf);
  } else if (syscall_num == SYS_MOUNT) {
    sys_mount(tf);
  } else if (syscall_num == SYS_UMOUNT) {
    sys_umount(tf);
  } else if (syscall_num == SYS_GETPID) {
    sys_getpid(tf);
  } else if (syscall_num == SYS_GETPPID) {
    sys_getppid(tf);
  } else if (syscall_num == SYS_WAITPID) {
    sys_waitpid(tf);
  } else if (syscall_num == SYS_EXEC) {
    sys_exec(tf);
  } else if (syscall_num == SYS_GETPROGNAME) {
    sys_get_progname(tf);
  } else if (syscall_num == SYS_GETARGV) {
    tf->regs[0] = (uint64_t)sys_readargv(current_process(),
                                         (int)tf->regs[5], /* rdi: idx */
                                         (char *)tf->regs[4], /* rsi: buf */
                                         (int)tf->regs[3]);  /* rdx: size */
  } else if (syscall_num == SYS_BRK) {
    tf->regs[0] = (uint64_t)sys_brk(tf->regs[5]); /* rdi */
  } else if (syscall_num == SYS_MMAP) {
    /* P2.3 (S3, design section 4.2): 6-arg Linux shape.
       rdi=r5, rsi=r4, rdx=r3, r10=r9, r8=r7, r9=r8 (the x64 reg map). */
    tf->regs[0] = (uint64_t)sys_mmap6(tf->regs[5],  /* rdi: addr */
                                      tf->regs[4],  /* rsi: len */
                                      (int64_t)tf->regs[3],  /* rdx: prot */
                                      (int64_t)tf->regs[9],  /* r10: flags */
                                      (int64_t)tf->regs[7],  /* r8: fd */
                                      tf->regs[8]);          /* r9: offset */
  } else if (syscall_num == SYS_MUNMAP) {
    tf->regs[0] = (uint64_t)sys_munmap(tf->regs[5], /* rdi: addr */
                                       tf->regs[4]); /* rsi: len */
  } else if (syscall_num == SYS_MPROTECT) {
    tf->regs[0] = (uint64_t)sys_mprotect(tf->regs[5],  /* rdi: addr */
                                         tf->regs[4],  /* rsi: len */
                                         (int64_t)tf->regs[3]); /* rdx: prot */
  } else if (syscall_num == SYS_MADVISE) {
    tf->regs[0] = (uint64_t)sys_madvise(tf->regs[5],  /* rdi: addr */
                                        tf->regs[4],  /* rsi: len */
                                        (int64_t)tf->regs[3]); /* rdx: advice */
  } else if (syscall_num == SYS_FTRUNCATE) {
    /* P2.4 (S4, design 4.3): (fd, size) -> rdi/rsi, memfd only. */
    tf->regs[0] = (uint64_t)sys_ftruncate((int64_t)tf->regs[5], tf->regs[4]);
  } else if (syscall_num == SYS_MEMFD_CREATE) {
    /* P2.4 (S4, design 4.3): (name*, flags) -> rdi/rsi. */
    tf->regs[0] =
      (uint64_t)sys_memfd_create(tf->regs[5], (int64_t)tf->regs[4]);
  } else if (syscall_num == SYS_SOCKET) {
    sys_socket(tf);
  } else if (syscall_num == SYS_CONNECT_FD) {
    sys_connect_fd(tf);
  } else if (syscall_num == SYS_SELECT) {
    sys_select(tf);
  } else if (syscall_num == SYS_FCNTL) {
    sys_fcntl(tf);
  } else if (syscall_num == SYS_GETSOCKOPT) {
    sys_getsockopt(tf);
  } else if (syscall_num == SYS_SETSOCKOPT) {
    sys_setsockopt(tf);
  } else if (syscall_num == SYS_GETRANDOM) {
    sys_getrandom(tf);
  } else if (syscall_num == SYS_THREAD_CREATE) {
    sys_thread_create(tf);
  } else if (syscall_num == SYS_FUTEX) {
    sys_futex(tf);
  } else if (syscall_num == SYS_THREAD_EXIT) {
    sys_thread_exit(tf);
  } else if (syscall_num == SYS_SET_TLS) {
    sys_set_tls(tf);
  } else {
    uart_puts("Unknown System Call Invoked!\n");
    tf->regs[0] = -ENOSYS;
  }
  core_in_syscall[get_cpuid()] = 0;
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

/* Stall watchdog: records each core's last-seen context on every interrupt
   and complains when the console has been silent for a long time while work
   should be running.  A silent hang otherwise leaves no trace; each core's
   last-seen RIP tells where it was when the lights went out. */
#define WATCHDOG_SILENCE_MS 20000
#define WATCHDOG_COOLDOWN_MS 30000

static volatile uint64_t wd_last_seen_ms[MAX_CPUS];
static volatile uint64_t wd_last_seen_rip[MAX_CPUS];
static volatile int wd_last_seen_pid[MAX_CPUS];
static volatile uint64_t wd_next_report_ms = 0;
/* Full last frame per core (copied at every tick) so the stall dump can
   show the register state + live stack pointer of every core, not just the
   interrupted RIP.  Key offsets (see save_context / the asm wrapper): the
   frame mirrors the push order of the interrupt wrapper — for x64 the
   generic layout is signed-off in trap.S; we record the whole struct. */
static struct trap_frame wd_last_frame[MAX_CPUS];
static volatile uint64_t wd_last_tfp[MAX_CPUS]; /* stack addr of that frame */
static volatile uint64_t wd_ticks[MAX_CPUS];     /* interrupt count per core */

static void watchdog_tick(uint32_t cpu, struct trap_frame *tf) {
  extern volatile uint64_t uart_last_activity_ms;
  extern volatile uint64_t lock_wait_addr[MAX_CPUS];
  extern volatile uint64_t lock_wait_caller[MAX_CPUS];
  extern volatile uint64_t lock_wait_caller2[MAX_CPUS];
  /* Stall-watchdog prints must be able to diagnose a wedge in uart_lock
     itself, so they go out through the lock-free raw sink. */
  extern void uart_puts_raw(const char *s);
  extern void uart_print_hex_raw(uint64_t v);
  extern void print_int_raw(int val);
  extern volatile uint64_t cpu_heartbeat_ms[];
  struct process *cur = current_process();
  uint64_t now = timer_get_ms();

  /* Per-CPU liveness heartbeat for the lost-owner reaper in process.c: a
     running CPU passes through here on every timer interrupt, so a stale
     heartbeat means the CPU is either dead (triple-fault reset) or frozen
     in an IRQ-off spin for the grace period — both count as 'dead' for
     reclaiming the RUNNING process it owns. */
  cpu_heartbeat_ms[cpu] = now;

  wd_last_seen_ms[cpu] = now;
  wd_last_seen_rip[cpu] = tf->elr;
  wd_last_seen_pid[cpu] = cur ? cur->pid : -1;
  wd_last_frame[cpu] = *tf;
  wd_last_tfp[cpu] = (uint64_t)tf;
  wd_ticks[cpu]++;

  if (now < wd_next_report_ms) {
    return;
  }
  if (now - uart_last_activity_ms < WATCHDOG_SILENCE_MS) {
    return;
  }
  wd_next_report_ms = now + WATCHDOG_COOLDOWN_MS;
  uart_puts_raw("[WATCHDOG] console silent for more than 20s\n");
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    uart_puts_raw("  CPU");
    print_int_raw((int)c);
    uart_puts_raw(" rip=");
    uart_print_hex_raw(wd_last_seen_rip[c]);
    uart_puts_raw(" pid=");
    print_int_raw(wd_last_seen_pid[c]);
    uart_puts_raw(" age_ms=");
    print_int_raw((int)(now - wd_last_seen_ms[c]));
    if (core_in_syscall[c] != 0) {
      uart_puts_raw(" syscall=");
      print_int_raw(core_in_syscall[c]);
    }
    if (lock_wait_addr[c] != 0) {
      uart_puts_raw(" wait_lock=");
      uart_print_hex_raw(lock_wait_addr[c]);
      uart_puts_raw(" caller=");
      uart_print_hex_raw(lock_wait_caller[c]);
      if (lock_wait_caller2[c] != 0) {
        uart_puts_raw(" caller2=");
        uart_print_hex_raw(lock_wait_caller2[c]);
      }
    }
    uart_puts_raw("\n");
  }

  /* Same dump on COM2 — a side-channel that never interleaves with the
     console, so the stall forensics come out byte-exact. */
  extern void uart_puts_raw2(const char *s);
  extern void uart_print_hex_raw2(uint64_t v);
  extern void print_int_raw2(int val);
  extern void wd_dump_proc_table(void);
  uart_puts_raw2("[WD2] stall dump\n");
  wd_dump_proc_table();
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    uart_puts_raw2(" CPU");
    print_int_raw2((int)c);
    uart_puts_raw2(" rip=");
    uart_print_hex_raw2(wd_last_seen_rip[c]);
    uart_puts_raw2(" pid=");
    print_int_raw2(wd_last_seen_pid[c]);
    uart_puts_raw2(" tks=");
    print_int_raw2((int)wd_ticks[c]);
    uart_puts_raw2(" cs=");
    uart_print_hex_raw2(wd_last_frame[c].cs);
    uart_puts_raw2(" fl=");
    uart_print_hex_raw2(wd_last_frame[c].spsr);
    uart_puts_raw2(" age=");
    print_int_raw2((int)(now - wd_last_seen_ms[c]));
    uart_puts_raw2(" vec=");
    print_int_raw2((int)wd_last_frame[c].vector);
    if (lock_wait_addr[c] != 0) {
      uart_puts_raw2(" wl=");
      uart_print_hex_raw2(lock_wait_addr[c]);
      uart_puts_raw2(" cl=");
      uart_print_hex_raw2(lock_wait_caller[c]);
      if (lock_wait_caller2[c] != 0) {
        uart_puts_raw2(" c2=");
        uart_print_hex_raw2(lock_wait_caller2[c]);
      }
    }
    uart_puts_raw2("\n");
    /* Full frame regs from the stored last frame (the mapping of
       regs[0..9] := rax..r9 as pushed by the trap wrapper). */
    uart_puts_raw2("   fr:");
    for (int r = 0; r < 10; r++) {
      uart_puts_raw2(" ");
      uart_print_hex_raw2(wd_last_frame[c].regs[r]);
    }
    uart_puts_raw2("\n");
    /* The last interrupt's kernel stack region (below the frame) — the
       caller chain of what the core was actually executing. */
    uint64_t frp = wd_last_tfp[c];
    uart_puts_raw2("  deep:");
    if (frp >= 0x70000000ULL && frp < 0x75000000ULL) {
      for (uint64_t a = frp + 8; a < frp + 8 + 96; a += 8) {
        uint64_t v = *(volatile uint64_t *)a;
        uart_puts_raw2(" ");
        uart_print_hex_raw2(v);
      }
    }
    uart_puts_raw2("\n");
    /* Full stack chain: every .text return address, deepest first. */
    uint64_t top = cpu_locals[c].kernel_stack;
    int hits = 0;
    uart_puts_raw2("  stk");
    print_int_raw2((int)c);
    uart_puts_raw2(":");
    for (uint64_t a = top - 8; a > (top - 24576) && hits < 20; a -= 8) {
      uint64_t v = *(volatile uint64_t *)a;
      if (v >= 0x70000000ULL && v < 0x7001c000ULL) {
        uart_puts_raw2(" ");
        uart_print_hex_raw2(v);
        hits++;
      }
    }
    uart_puts_raw2("\n");
  }

  /* Deep-freeze forensics: for any core silent >60s, scan its kernel stack for
     return addresses into .text ([0x70000000,0x7001c000)).  The lowest hit
     approximates the deepest (newest) frame = where the core sits right now. */
  for (uint32_t c = 1; c < MAX_CPUS; c++) {
    if (now - wd_last_seen_ms[c] < 60000) continue;
    uint64_t top = cpu_locals[c].kernel_stack;
    uint64_t last_hit = 0;
    int hits = 0;
    uart_puts_raw("  CSTK cpu");
    print_int_raw((int)c);
    uart_puts_raw(":");
    for (uint64_t a = top - 8; a > (top - 24576) && hits < 16; a -= 8) {
      uint64_t v = *(volatile uint64_t *)a;
      if (v >= 0x70000000ULL && v < 0x7001c000ULL) {
        uart_puts_raw(" ");
        uart_print_hex_raw(v);
        last_hit = a;
        hits++;
      }
    }
    uart_puts_raw("\n");
    if (last_hit) {
      uart_puts_raw("  CDATA cpu");
      print_int_raw((int)c);
      uart_puts_raw(" cur~=");
      uart_print_hex_raw(last_hit);
      uart_puts_raw(":");
      for (int q = 0; q < 12; q++) {
        uart_puts_raw(" ");
        uart_print_hex_raw(*(volatile uint64_t *)(last_hit + 8 * q));
      }
      uart_puts_raw("\n");
    }
  }
}

/**
 * Sends End of Interrupt (EOI) to this core's LAPIC.
 * Required for LAPIC-delivered vectors (reschedule IPI 0x81, LVT timer):
 * without it the APIC keeps the in-service bit set and silently blocks
 * every further interrupt of that priority class.
 */
void lapic_send_eoi(void) {
  *(volatile uint32_t *)0xFEE000B0 = 0;
}

void general_interrupt_handler(struct trap_frame *tf) {
  extern void gic_set_current_vector(uint32_t cpu, uint32_t vector);
  extern uint32_t gic_acknowledge_interrupt(void);
  extern void gic_end_interrupt(uint32_t intid);
  extern int virtio_net_irq;

  uint32_t cpu = get_cpuid();
  gic_set_current_vector(cpu, tf->vector);
  watchdog_tick(cpu, tf);

  if (tf->vector == 32) {
    // PIT/LAPIC timer interrupt
    uint32_t intid = gic_acknowledge_interrupt();

    if (cpu == 0) {
      // The PIT is the global time base and is wired to the boot core only.
      timer_reload();

      // Broadcast rescheduling IPI (vector 0x81) to all other cores.
      // Bit 14 stays clear: it is defined for level-triggered delivery only.
      *(volatile uint32_t*)(0xFEE00300) = 0x000C0081;
    }

    // Per-core LVT-timer ticks arrive here too; always release the LAPIC.
    // The PIC EOI is the boot core's job alone: a stray EOI from another
    // core could clear an unrelated in-service PIC interrupt.
    lapic_send_eoi();

    struct process *cur = current_process();
    /* Only the boot core's tick preempts USER-mode contexts.  The AP LVT
       timers exist to wake stuck WFI waits and feed the liveness heartbeat,
       NOT to preempt: a per-core tick preempts a freshly-resumed user
       process at its very first instruction every single time (the count
       expires during the IRQ-off resume window, so the pending tick fires
       the instant iretq restores IF), pinning the process at 0x44000000
       forever — it never executes, never exits, pins its physical block,
       and the wave's block-waiting loader then stalls silently.  CPU0's
       PIT tick + the broadcast 0x81 IPI (also 100 Hz) are what actually
       schedule user processes around. */
    if (cur && (tf->cs & 3) == 3 && cpu == 0) {
      if (cpu == 0) {
        gic_end_interrupt(intid);
      }
      schedule(tf, 0);
      return;
    }
    if (cpu == 0) {
      gic_end_interrupt(intid);
    }
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
    // Reschedule/yield IPI. Release the LAPIC first: this vector is
    // LAPIC-delivered and would wedge at this priority without an EOI.
    lapic_send_eoi();
    /* Never preempt a USER process while it is inside its syscall
       (kernel mode, CS&3==0).  Its live kernel-mode continuation — the
       syscall handler's frames on THIS CPU's kernel stack — cannot be
       resumed on another CPU once that stack is reused (observed: #UD
       executing .bss and RIP=0x7009, common_trap_exit #GPs).  The timer
       handler already applies this guard; the broadcast IPI reaches
       kernel-mode windows too, so guard it the same way — the IPI just
       becomes a no-op for that window and the syscall finishes. */
    struct process *ipicur = current_process();
    if (ipicur && !ipicur->is_kernel_process && (tf->cs & 3) != 3) {
      return;
    }
    schedule(tf, 1);
  } else if (tf->vector == 0x82) {
    // P2.2 (S2/OQ5) TLB shootdown IPI: LAPIC-delivered, so EOI first.
    // The handler never schedules and never preempts anything: it may
    // only reload CR3 when this CPU runs the target AS (snapshot read).
    lapic_send_eoi();
    extern void vm_x64_shootdown_handler(void);
    vm_x64_shootdown_handler();
  } else if (tf->vector < 32) {
    // Exception
    struct process *cur = current_process();
    /* P2.3 (S3, design section 5.2): a v2 process's user #PF is either a
       demand fault (materialize + retry) or a kill with an exact reason.
       Error code: bit0 P (0 = not-present), bit1 W/R, bit4 I/D. */
    if (tf->vector == 14 && cur && cur->as && (tf->cs & 3) == 3) {
      uint64_t cr2;
      __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
      int pf_w = (int)(tf->error_code & 2);
      int pf_x = (int)((tf->error_code >> 4) & 1);
      int pf_present = (int)(tf->error_code & 1);
      const char *why = "PROT";
      if (!pf_present &&
          vm_handle_fault(process_group(cur), cr2, pf_w, pf_x, &why) == 0) {
        return; /* mapping is live; iretq re-executes the instruction */
      }
      uart_puts("[KERNEL] pid=");
      print_int(cur->pid);
      uart_puts(" (");
      uart_puts(cur->name);
      uart_puts(") memory fault VA=");
      uart_print_hex(cr2);
      uart_puts(" PC=");
      uart_print_hex(tf->elr);
      uart_puts(" rw=");
      uart_puts(pf_w ? "w" : "r");
      uart_puts(pf_x ? "x" : " ");
      uart_puts(" in=");
      uart_puts(why);
      uart_puts(" -> killed\n");
      process_fault_exit(tf, 11); /* SIGSEGV status byte */
      return;
    }
    if (cur && !cur->is_kernel_process && (tf->cs & 3) == 3) {
      uart_puts("[KERNEL] User process fault! Vector: ");
      safe_print_int(tf->vector);
      uart_puts(" RIP: ");
      safe_print_hex(tf->elr);
      uart_puts("\n");
      process_exit(tf);
    } else {
      /* Kernel-mode fault.  ALL of this dump goes to COM1 via the
         lock-free raw sink (NOT the 0xE9 debugcon, which QEMU usually
         never maps — the pre-fix handler's output was invisible and the
         death looked like a silent triple fault).  In a #GP/#PF the frame
         may be garbage, so the stack window below is range-guarded: an
         unchecked dereference of tf[38] was itself faulting (#PF -> #DF
         -> CPU reset) before the dump could finish. */
      extern void uart_puts_raw(const char *s);
      extern void uart_print_hex_raw(uint64_t v);
      extern void print_int_raw(int val);
      uart_puts_raw("[KERNEL] FATAL: Exception in Kernel Mode! Vector: ");
      print_int_raw((int)tf->vector);
      uart_puts_raw(" Error Code: ");
      uart_print_hex_raw(tf->error_code);
      uart_puts_raw("\n");
      uart_puts_raw("  RIP: ");
      uart_print_hex_raw(tf->elr);
      uart_puts_raw("  RSP: ");
      uart_print_hex_raw(((uint64_t*)tf)[38]);
      uart_puts_raw("\n");
      uart_puts_raw("  CS:  ");
      uart_print_hex_raw(tf->cs);
      uart_puts_raw("  SS:  ");
      uart_print_hex_raw(tf->ss);
      uart_puts_raw("  RFLAGS: ");
      uart_print_hex_raw(tf->spsr);
      uart_puts_raw("\n");
      uart_puts_raw("  RAX: ");
      uart_print_hex_raw(tf->regs[0]);
      uart_puts_raw("  RBX: ");
      uart_print_hex_raw(tf->regs[1]);
      uart_puts_raw("  RCX: ");
      uart_print_hex_raw(tf->regs[2]);
      uart_puts_raw("  RDX: ");
      uart_print_hex_raw(tf->regs[3]);
      uart_puts_raw("\n");
      uart_puts_raw("  RDI: ");
      uart_print_hex_raw(tf->regs[5]);
      uart_puts_raw("  RSI: ");
      uart_print_hex_raw(tf->regs[4]);
      uart_puts_raw("  RBP: ");
      uart_print_hex_raw(tf->regs[6]);
      uart_puts_raw("\n");
      uart_puts_raw("  CPU: ");
      print_int_raw((int)get_cpuid());
      struct process *curproc = current_process();
      uart_puts_raw("  pid: ");
      print_int_raw(curproc ? curproc->pid : -1);
      uart_puts_raw("\n");
      /* Raw stack window: even when RIP is garbage, the return addresses
         on the stack tell where the fault came from. */
      uint64_t fsp = ((uint64_t*)tf)[38];
      if (fsp >= 0x00000000ULL && fsp < 0x80000000ULL) {
        for (int si = 0; si < 16; si++) {
          uart_puts_raw("  STACK[");
          print_int_raw(si);
          uart_puts_raw("]: ");
          uart_print_hex_raw(((volatile uint64_t *)fsp)[si]);
          uart_puts_raw("\n");
        }
      } else {
        uart_puts_raw("  STACK: skipped (RSP outside identity-mapped RAM)\n");
      }
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

  /* F1.5: x87/SSE enable is per-core (CR0/CR4).  This runs on the boot core
     from mmu_init() and on every AP from secondary_main(), always before
     start_scheduler() can run any user code on the core; the early boot
     paths set the same bits in boot.s, so this is belt-and-braces. */
  extern void arch_fpu_enable_core(void);
  arch_fpu_enable_core();

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
// P2.2 (S2/OQ5): TLB shootdown IPI
EXCEPTION_NO_ERR(130); // 0x82

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
"    /* Carry the user RSP inside the trap frame (the dummy `lr` slot,\n"
"       [rsp + 240]) on EVERY entry, so it survives a process switch during\n"
"       a -2 syscall restart.  From user mode the hardware-pushed RSP\n"
"       ([rsp + 304]) is authoritative; a kernel-mode trap (timer interrupt\n"
"       inside a syscall handler) inherits the live gs:[24], which is the\n"
"       current process's user RSP from its outermost entry.  The old code\n"
"       only mirrored gs:[24] and relied on the per-CPU roundtrip to still\n"
"       hold OUR value at exit time — it does not when another process\n"
"       entered on this CPU in between, which restored a stale RSP and\n"
"       wedged the resumed task in kernel mode (the \"tarpit\" signature). */\n"
"    mov rax, [rsp + 288]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    jne 1f\n"
"    mov rax, [rsp + 304]\n"
"    mov [rsp + 240], rax\n"
"    mov gs:[24], rax\n"
"    call save_user_sp_helper\n"
"    jmp 2f\n"
"1:\n"
"    mov rax, gs:[24]\n"
"    mov [rsp + 240], rax\n"
"2:\n"
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
"    /* 2. RSP -> [rsp + 304] only if returning to user mode.  Read the\n"
"       user RSP from the frame's own lr slot ([rsp + 240]), NOT from\n"
"       gs:[24]: the frame's copy belongs to THIS trap and is immune to\n"
"       whichever process last entered or resumed on this CPU. */\n"
"    mov rax, [rsp + 280]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    jne 3f\n"
"    mov rax, [rsp + 240]\n"
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
"    /* rdi = tf (source frame), rsi = target_sp.  Stash both in\n"
"       callee-saved registers: restore_user_sp_helper is C and may\n"
"       clobber caller-saved registers. */\n"
"    mov r12, rdi\n"
"    mov r13, rsi\n"
"    call restore_user_sp_helper\n"
"    /* Same-privilege (kernel-mode) resume: common_trap_exit's iretq\n"
"       would pop only three words and leave RSP on this CPU's scratch\n"
"       buffer, severing the task from its live stack frames.  Relocate\n"
"       RIP/CS/RFLAGS onto the task's OWN stack (gs:[24] is context[33]:\n"
"       the interrupted RSP saved by save_context for a preempted task,\n"
"       the task region's stack top for a fresh one) and iretq from\n"
"       there. */\n"
"    mov rax, [r12 + 280]\n"
"    and rax, 3\n"
"    cmp rax, 3\n"
"    je 7f\n"
"    /* Kernel-mode resume: copy the register frame straight from the\n"
"       source onto the task's own stack.  Never bounce it through\n"
"       [target_sp-296, target_sp): that scratch region is the shared\n"
"       per-CPU stack, where parked kernel tasks keep their live frames,\n"
"       and copying the frame through it overwrote them (observed as a\n"
"       task resuming on a clobbered return chain, then executing data\n"
"       at an address in .bss).  In IA-32e mode an IRETQ always pops\n"
"       all five words (Intel SDM 5.14.3), so the five slots must END\n"
"       exactly at that RSP: dest = RSP - 320 writes nothing at or above\n"
"       the task's live stack pointer, and the popped RSP slot (filled\n"
"       in the tail) then resumes the task with RSP == gs:[24]. */\n"
"    mov rax, gs:[24]\n"
"    lea rdi, [rax - 320]\n"
"    mov rsi, r12\n"
"    mov rcx, 37\n"
"    rep movsq\n"
"    lea rsp, [rdi - 296]\n"
"    jmp 9f\n"
"7:\n"
"    /* User-mode resume: land the frame on this CPU's scratch buffer\n"
"       [target_sp-296, target_sp) and plant the user RSP from the RESTORED\n"
"       frame (tf->lr = the process's own saved RSP, rode context[30]/\n"
"       context[33] through the switch) — never from gs:[24], which this\n"
"       CPU may have overwritten while running other processes. */\n"
"    lea rdi, [r13 - 296]\n"
"    mov rsi, r12\n"
"    mov rcx, 37\n"
"    rep movsq\n"
"    lea rsp, [rdi - 296]\n"
"    mov rax, [r12 + 240]\n"
"    mov [rsp + 304], rax\n"
"    /* User iretq is USER by definition: stamp CS=0x1B and SS=0x23 into\n"
"       the frame slots so a frame saved carrying a stale kernel CS can\n"
"       never resume into user pages at CPL0 (the 0x440XXXXX/CS=0x08\n"
"       runaway).  The 3: tail copies [rsp+288]->SS and [rsp+280]->CS. */\n"
"    mov qword ptr [rsp + 280], 0x1B\n"
"    mov qword ptr [rsp + 288], 0x23\n"
"9:\n"
"    /* Inline copy of the common_trap_exit tail (the assembler resolves\n"
"       `jmp common_trap_exit` to a wrong offset inside the function, so\n"
"       we keep the resume path self-contained). */\n"
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
"    /* 1+2. SS and RSP slots.  Both are written unconditionally: the\n"
"       observed iretq behavior in this environment consumes all five\n"
"       words even for a ring-0 return, so leaving the SS slot holding a\n"
"       leftover stack value makes the selector load #GP.  SS comes from\n"
"       the frame (0x10 kernel / 0x23 user).  RSP comes from gs:[24]:\n"
"       restore_context() just ran arch_set_user_sp(p->context[33]), so\n"
"       this is the resumed process's OWN saved RSP (a user-mode trap's\n"
"       frame RSP, a fresh process's entry stack) — NOT another\n"
"       process's; gs:[24] is only stale if read before a restore.\n"
"       tf->lr (context[30]) is NOT a safe source here: fresh processes\n"
"       never set it, so resuming from lr hands the process RSP=0. */\n"
"    mov rax, [rsp + 288]\n"
"    mov [rsp + 312], rax\n"
"    mov rax, gs:[24]\n"
"    mov [rsp + 304], rax\n"
"    /* 3. RFLAGS -> [rsp + 296] */\n"
"    mov rax, [rsp + 256]\n"
"    mov [rsp + 296], rax\n"
"    /* 4. CS -> [rsp + 288] */\n"
"    mov rax, [rsp + 280]\n"
"    mov [rsp + 288], rax\n"
"    /* 5. RIP -> [rsp + 280] */\n"
"    mov rax, [rsp + 248]\n"
"    mov [rsp + 280], rax\n"
"    /* Restore the original rax */\n"
"    mov rax, [rsp]\n"
"    add rsp, 264\n"
"    add rsp, 16\n"
"    iretq\n"
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
  // P2.2 (S2): TLB shootdown IPI on vector 0x82 (kernel-only gate, 0x8E).
  idt_set_gate(130, (uint64_t)exception_130, 0x08, 0x8E);
}
