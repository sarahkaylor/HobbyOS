#include "trap.h"

#include "fs.h"
#include "net.h"
#include "process.h"
#include "setjmp.h"
#include "timer.h"
#include "lock.h"
#include "arch/cpu.h"
#include "arch/timer.h"
#include "syscall.h"
#include "errno.h"
#include "vfs.h"
#include "vm.h"
#include <stdint.h>

extern jmp_buf user_exit_context;

// External functions we need
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

/* Defined below (line ~330); declared here for the early syscall helpers
   that are v2-aware since P2.2 S2. */
static int sys_user_range_ok(uint64_t ptr, uint64_t len);

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

#define TIMER_PPI_INTID 30

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[0];
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
  const char *filename = (const char *)tf->regs[0];
  int flags = (int)tf->regs[1];
  struct process *caller = current_process();
  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    /* file_open returns >= 0 (fd) or a negative errno it picked itself
       (ENOENT/EEXIST/EMFILE), which errno_ret decodes in libc. */
    tf->regs[0] = file_open(caller, filename, flags);
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_close(struct trap_frame *tf) {
  int fd = (int)tf->regs[0];
  struct process *caller = current_process();
  int r = file_close(caller, fd);
  tf->regs[0] = r < 0 ? -EBADF : r;
}

static void sys_dup(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_dup(caller, (int)tf->regs[0]);
}

static void sys_dup2(struct trap_frame *tf) {
  struct process *caller = current_process();
  int r = file_dup2(caller, (int)tf->regs[0], (int)tf->regs[1]);
  tf->regs[0] = (uint64_t)(r < 0 ? -EBADF : r);
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
  char *buf = (char *)tf->regs[0];
  int size = (int)tf->regs[1];
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  if (cur && buf && sys_user_range_ok((uint64_t)buf, (uint64_t)size)) {
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
  int cmd = (int)tf->regs[0];
  void *buf = (void *)tf->regs[1];
  int size = (int)tf->regs[2];

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
    } else if (cmd == 6) { // Wall-clock time (PL031 RTC)
      if (size >= (int)sizeof(struct sys_time)) {
        extern uint64_t rtc_read_epoch(void);
        extern void rtc_epoch_to_time(uint64_t epoch, int *year, int *month,
                                      int *day, int *hour, int *minute,
                                      int *second, int *weekday);
        struct sys_time *info = (struct sys_time *)buf;
        uint64_t epoch = rtc_read_epoch();
        if (epoch == 0) {
          tf->regs[0] = -1; // no RTC / not yet set
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
  const char *filename = (const char *)tf->regs[0];
  if ((uint64_t)filename >= USER_VIRT_BASE &&
      (uint64_t)filename < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    extern int vfs_unlink(const char *path);
    int r = vfs_unlink(filename);
    tf->regs[0] = r < 0 ? -ENOENT : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_rename(struct trap_frame *tf) {
  const char *oldname = (const char *)tf->regs[0];
  const char *newname = (const char *)tf->regs[1];
  if ((uint64_t)oldname >= USER_VIRT_BASE &&
      (uint64_t)oldname < (USER_VIRT_BASE + USER_REGION_SIZE) &&
      (uint64_t)newname >= USER_VIRT_BASE &&
      (uint64_t)newname < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    extern int vfs_rename(const char *oldp, const char *newp);
    int r = vfs_rename(oldname, newname);
    tf->regs[0] = r < 0 ? -ENOENT : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_connect(struct trap_frame *tf) {
  uint32_t ip = (uint32_t)tf->regs[0];
  uint16_t port = (uint16_t)tf->regs[1];
  int protocol = (int)tf->regs[2];
  struct process *caller = current_process();

  extern int file_connect(struct process *caller, uint32_t ip, uint16_t port, int protocol);
  int r = file_connect(caller, ip, port, protocol);
  tf->regs[0] = r < 0 ? -EIO : r;
}

/* ---- Phase F1 (browser.md A.1a, frozen): sockets + select (65-71) ------
 * Thin argument/pointer layers over the fs.c engine; the select restart
 * follows the pipe_read -2 convention (rewind ELR over the svc, schedule
 * away; the syscall re-executes on wake). */

/* True when [ptr, ptr+len) lies inside the caller's user region. */
static int sys_user_range_ok(uint64_t ptr, uint64_t len) {
  struct process *p = current_process();
  if (p && p->as) {
    /* P2.2 (S2): a v2 process answers to the address-space walk instead
       of the legacy 32 MiB range.  Residency-only until S3's vm_touch
       (no demand paging yet). */
    return vm_range_ok(p, ptr, len, 0) == 0;
  }
  if (ptr < USER_VIRT_BASE) return 0;
  if (len > USER_REGION_SIZE) return 0;
  return ptr - USER_VIRT_BASE <= USER_REGION_SIZE - len;
}

static void sys_socket(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_socket(caller, (int)tf->regs[0],
                                      (int)tf->regs[1], (int)tf->regs[2]);
}

static void sys_connect_fd(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_socket_connect(caller, (int)tf->regs[0],
                                              (uint32_t)tf->regs[1],
                                              (uint16_t)tf->regs[2]);
}

static void sys_fcntl(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_fcntl(caller, (int)tf->regs[0],
                                     (int)tf->regs[1], (int)tf->regs[2]);
}

static void sys_select(struct trap_frame *tf) {
  int nfds = (int)tf->regs[0];
  struct fd_set_k *rd = (struct fd_set_k *)tf->regs[1];
  struct fd_set_k *wr = (struct fd_set_k *)tf->regs[2];
  struct fd_set_k *ex = (struct fd_set_k *)tf->regs[3];
  int timeout_ms = (int)tf->regs[4];
  struct process *caller = current_process();

  if ((rd && !sys_user_range_ok((uint64_t)rd, sizeof(struct fd_set_k))) ||
      (wr && !sys_user_range_ok((uint64_t)wr, sizeof(struct fd_set_k))) ||
      (ex && !sys_user_range_ok((uint64_t)ex, sizeof(struct fd_set_k)))) {
    tf->regs[0] = -EFAULT;
    return;
  }

  int r = file_select(caller, nfds, rd, wr, ex, timeout_ms);
  if (r == -2) {
    tf->elr -= 4; // Restart the syscall when the slice expires
    schedule(tf, 0);
  } else {
    tf->regs[0] = (uint64_t)r;
  }
}

/* --- P1 (browser.md A.1b): threads, futex-lite, TLS (72-75) -------------
 * One else-if per syscall in the dispatch chain above (never a table).
 * Args: x0..x3 = regs[0..3]. */

/* SYS_THREAD_CREATE (72): (entry, arg, stack, flags) -> tid | -errno. */
static void sys_thread_create(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_thread_create(
      caller, tf->regs[0], tf->regs[1], tf->regs[2], tf->regs[3]);
}

/* SYS_FUTEX (73): (uaddr, op, val, timeout_ms) -> 0/count | -errno.  A WAIT
 * park never returns here: the resume re-enters user space with
 * context[0]. */
static void sys_futex(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_futex(
      caller, tf, tf->regs[0], (int)tf->regs[1], (int64_t)tf->regs[2],
      (int64_t)tf->regs[3]);
}

/* SYS_THREAD_EXIT (74): noreturn -- exits only the calling thread. */
static void sys_thread_exit(struct trap_frame *tf) {
  process_thread_exit(tf, tf->regs[0]);
}

/* SYS_SET_TLS (75): (tls) -> 0 | -EINVAL. */
static void sys_set_tls(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)(int64_t)process_set_tls(caller, tf->regs[0]);
}

static void sys_getsockopt(struct trap_frame *tf) {
  void *val = (void *)tf->regs[3];
  int *len = (int *)tf->regs[4];
  struct process *caller = current_process();

  if (!val || !len || !sys_user_range_ok((uint64_t)val, sizeof(int)) ||
      !sys_user_range_ok((uint64_t)len, sizeof(int))) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)file_socket_getopt(caller, (int)tf->regs[0],
                                             (int)tf->regs[1],
                                             (int)tf->regs[2], val, len);
}

static void sys_setsockopt(struct trap_frame *tf) {
  const void *val = (const void *)tf->regs[3];
  int len = (int)tf->regs[4];
  struct process *caller = current_process();

  if (len < 0) {
    tf->regs[0] = -EINVAL;
    return;
  }
  if (val && len > 0 && !sys_user_range_ok((uint64_t)val, (uint64_t)len)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)file_socket_setopt(caller, (int)tf->regs[0],
                                             (int)tf->regs[1],
                                             (int)tf->regs[2], val, len);
}

/* SYS_GETRANDOM (F1.4): mixed kernel entropy, see net.c's entropy pool. */
static void sys_getrandom(struct trap_frame *tf) {
  void *buf = (void *)tf->regs[0];
  uint32_t len = (uint32_t)tf->regs[1];
  unsigned int flags = (unsigned int)tf->regs[2];

  if (flags != 0 || len > (1u << 20)) { // GRND_* unsupported / v1 cap
    tf->regs[0] = -EINVAL;
    return;
  }
  if (len == 0) {
    tf->regs[0] = 0;
    return;
  }
  if (!sys_user_range_ok((uint64_t)buf, len)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)net_get_random_bytes(buf, len);
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
    } else if (ret == -1) {
      tf->regs[0] = -EBADF;
    } else if (ret < 0) {
      /* P4/P5 carve-out: a backend's rich negative errno (e.g. -EPIPE
         from a closed pipe peer) passes through; only the legacy -1
         means EBADF. */
      tf->regs[0] = (uint64_t)(int64_t)ret;
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
  int stderr_fd = (int)tf->regs[3];
  const char *args_ptr = (const char *)tf->regs[4];

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
         restores x0 from context[0], so the failure value must be stored
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
      tf->regs[0] = -EMFILE;
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

extern int vfs_read_dir(const char *path, int index, char *name, int ncap, uint8_t *attr, uint32_t *size);
static void sys_read_dir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[0];
  int index = (int)tf->regs[1];

  struct local_dirent {
    char name[32];
    uint8_t attr;
    uint32_t size;
  } __attribute__((packed));

  struct local_dirent *ud = (struct local_dirent *)tf->regs[2];

  if ((uint64_t)path >= USER_VIRT_BASE &&
      (uint64_t)path < (USER_VIRT_BASE + USER_REGION_SIZE) &&
      (uint64_t)ud >= USER_VIRT_BASE &&
      (uint64_t)ud + sizeof(struct local_dirent) <= (USER_VIRT_BASE + USER_REGION_SIZE)) {

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
  const char *path = (const char *)tf->regs[0];
  struct process *caller = current_process();
  if ((uint64_t)path >= USER_VIRT_BASE &&
      (uint64_t)path < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    extern int file_mkdir(struct process *cur, const char *path);
    int r = file_mkdir(caller, path);
    tf->regs[0] = r < 0 ? -EEXIST : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* mount(source, target): mount an NFS export ("A.B.C.D:/export") at a
 * directory of the FAT volume.  The mount point is created when missing. */

/* Copy a NUL-terminated user string into kernel memory with full bounds
 * checking. Returns 1 on success (dst NUL-terminated), 0 on bad ptr. */
static int u_strcpy(const char *src, char *dst, int cap) {
  uint64_t base = (uint64_t)src;
  if (!src || base < USER_VIRT_BASE ||
      base >= USER_VIRT_BASE + USER_REGION_SIZE)
    return 0;
  if (base + cap - 1 >= USER_VIRT_BASE + USER_REGION_SIZE)
    return 0;
  int i = 0;
  for (; i < cap - 1 && src[i]; i++)
    dst[i] = src[i];
  dst[i] = '\0';
  return 1;
}

static void sys_getpid(struct trap_frame *tf) {
  struct process *cur = current_process();
  tf->regs[0] = cur ? (uint64_t)cur->pid : (uint64_t)-1;
}

static void sys_getppid(struct trap_frame *tf) {
  struct process *cur = current_process();
  tf->regs[0] = cur ? (uint64_t)cur->parent_pid : (uint64_t)-1;
}

static void sys_waitpid(struct trap_frame *tf) {
  tf->regs[0] = process_waitpid(tf);
}

static void sys_exec(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[0];
  char *const *argv = (char *const *)tf->regs[1];
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

  /* argv[0] names the new program (POSIX: may differ from the path). */
  char namebuf[32];
  int named = 0;
  if (argv && u_strcpy((const char *)argv[0], namebuf, sizeof namebuf)
      && namebuf[0])
    named = 1;
  if (!named) {
    /* fall back to the basename of the path */
    const char *b = pathbuf;
    for (int i = 0; pathbuf[i]; i++)
      if (pathbuf[i] == '/')
        b = &pathbuf[i + 1];
    int i = 0;
    for (; b[i] && i < 31; i++)
      namebuf[i] = b[i];
    namebuf[i] = '\0';
  }

  /* argv[1..] become the args string (space-joined, like spawn). */
  char argbuf[256];
  int alen = 0;
  argbuf[0] = '\0';
  if (argv) {
    for (int ai = 1; argv[ai] != 0 && alen < 251; ai++) {
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

  /* P5 (D3-order): argv[] elements live in the OLD image -- marshal them
     into kernel memory before the load clobbers it. */
  char argvblob[HO_EXEC_ARGV_LEN];
  int argvc = 0;
  proc_marshal_argv((char *const *)argv, argvblob, HO_EXEC_ARGV_LEN, &argvc);

  /* P5 (D3.1/D3.5): marshal envp (regs[2]) into the kernel env blob; a bad
     pointer anywhere is -EFAULT (argv keeps its truncating behavior). */
  char envbuf[HO_ENV_LEN];
  for (int i = 0; i < HO_ENV_LEN; i++)
    envbuf[i] = 0;
  int envc = 0;
  if (process_read_envp(cur, (const char *const *)tf->regs[2], envbuf,
                        HO_ENV_LEN, &envc) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }

  /* Success redirects elr + sets regs[0]=0 (the new program never reads
     the exec return); the argv/env blobs were installed kernel-side. */
  int r = process_exec_current(tf, pathbuf, argbuf, namebuf, envbuf, envc,
                               argvblob, argvc);
  if (r < 0)
    tf->regs[0] = (uint64_t)r;
}

/* --- P5 (docs/browser/p5-exec-signals-design.md): rows 83-85 ---------- */

/* Byte-wise marshal helpers for the sysroot's struct sigaction (P5 OQ4
 * layout).  Never a whole-struct cast: the kernel parses binary layouts
 * byte-by-byte (house rule), so a misaligned user pointer can never fault
 * the kernel with a wide load. */
static uint32_t p5_ld32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t p5_ld64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= ((uint64_t)p[i]) << (8 * i);
  return v;
}
static void p5_st32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
static void p5_st64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

/* SYS_SIGACTION (83): marshal the user struct field-wise; process.c
 * validates + stores (D7). */
#define P5_SIGACTION_SIZE 40
static void sys_sigaction(struct trap_frame *tf) {
  int signum = (int)tf->regs[0];
  const uint8_t *uact = (const uint8_t *)tf->regs[1];
  uint8_t *uold = (uint8_t *)tf->regs[2];
  struct process *cur = current_process();
  if (!cur) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }
  struct ho_sigaction actk, oldk;
  const struct ho_sigaction *actp = 0;
  struct ho_sigaction *oldp = 0;
  if (uact) {
    if (process_user_ok(cur, (uint64_t)uact, P5_SIGACTION_SIZE, 0) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return;
    }
    actk.sa_handler = p5_ld64(uact + 0);
    actk.sa_mask[0] = p5_ld64(uact + 8);
    actk.sa_mask[1] = p5_ld64(uact + 16);
    actk.sa_flags = (int)p5_ld32(uact + 24);
    actk.sa_restorer = p5_ld64(uact + 32);
    actp = &actk;
  }
  if (uold) {
    if (process_user_ok(cur, (uint64_t)uold, P5_SIGACTION_SIZE, 1) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return;
    }
    oldp = &oldk;
  }
  int r = process_sigaction(signum, actp, oldp);
  if (r == 0 && oldp) {
    p5_st64(uold + 0, oldk.sa_handler);
    p5_st64(uold + 8, oldk.sa_mask[0]);
    p5_st64(uold + 16, oldk.sa_mask[1]);
    p5_st32(uold + 24, (uint32_t)oldk.sa_flags);
    p5_st64(uold + 32, oldk.sa_restorer);
  }
  tf->regs[0] = (uint64_t)(int64_t)r;
}

/* SYS_GETENV (85): mirror of SYS_GETARGV's marshalling; buf validated for
 * write when the call will copy into it. */
static void sys_getenv(struct trap_frame *tf) {
  struct process *cur = current_process();
  int idx = (int)tf->regs[0];
  char *buf = (char *)tf->regs[1];
  int size = (int)tf->regs[2];
  if (idx >= 0 && size > 0) {
    if (!cur || process_user_ok(cur, (uint64_t)buf, (uint64_t)size, 1) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return;
    }
  }
  tf->regs[0] = (uint64_t)(int64_t)sys_readenv(cur, idx, buf, size);
}

static void sys_mount(struct trap_frame *tf) {
  const char *source = (const char *)tf->regs[0];
  const char *target = (const char *)tf->regs[1];
  if ((uint64_t)source >= USER_VIRT_BASE &&
      (uint64_t)source < (USER_VIRT_BASE + USER_REGION_SIZE) &&
      (uint64_t)target >= USER_VIRT_BASE &&
      (uint64_t)target < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    extern int vfs_mount(const char *source, const char *target);
    int r = vfs_mount(source, target);
    tf->regs[0] = r < 0 ? -EINVAL : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* umount(target): unmount the NFS export mounted exactly at `target`. */
static void sys_umount(struct trap_frame *tf) {
  const char *target = (const char *)tf->regs[0];
  if ((uint64_t)target >= USER_VIRT_BASE &&
      (uint64_t)target < (USER_VIRT_BASE + USER_REGION_SIZE)) {
    extern int vfs_umount(const char *target);
    int r = vfs_umount(target);
    tf->regs[0] = r < 0 ? -EINVAL : r;
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_getcwd(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[0];
  int size = (int)tf->regs[1];
  struct process *caller = process_group(current_process()); /* P1 (D7) */
  if (caller && (uint64_t)buf >= USER_VIRT_BASE &&
      (uint64_t)buf + size <= (USER_VIRT_BASE + USER_REGION_SIZE)) {
    int len = 0;
    while (caller->cwd[len]) len++;
    if (len + 1 > size) {
      tf->regs[0] = -ERANGE;   /* buffer too small */
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
  int fd = (int)tf->regs[0];
  int64_t offset = (int64_t)tf->regs[1];
  int whence = (int)tf->regs[2];
  int err = 0;
  extern int64_t file_seek(struct process *p, int fd, int64_t offset,
                           int whence, int *errp);
  int64_t r = file_seek(caller, fd, offset, whence, &err);
  tf->regs[0] = r < 0 ? (uint64_t)(-err) : (uint64_t)r;
}

static void sys_stat(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[0];
  struct k_stat *st = (struct k_stat *)tf->regs[1];
  if ((uint64_t)path >= USER_VIRT_BASE &&
      (uint64_t)path < (USER_VIRT_BASE + USER_REGION_SIZE) &&
      (uint64_t)st >= USER_VIRT_BASE &&
      (uint64_t)st + sizeof(struct k_stat) <=
          (USER_VIRT_BASE + USER_REGION_SIZE)) {
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
  int fd = (int)tf->regs[0];
  struct k_stat *st = (struct k_stat *)tf->regs[1];
  if ((uint64_t)st >= USER_VIRT_BASE &&
      (uint64_t)st + sizeof(struct k_stat) <=
          (USER_VIRT_BASE + USER_REGION_SIZE)) {
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
  const char *path = (const char *)tf->regs[0];
  struct process *caller = process_group(current_process()); /* P1 (D7) */
  if (caller && (uint64_t)path >= USER_VIRT_BASE &&
      (uint64_t)path < (USER_VIRT_BASE + USER_REGION_SIZE)) {
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
  int pid = (int)tf->regs[0];
  int sig = (int)tf->regs[1];
  if (sig == 0) {
    struct process *p = process_get_pcb(pid);
    if (p && p->state != PROC_STATE_FREE && p->state != PROC_STATE_EXITED) {
      tf->regs[0] = 0;
    } else {
      tf->regs[0] = -ESRCH;
    }
  } else {
    /* P5 (D9): signal semantics — dispositions decide; default kills. */
    int r = process_signal(pid, sig);
    tf->regs[0] = (uint64_t)(int64_t)r;
    /* If the default action just killed the CALLER's own group, do not
       return to a released image: schedule away now (never returns). */
    struct process *cur = current_process();
    if (cur && process_group(cur)->state == PROC_STATE_EXITED)
      schedule(tf, 0);
  }
}

/* SYS_GETPROGNAME: copy the process's binary name into the user buffer.
 * Used by crt0 to build a correct argv[0] for main() programs. Native
 * extension: 0 on success, -EFAULT on a bad pointer, -1 if no process. */
static void sys_get_progname(struct trap_frame *tf) {
  char *buf = (char *)tf->regs[0];
  int size = (int)tf->regs[1];
  struct process *caller = current_process();
  if (!caller) {
    tf->regs[0] = -1;
  } else if (buf && sys_user_range_ok((uint64_t)buf, (uint64_t)size)) {
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

  // Serialize multi-core fault dumps so one CPU at a time prints a clean
  // line (otherwise 4 CPUs interleave the FATAL text char-by-char).
  static spinlock_t fatal_lock = {0};
  uint64_t flags = spinlock_acquire_irqsave(&fatal_lock);
  uart_puts("\n[KERNEL] FATAL: CPU ");
  uart_print_hex(get_cpuid());
  uart_puts(" Synchronous Exception in EL1! ESR: ");
  uart_print_hex(esr);
  uart_puts(" EC: ");
  uart_print_hex(ec);
  uart_puts(" ISS: ");
  uart_print_hex(iss);
  uint64_t far;
  __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  uart_puts(", FAR: ");
  uart_print_hex(far);
  uart_puts(", ELR: ");
  uart_print_hex(tf->elr);
  {
    struct process *fp = current_process();
    if (fp) {
      uart_puts(", PROC pid=");
      print_int(fp->pid);
      uart_puts(" name=");
      uart_puts(fp->name);
    } else {
      uart_puts(", PROC= (none)");
    }
    /* EL1 SP from the trap frame's saved SP */
    uart_puts(", SPSR: ");
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(esr) : :); // reuse esr as scratch
    uart_print_hex(esr);
    uart_puts(", LR: ");
    uart_print_hex(tf ? tf->lr : 0);
    uart_puts(", TFP: ");
    uart_print_hex((uint64_t)tf);
    /* Dump 4 words at ELR to see whether the fetched bytes match the ELF */
    uart_puts("\n[KERNEL] bytes@ELR:");
    if ((uint64_t)tf->elr >= 0x40000000ULL) {
      volatile uint32_t *wp = (volatile uint32_t *)tf->elr;
      for (int i = 0; i < 16; i++) {
        uart_puts(" ");
        uart_print_hex(wp[i]);
      }
      /* Dump the live L2 descriptors around the fault address + the user block */
      extern uint64_t l2_table_1[][512];
      uint32_t l2idx = ((uint64_t)tf->elr >> 21) & 0x1FF;
      uint32_t cp = (uint32_t)get_cpuid();
      uart_puts("\n[KERNEL] L2[");
      print_int(cp);
      uart_puts("][0x200] (kernel text block)=");
      uart_print_hex(l2_table_1[cp][0x200]);
      uart_puts("  L2[0x220]=");
      uart_print_hex(l2_table_1[cp][0x220]);
      uart_puts("\n[KERNEL] &sys_write=");
      uart_print_hex((uint64_t)&sys_write);
      uart_puts(" &sys_write_console=");
      uart_print_hex((uint64_t)&sys_write_console);
      uart_puts(" &uart_puts=");
      uart_print_hex((uint64_t)&uart_puts);
      /* Compare bytes at uart_puts (known-good, very early) vs ELR */
      uint32_t *up = (uint32_t *)&uart_puts;
      uart_puts("\n[KERNEL] bytes@uart_puts:");
      for (int i = 0; i < 4; i++) {
        uart_puts(" ");
        uart_print_hex(up[i]);
      }
      /* elr really in kernel text? dump L2 block phys of fault addr properly */
      uint64_t fa = (uint64_t)tf->elr;
      uint32_t fal2 = (uint32_t)(fa >> 21);
      uart_puts("\n[KERNEL] L2[");
      print_int((int)fal2);
      uart_puts("]=");
      uart_print_hex(l2_table_1[cp][fal2]);
    }
    uart_puts("\n");
  }
  spinlock_release_irqrestore(&fatal_lock, flags);
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

/* --- Syscall dispatch ------------------------------------------------
 * Keyed off the shared SYS_* constants from syscall.h (single source of
 * truth — the numbers can never drift between libc.c and the trap
 * handlers).  Deliberately an if/else chain, NOT a table of function
 * pointers: the UEFI bootloader loads the kernel at a base that differs
 * from the link-time 0x4008.... address (AArch64 code is naturally
 * position-independent, so this normally doesn't matter), but a table
 * stores ABSOLUTE link-time addresses and calling them jumps to physical
 * RAM that was never loaded — executing zeroes (ESR EC=0) on the first
 * syscall.  Any added arm/x64 syscall must append an else-if here. */

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
      struct process *gp = current_process();
      tf->regs[0] = sys_readargv(gp, (int)tf->regs[0], (char *)tf->regs[1],
                                 (int)tf->regs[2]);
    } else if (syscall_num == SYS_BRK) {
      tf->regs[0] = sys_brk(tf->regs[0]);
    } else if (syscall_num == SYS_MMAP) {
      /* P2.3 (S3): 6-arg Linux shape, ARM x0..x5 = regs[0..5]. */
      tf->regs[0] = sys_mmap6(tf->regs[0], tf->regs[1], (int64_t)tf->regs[2],
                              (int64_t)tf->regs[3], (int64_t)tf->regs[4],
                              tf->regs[5]);
    } else if (syscall_num == SYS_MUNMAP) {
      tf->regs[0] = sys_munmap(tf->regs[0], tf->regs[1]);
    } else if (syscall_num == SYS_MPROTECT) {
      tf->regs[0] = sys_mprotect(tf->regs[0], tf->regs[1], (int64_t)tf->regs[2]);
    } else if (syscall_num == SYS_MADVISE) {
      tf->regs[0] = sys_madvise(tf->regs[0], tf->regs[1], (int64_t)tf->regs[2]);
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
    } else if (syscall_num == SYS_SIGACTION) {
      sys_sigaction(tf);
    } else if (syscall_num == SYS_SIGRETURN) {
      tf->regs[0] = (uint64_t)(int64_t)process_sigreturn(tf);
    } else if (syscall_num == SYS_GETENV) {
      sys_getenv(tf);
    } else {
      uart_puts("Unknown System Call Invoked!\n");
      tf->regs[0] = -ENOSYS;
    }
  } else if (ec == 0x20 || ec == 0x24) {
    // EC = 0x20: Instruction Abort from a lower Exception Level
    // EC = 0x24: Data Abort from a lower Exception Level
    struct process *gcur = current_process();
    uint64_t far = 0;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    int wnr = (int)((iss >> 6) & 1);   // WnR (data aborts)
    uint32_t dfsc = iss & 0x3F;        // fault status
    int is_exec = (ec == 0x20);

    /* P2.3 (design section 5.1): v2 processes get demand-paging
       classification: translation faults (DFSC 0b0001xx) demand a
       zero-fill page; everything else is a kill with the exact reason. */
    if (gcur && gcur->as) {
      const char *why = "PROT";
      int demand = ((dfsc & 0x3C) == 0x04);
      if (demand &&
          vm_handle_fault(process_group(gcur), far, wnr, is_exec, &why) == 0) {
        return; /* mapping is live; eret re-executes the instruction */
      }
      uart_puts("[KERNEL] pid=");
      print_int(gcur->pid);
      uart_puts(" (");
      uart_puts(gcur->name);
      uart_puts(") memory fault VA=");
      uart_print_hex(far);
      uart_puts(" PC=");
      uart_print_hex(tf->elr);
      uart_puts(" rw=");
      uart_puts(wnr ? "w" : "r");
      uart_puts(is_exec ? "x" : " ");
      uart_puts(" in=");
      uart_puts(why);
      uart_puts(" -> killed\n");
      process_fault_exit(tf, 11); /* SIGSEGV status byte */
      return;
    }

    // v1 (or no process): the pre-P2 fatal path, verbatim.
    if (gcur) {
      uart_puts("[KERNEL] User process ");
      print_int(gcur->pid);
      if (gcur->name[0] != '\0') {
        uart_puts(" (");
        uart_puts(gcur->name);
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
  } else if (ec == 0x00) {
    // EC = 0x00: Unknown Reason (e.g. executing zeroes) -- no FAR/ISS
    // classification; keep the pre-P2 fatal path.
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
