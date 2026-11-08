#include "trap.h"
#include "kline.h"
#include "fs.h"
#include "net.h"
#include "process.h"
#include "vdisplay.h"
#include "display_mode.h"
#include "setjmp.h"
#include "timer.h"
#include "lock.h"
#include "arch/cpu.h"
#include "arch/mmu.h"
#include "syscall.h"

/* GB-027: /dev/input/event* ioctl (EVIOCGVERSION/GNAME/GBIT/GABS) + the
 * Linux-ABI event read that translates the virtio_input ring into
 * 24-byte input_event records.  ABI numbers from src/include/linux_input.h
 * / docs/linux-abi.md §9 (not from memory). */
#include "input_node.h"
#include "virtio_input.h"   /* struct virtio_input_event: the ring's record */

/* WD lane: x64 flush-path probes (serial "[WD] ..."); default 0 keeps the
 * shipped kernel quiet.  See docs/wd-x64-white-blit.md. */
#ifndef WD_X64_FLUSH_PROBE
#define WD_X64_FLUSH_PROBE 0
#endif
/* GB-057: the sys_fork / sys_close per-syscall debug prints fired on EVERY
 * call and flooded the emulated UART (~29% of the intel T3 console: the
 * GB-009 baseline carried 5077+5524 sys_close lines and 85 sys_fork lines).
 * That PIO write storm slowed the wave and interleaved real FAIL lines so
 * they could not be attributed — the interleaving class that hid the
 * GB-057 gate defect.  Off by default; turn on only when tracing the syscall
 * path by hand. */
#ifndef TRAP_SYSCALL_DEBUG
#define TRAP_SYSCALL_DEBUG 0
#endif
#include "errno.h"
#include "linux_uname.h"   /* GB-016: linux_fill_utsname + linux_utsname_t */
#include "linux_compat.h"  /* compat handler-table dispatch + [LCOMPAT] tracer */
#include "vfs.h"
#include "vm.h"
#include "frame.h"
#include "rw_watch.h"
#include "blk_cache.h"
#include "fb_mmap.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);
// L8 ABI ext (row 87): boot-anchored REALTIME clock (src/kernel/time.c).
extern uint64_t hb_clock_realtime_ms(void);

/* GB-097: the console's LOCK-FREE, NON-PANIC character sink.  The watchdog
   dump below is a RETURNING diagnostic (it prints, then the tick continues),
   so it must not use panic_puts/panic_print_hex: upstream's GB-097 rename
   reserves those for non-returning paths and reports a
   [PANIC-SINK-VIOLATION] for any other caller.  uart_putc_dbg is the sink the
   rename left for exactly this case; fragments are emitted one character at
   a time so a wedged console lock can never deadlock the reporter. */
extern void uart_putc_dbg(char c);

static void wd_puts(const char *s) {
  while (s && *s) uart_putc_dbg(*s++);
}

static void wd_hex(uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  wd_puts("0x");
  for (int sh = 60; sh >= 0; sh -= 4) uart_putc_dbg(hx[(v >> sh) & 0xF]);
}

static void wd_int(int v) {
  if (v < 0) { uart_putc_dbg('-'); v = -v; }
  char b[12]; int n = 0;
  if (v == 0) { uart_putc_dbg('0'); return; }
  while (v > 0) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n > 0) uart_putc_dbg(b[--n]);
}

/* Defined below (line ~380); declared here for the early syscall helpers
   that are v2-aware since P2.2 S2.  P4: non-static — fs.c range-checks
   msghdr/iovec/cmsg blocks through the same arch helper. */
static int sys_user_range(uint64_t ptr, uint64_t len, int write);
int sys_user_range_ok(uint64_t ptr, uint64_t len);
static int copy_user_path(const char *user, char *kbuf, int cap);

/* GB-152: struct cpu_local is declared once in process.h (the asm's fixed
   gs: offsets are pinned by the typedefs below). */

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

/* GB-057: called from common_trap_exit immediately before the iretq, with a
   pointer to the hardware frame the iretq is about to consume ([0] = RIP,
   [1] = CS).

   Why this site: probe26 showed the corrupt frame does NOT come from the
   scheduler.  The corrected [BADELR] check reads context[31] at the pick and
   stayed silent through three faults, two of them at rip=0x7 -- a value it tests
   for.  So the bad RIP never passed through context[]: it is already in the stack
   frame that common_trap_exit iretqs out of.  The scheduler resume sites carry an
   IF=0 discipline against a clobbered frame window; this path has no equivalent,
   which is the same bug at a site nobody armed.

   The CS discriminator is load-bearing, not decoration: a kernel-mode trap return
   has a perfectly legal kernel RIP, so a rule that only looked at the address
   would flag every kernel iretq and spend its whole budget on false positives --
   which is exactly the failure of my first [BADELR] rule (probe25).  Only
   RPL==3 returns are user returns, and only those are judged. */
void trap_exit_frame_check(uint64_t *frame) {
  extern volatile uint64_t trap_exit_bad_rip_seen;
  extern void panic_puts(const char *s);
  extern void panic_print_hex(uint64_t v);
  uint64_t rip = frame[0];
  uint64_t cs = frame[1];
  int implausible;

  if ((cs & 3u) != 3u) {
    /* GB-061: a kernel-mode return can have a perfectly legal kernel RIP --
       that is why the CS discriminator exists -- but it can never be below the
       NULL page.  Kernel text lives above the kernel base, so `rip < 0x1000`
       is poison for BOTH privilege levels.  Excluding kernel returns here (and
       at the pick site) is exactly why the observed
       `vector=13 rip=0x0 pid=1` halt went unreported while [LOSTWAKE] disposed
       the same slot afterwards.  Judge kernel returns on the LOW bound only;
       the upper range check stays user-only, because 0x70000000..USER_VA_BASE
       is a legitimate kernel text window. */
    if (rip >= 0x1000u)
      return;
  } else {
    implausible = (rip < 0x1000u) || (rip >= 0x70000000u && rip < USER_VA_BASE);
    if (!implausible)
      return;
  }
  if (trap_exit_bad_rip_seen >= 20)
    return;
  trap_exit_bad_rip_seen++;
  panic_puts("[BADIRET] about to return to an implausible user RIP=");
  panic_print_hex(rip);
  panic_puts(" cs=");
  panic_print_hex(cs);
  panic_puts(" rsp=");
  panic_print_hex(frame[3]);
  panic_puts(" eflags=");
  panic_print_hex(frame[2]);
  panic_puts(" cpu=");
  panic_print_hex((uint64_t)get_cpuid());
  panic_puts("\n");
}

/* GB-057: bounded count for the pre-iretq frame report (a rare event, not a
   per-trap log -- a diagnostic that reports on every trap exit would be a
   per-trap cost and a per-trap flood). */
volatile uint64_t trap_exit_bad_rip_seen;

void restore_user_sp_helper(void) {
  uint32_t cpu = get_cpuid();
  uint64_t v = arch_get_user_sp();
  cpu_locals[cpu].user_sp_temp = v;
}

static void sys_write_console(struct trap_frame *tf) {
  uint64_t ptr = tf->regs[5]; /* rdi */
  int len = (int)tf->regs[4]; /* rsi — WK-1 M3: honor the length */
  struct process *p = current_process();
  /* SEC-2 (security-plan.md #2): the legacy no-context branch range-checked
     the raw user pointer and then read it byte-by-byte in kernel mode,
     assuming everything inside the window was a valid mapped buffer.  With
     an address space present, console_write_user now validates AND copies
     atomically per page (vm_user_read); without one there is nothing to
     validate against, so reject instead of guessing. */
  tf->regs[0] =
      (uint64_t)(p ? console_write_user(p, (const void *)ptr, len) : -EFAULT);
}

/* SYS_EXIT: the shared process_exit() reads the exit code from the
 * CANONICAL regs[0] slot (on ARM x0 carries the code — the syscall number
 * rides in x8).  On x64 the number rides in rax == regs[0] and the code in
 * rdi, so without this remap EVERY intel process exit is captured as the
 * syscall number: SYS_EXIT == 2, i.e. (2 << 8) == 512, so every parent on
 * intel sees WEXITSTATUS == 2 whatever the child returned.  That single
 * missing remap is what turned the whole intel T3 tier red in the
 * output-comparing suites ("status) got 2, want 0" everywhere) — GB-057.
 * Remap exactly the way sys_waitpid/sys_fork do. */
static void sys_exit(struct trap_frame *tf) {
  struct process *cur = current_process();
  /* GB-021: a musl pthread exits with exit(60), not SYS_THREAD_EXIT(74).
     A clone(2) child must leave only its thread; the group stays alive.
     GB-216: that carve-out is for the LINUX-ABI clone(2) children only
     (abi_flags carries PROC_FLAG_LINUX_ABI from the anchor).  A native
     HobbyOS thread calling exit() must terminate the whole group, exactly
     as process_exit's contract says ("SYS_EXIT keeps exit_group meaning
     from ANY thread") -- and as ARM's SYS_EXIT already does.  Before this,
     THRD_T's exit(7)-from-a-secondary-thread left the group alive and the
     parent's waitpid() never returned. */
  if (cur && cur->is_thread && (cur->abi_flags & PROC_FLAG_LINUX_ABI)) {
    process_thread_exit(tf, tf->regs[5]);
    return;
  }
  tf->regs[0] = tf->regs[5]; /* rdi = the exit code */
  process_exit(tf);
}

static void sys_fork(struct trap_frame *tf) {
  int pid = process_fork(tf);
  tf->regs[0] = pid < 0 ? (uint64_t)-EAGAIN : (uint64_t)pid;
#if TRAP_SYSCALL_DEBUG
  /* GB-082: ONE uart_puts for the whole record.  This was three calls
     ("... is now ", the number, "\n"), so a peer CPU's uart_puts could land
     between them and fuse two records on one line -- measured as the last two
     two-record lines in the ARM tier after the userland buffering landed:
       [KERNEL] sys_fork: tf->regs[0] is now [WAVE-TRY] TAC.BIN
     uart_puts holds uart_lock for the whole string, so building the line first
     and emitting once is the whole fix. */
  {
    char b[64];
    int li = 0;
    const char *pre = "[KERNEL] sys_fork: tf->regs[0] is now ";
    while (*pre && li < 40) b[li++] = *pre++;
    int v = (int)tf->regs[0];
    if (v < 0) { b[li++] = '-'; v = -v; }
    char t[12];
    int ti = 0;
    if (v == 0) t[ti++] = '0';
    while (v > 0 && ti < 11) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
    while (ti > 0 && li < 62) b[li++] = t[--ti];
    b[li++] = '\n';
    b[li] = 0;
    uart_puts(b);
  }
#endif
}

static void sys_open(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; // rdi
  int flags = (int)tf->regs[4];                     // rsi (syscall arg 2)
  struct process *caller = current_process();
  /* L73/SEC-3: copy_user_path reads every byte through vm_user_read, so a
     string that ends on a region boundary answers -EFAULT instead of letting
     the kernel walk into the hole past it. */
  char kpath[VFS_PATH_MAX];
  int plen = copy_user_path(filename, kpath, sizeof kpath);
  if (plen < 0) {
    tf->regs[0] = (uint64_t)(int64_t)plen;
    return;
  }
  /* file_open returns >= 0 (fd) or a negative errno it picked itself
     (ENOENT/EEXIST/EMFILE), which errno_ret decodes in libc. */
  tf->regs[0] = file_open(caller, kpath, flags);
}

/* GB-201: FAT16 cannot name a glibc SONAME (two dots), but ld.so opens its
   dependencies by SONAME.  A compat open whose *basename* is one of the staged
   SONAMEs resolves to the 8.3 name the loader staged.  The table is exact and
   bounded: every other path, including an unknown .so name, keeps the ordinary
   file_open result, so a miss is still ENOENT — never a wildcard. */
static const struct {
  const char *soname;
  const char *staged;
} compat_sonames[] = {
  { "libc.so.6", "LIBC6.SO" },
  { "ld-linux-x86-64.so.2", "LD64.SO" },
};

static int compat_name_eq_ci(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static const char *compat_soname_staged(const char *path) {
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == '/') base = p + 1;
  for (unsigned i = 0; i < sizeof compat_sonames / sizeof compat_sonames[0]; i++)
    if (compat_name_eq_ci(base, compat_sonames[i].soname))
      return compat_sonames[i].staged;
  return 0;
}

static void compat_open(struct trap_frame *tf) {
  const char *filename = (const char *)tf->regs[5]; /* rdi */
  int flags = (int)tf->regs[4];                     /* rsi */
  struct process *caller = current_process();
  char kpath[VFS_PATH_MAX];
  int plen = copy_user_path(filename, kpath, sizeof kpath);
  if (plen < 0) {
    tf->regs[0] = (uint64_t)(int64_t)plen;
    return;
  }
  const char *staged = compat_soname_staged(kpath);
  int fd = file_open(caller, staged ? staged : kpath, flags);
  if (staged) {
    /* One record: the guest's SONAME, the file it resolved to, and the fd —
       the evidence the open path (not a staged rename) did the work. */
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "open: ");
    kl_s(&kl, kpath);
    kl_s(&kl, " -> ");
    kl_s(&kl, staged);
    kl_s(&kl, " fd=");
    kl_i(&kl, fd);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  tf->regs[0] = (uint64_t)(int64_t)fd;
}

static void sys_close(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  struct process *caller = current_process();

#if TRAP_SYSCALL_DEBUG
  uint32_t cpu = get_cpuid();
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
#endif

  int ret = file_close(caller, fd);
#if TRAP_SYSCALL_DEBUG
  uart_puts("[KERNEL Debug] sys_close: fd=");
  print_int(fd);
  uart_puts(", ret=");
  print_int(ret);
  uart_puts("\n");
#endif
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
      /* P4 (docs/browser/p4-ipc-design.md §6.5): -errno (< -1) passes
       * through; -1 keeps the historical EBADF mapping for legacy
       * backends.  -2 is the restart marker handled above. */
      tf->regs[0] = (ret == -1) ? (uint64_t)-EBADF : (uint64_t)ret;
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

/* cmd 10 (L35): NFS READ traffic counters.  Mirrors struct sys_nfsio in
 * src/user_include/libc.h. */
struct sys_nfsio {
  uint64_t bytes;
  uint64_t ops;
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
    } else if (cmd == 9) { // Block cache stats (docs/blk-cache.md)
      if (size >= (int)sizeof(struct blk_cache_stats)) {
        blk_cache_get_stats((struct blk_cache_stats *)buf);
        tf->regs[0] = 0;
      } else {
        tf->regs[0] = -1;
      }
    } else if (cmd == 10) { // NFS READ traffic counters (L35 bench)
      if (size >= (int)sizeof(struct sys_nfsio)) {
        extern volatile uint64_t nfs_io_bytes, nfs_io_ops;
        struct sys_nfsio *io = (struct sys_nfsio *)buf;
        io->bytes = nfs_io_bytes;
        io->ops = nfs_io_ops;
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
  char kpath[VFS_PATH_MAX];
  int plen = copy_user_path(filename, kpath, sizeof kpath);
  if (plen < 0) {
    tf->regs[0] = (uint64_t)(int64_t)plen;
    return;
  }
  extern int vfs_unlink(const char *path);
  int r = vfs_unlink(kpath);
  tf->regs[0] = r < 0 ? -ENOENT : r;
}

static void sys_rename(struct trap_frame *tf) {
  char kold[VFS_PATH_MAX], knew[VFS_PATH_MAX];
  if (!u_strcpy((const char *)tf->regs[5], kold, sizeof kold) ||
      !u_strcpy((const char *)tf->regs[4], knew, sizeof knew)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  extern int vfs_rename(const char *oldp, const char *newp);
  int r = vfs_rename(kold, knew);
  tf->regs[0] = r < 0 ? -ENOENT : r;
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

/* Read-side sugar (in-buffers, strings, structures the kernel only reads).
 * P4: non-static — fs.c range-checks msghdr/iovec/cmsg blocks through this
 * same arch helper (extern in fs.c). */
int sys_user_range_ok(uint64_t ptr, uint64_t len) {
  return sys_user_range(ptr, len, 0);
}

/* GB-178: copy a NUL-terminated user path byte by byte, validating each
   byte's page before the load.  A one-byte guard at the syscall entry is
   not enough: a string whose first byte is mapped and whose last byte is in
   the next (unmapped) page faults at CPL0 inside vfs_clean.  Returns the
   length on success, -EFAULT on an unmapped byte, -ENAMETOOLONG if no NUL
   fits.  Kernel callers (string literals) do not use this helper.

   L73/SEC-3 (docs/l73-kpf-report.md): the original per-byte
   "validate with sys_user_range_ok, then load" was a TOCTOU -- a peer thread
   sharing the address space could unmap the page between the check and the
   load, faulting the kernel-mode read.  vm_user_read validates AND reads each
   byte atomically under vm_lock, so the load can never race.  This is the
   path-shaped variant of u_strcpy(): same guarantee, but it also reports
   -ENAMETOOLONG instead of silently truncating, which is what the VFS
   callers want. */
static int copy_user_path(const char *user, char *kbuf, int cap) {
  if (!user || !kbuf || cap <= 0) return -EINVAL;
  struct process *p = current_process();
  for (int i = 0; i < cap; i++) {
    char c;
    if (vm_user_read(p, (uint64_t)(uintptr_t)(user + i), &c, 1) != 0)
      return -EFAULT;
    kbuf[i] = c;
    if (c == '\0') return i;
  }
  return -ENAMETOOLONG;
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
  int cmd = (int)tf->regs[4];      // rsi
  uint64_t arg = tf->regs[3];      // rdx
  if ((cmd == K_F_GETLK || cmd == K_F_SETLK || cmd == K_F_SETLKW) &&
      ((arg & 7) || !sys_user_range_ok(arg, K_FLOCK_SIZE))) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)file_fcntl(caller, (int)tf->regs[5], cmd, arg);
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

/* --- P4 (docs/browser/p4-ipc-design.md §6): poll/socketpair/sendmsg/
 * recvmsg (76-79).  Args: rdi/rsi/rdx/r10 = regs[5]/[4]/[3]/[9].  -2 parks
 * ride the same rewind+save_context+schedule restart as sys_read. -------- */

/* SYS_POLL (76): (struct pollfd *fds, int nfds, int timeout_ms) -> count. */
static void sys_poll(struct trap_frame *tf) {
  struct k_pollfd *fds = (struct k_pollfd *)tf->regs[5]; // rdi
  int nfds = (int)tf->regs[4]; // rsi
  int timeout_ms = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();

  if (nfds < 0 || nfds > K_FD_SETSIZE) {
    tf->regs[0] = -EINVAL;
    return;
  }
  if (nfds > 0 &&
      (!fds || ((uint64_t)fds & 3) != 0 ||
       !sys_user_range_ok((uint64_t)fds, (uint64_t)nfds * sizeof(struct k_pollfd)))) {
    tf->regs[0] = -EFAULT; /* range + natural (4-byte) alignment, §6.5 */
    return;
  }

  int r = file_poll(caller, fds, nfds, timeout_ms);
  if (r == -2) {
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2; // rewind over the `syscall` instruction
    }
    save_context(caller, tf);
    schedule(tf, 0);
    /* GB-057 diagnostic: reaching here means schedule() returned WITHOUT a
       switch, so the trap handler will fall through with elr still on the
       syscall and re-execute poll in kernel context, never idling.  Counted,
       not fixed, here -- see the comment on poll_park_returns.  (Deliberately
       no return-value assignment: changing what a park returns is a behaviour
       change and belongs with the fix, once the counter says the loop is real.) */
    { extern int poll_park_returns; poll_park_returns++; }
  } else {
    tf->regs[0] = (uint64_t)r;
  }
}

/* SYS_SOCKETPAIR (77): (domain, type, proto, int fds[2]) -> 0 | -errno. */
static void sys_socketpair(struct trap_frame *tf) {
  int domain = (int)tf->regs[5]; // rdi
  int type = (int)tf->regs[4]; // rsi
  int proto = (int)tf->regs[3]; // rdx
  int *ufds = (int *)tf->regs[9]; // r10
  struct process *caller = current_process();

  if (!ufds || ((uint64_t)ufds & 3) != 0 ||
      !sys_user_range_ok((uint64_t)ufds, 2 * sizeof(int))) {
    tf->regs[0] = -EFAULT; /* validated before any allocation (§3) */
    return;
  }

  int out[2] = {-1, -1};
  int r = file_socketpair(caller, domain, type, proto, out);
  if (r == 0) {
    ufds[0] = out[0];
    ufds[1] = out[1];
  }
  tf->regs[0] = (uint64_t)r;
}

/* SYS_SENDMSG (78): (fd, struct msghdr *msg, flags) -> bytes | -errno. */
static void sys_sendmsg(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  uint64_t umsg = tf->regs[4]; // rsi
  int flags = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();

  if (!sys_user_range_ok(umsg, sizeof(struct k_msghdr))) {
    tf->regs[0] = -EFAULT;
    return;
  }
  int r = file_sendmsg(caller, fd, umsg, flags);
  if (r == -2) {
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2;
    }
    save_context(caller, tf);
    schedule(tf, 0);
  } else {
    tf->regs[0] = (uint64_t)r;
  }
}

/* SYS_RECVMSG (79): (fd, struct msghdr *msg, flags) -> bytes | -errno. */
static void sys_recvmsg(struct trap_frame *tf) {
  int fd = (int)tf->regs[5]; // rdi
  uint64_t umsg = tf->regs[4]; // rsi
  int flags = (int)tf->regs[3]; // rdx
  struct process *caller = current_process();

  if (!sys_user_range_ok(umsg, sizeof(struct k_msghdr))) {
    tf->regs[0] = -EFAULT;
    return;
  }
  int r = file_recvmsg(caller, fd, umsg, flags);
  if (r == -2) {
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2;
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

/* --- IceWM M1 (icewm.md section 4.1): named AF_UNIX (96-102).  Thin
 * layer over fs.c, which validates every user range before changing any
 * state; -2 parks (blocking accept/connect) ride the pipe restart. ---- */

static void sys_bind(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)file_bind_user(
      current_process(), (int)tf->regs[5], tf->regs[4], (int)tf->regs[3]);
}

static void sys_listen(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)file_listen(current_process(),
                                               (int)tf->regs[5], (int)tf->regs[4]);
}

static void sys_accept4(struct trap_frame *tf) {
  struct process *caller = current_process();
  int r = file_accept_user(caller, (int)tf->regs[5], tf->regs[4], tf->regs[3],
                           (int)tf->regs[9]);
  if (r == -2) {
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2; /* rewind over `syscall`: restart when woken */
    }
    save_context(caller, tf);
    schedule(tf, 0);
  } else {
    tf->regs[0] = (uint64_t)(int64_t)r;
  }
}

static void sys_connect_addr(struct trap_frame *tf) {
  struct process *caller = current_process();
  int r = file_connect_user(caller, (int)tf->regs[5], tf->regs[4], (int)tf->regs[3]);
  if (r == -2) {
    /* GB-180: -ENOENT is 2, so a connect() to a missing path name returns
       exactly the park/restart sentinel.  usock_name_park() sets the caller
       BLOCKED before returning -2, and a waker may already have flipped it
       to READY; a caller in any other state got -ENOENT from
       usock_name_lookup().  Treat that as the errno it is -- otherwise the
       trap rewinds elr and re-executes the same connect() forever (the
       wave's pid-63 terminal stall). */
    if (!caller || (caller->state != PROC_STATE_BLOCKED &&
                    caller->state != PROC_STATE_READY)) {
      tf->regs[0] = (uint64_t)(int64_t)-ENOENT;
      return;
    }
    if ((tf->cs & 3) == 3) {
      tf->elr -= 2; /* rewind over `syscall`: restart when woken */
    }
    save_context(caller, tf);
    schedule(tf, 0);
  } else {
    tf->regs[0] = (uint64_t)(int64_t)r;
  }
}

static void sys_getsockname(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)file_sockname_user(
      current_process(), (int)tf->regs[5], 0, tf->regs[4], tf->regs[3]);
}

static void sys_getpeername(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)file_sockname_user(
      current_process(), (int)tf->regs[5], 1, tf->regs[4], tf->regs[3]);
}

static void sys_shutdown(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)file_shutdown(current_process(),
                                                 (int)tf->regs[5], (int)tf->regs[4]);
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
  /* IceWM M1: SO_PEERCRED writes a 12-byte struct ucred; validate what
   * the caller's *len offers beyond one int (capped). */
  if (*len > (int)sizeof(int) &&
      !sys_user_range((uint64_t)val, (uint64_t)(*len > 64 ? 64 : *len), 1)) {
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

/* L8 ABI extension (row 87): (clk_id, struct timespec*) -> 0 | -EINVAL |
 * -EFAULT.  CLOCK_REALTIME (0) is the boot-anchored wall clock (RTC epoch
 * at boot + monotonic uptime); CLOCK_MONOTONIC (1) is timer_get_ms(). */
static void sys_gettime(struct trap_frame *tf) {
  int clk = (int)tf->regs[5]; // rdi
  void *tp = (void *)tf->regs[4]; // rsi
  int64_t *sec = (int64_t *)tp;
  int64_t *nsec = sec + 1;
  uint64_t ms;

  if (!tp || !sys_user_range((uint64_t)tp, 16, 1)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  if (clk == 0) {               /* CLOCK_REALTIME */
    ms = hb_clock_realtime_ms();
  } else if (clk == 1) {        /* CLOCK_MONOTONIC */
    extern uint64_t timer_get_ms(void);
    ms = timer_get_ms();
  } else if (clk == 4 || clk == 6 || clk == 7) {
    /* M6/L33: CLOCK_MONOTONIC_RAW (4), CLOCK_MONOTONIC_COARSE (6) and
       CLOCK_BOOTTIME (7).  All three are the monotonic clock in this kernel
       (there is no NTP slew to distinguish RAW, no suspend time to add to
       BOOTTIME, and COARSE differs only in resolution).  Measured: Mono's
       GC timer asks for clock 6 at domain reset and ABORTS the process on
       EINVAL ("clock_gettime(CLOCK_MONOTONIC*) failed; errno is 22"). */
    extern uint64_t timer_get_ms(void);
    ms = timer_get_ms();
  } else if (clk == 5) {        /* CLOCK_REALTIME_COARSE */
    ms = hb_clock_realtime_ms();
  } else {
    /* 2/3 are PROCESS/THREAD_CPUTIME: answering them with wall time would be
       a lie, and nothing in the target set uses them. */
    tf->regs[0] = -EINVAL;
    return;
  }
  *sec = (int64_t)(ms / 1000);
  *nsec = (int64_t)((ms % 1000) * 1000000);
  tf->regs[0] = 0;
}

/* L8 ABI extension (row 88): (struct hb_stackinfo*) -> 0 | -EFAULT.
 * Returns the calling process's MAIN-THREAD kernel-created stack region:
 * v2 = [USER_MAIN_STK_LIMIT_V2, +USER_MAIN_STK_SIZE); v1 = the cleared top
 * USER_STACK_CLEAR_SIZE bytes of the 32 MiB user region.  WebKit's
 * StackBounds (UNIX branch) gets the same numbers via pthread_getattr_np. */
static void sys_getstack(struct trap_frame *tf) {
  struct process *p = current_process();
  void *out = (void *)tf->regs[5]; // rdi
  uint64_t *base = (uint64_t *)out;
  uint64_t *size = base + 1;

  if (!out || !sys_user_range((uint64_t)out, 16, 1)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  if (p && p->as) {             /* v2 */
    *base = USER_MAIN_STK_LIMIT_V2;
    *size = USER_MAIN_STK_SIZE;
  } else {                      /* v1 */
    *base = USER_VIRT_BASE + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE;
    *size = USER_STACK_CLEAR_SIZE;
  }
  tf->regs[0] = 0;
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
      /* P4/P5 carve-out (docs/browser/p4-ipc-design.md §6.5): a backend's
         rich negative errno (e.g. -EPIPE from a closed pipe peer) passes
         through; only the legacy -1 keeps the historical EBADF mapping
         for the older backends.  -2 is the restart marker handled above. */
      tf->regs[0] = (ret == -1) ? (uint64_t)-EBADF : (uint64_t)(int64_t)ret;
    } else {
      tf->regs[0] = ret;
    }
  } else {
    tf->regs[0] = -EFAULT;
  }
}

/* GB-141 (mechanism, V2): musl's stdio writes through writev(2), NOT write(2) —
   __stdio_write builds an iovec and calls writev.  The compat table had no case 20,
   so every printf byte fell to -ENOSYS and died (raw write(1) landed via file_write's
   fd-1 walk).  Loop the iovec array, validating each range, and write each segment
   through file_write (which carries the WK-1 fd-1/2 console fallback).
   GB-143 (reviewer bounce): honour file_write's OTHER return shapes, not just full
   size/negative errno — a parked write (-2), a short write (0 <= r < len), and one
   over-2GiB iovec_len would otherwise fabricate a byte count the caller never wrote. */
#define COMPAT_MAX_RW_COUNT 0x7ffff000ULL  /* Linux MAX_RW_COUNT per write() */
static void compat_writev(struct trap_frame *tf) {
  struct process *c = current_process();
  int fd = (int)tf->regs[5];
  uint64_t iov = tf->regs[4];
  long cnt = (long)tf->regs[3];
  if (cnt < 0 || cnt > 1024) { tf->regs[0] = (uint64_t)(int64_t)-EINVAL; return; }
  if (cnt == 0) { tf->regs[0] = 0; return; }
  if (!sys_user_range_ok(iov, (uint64_t)cnt * 16)) { tf->regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
  int64_t total = 0;
  for (long i = 0; i < cnt; i++) {
    uint64_t base = 0, len = 0;
    if (vm_user_read(c, iov + (uint64_t)i * 16, &base, 8) != 0 ||
        vm_user_read(c, iov + (uint64_t)i * 16 + 8, &len, 8) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT; return;
    }
    if (len == 0) continue;
    if (len > COMPAT_MAX_RW_COUNT) len = COMPAT_MAX_RW_COUNT; /* clamp one write() */
    int r = file_write(c, fd, (const void *)(uintptr_t)base, (int)len, tf);
    if (r == -2) {
      /* A parked write (full pipe / unix socket).  Mirror sys_write: rewind the
         2-byte `syscall` and re-schedule so the write RESTARTS on wake; the parked
         segment resumes as if nothing was written.  A subsequent iovec segment that
         already advanced total is a short write, not a park — see below. */
      if (total > 0) { tf->regs[0] = (uint64_t)total; return; }
      if ((tf->cs & 3) == 3) tf->elr -= 2;
      save_context(c, tf);
      schedule(tf, 0);
      return;
    }
    if (r < 0) { tf->regs[0] = (uint64_t)(int64_t)r; return; }
    total += r;
    /* Short write: Linux stops here and returns the partial total, NOT the next
       segment (a reordered stream with a matching count would hide the loss). */
    if ((uint64_t)r < len) { tf->regs[0] = (uint64_t)total; return; }
  }
  tf->regs[0] = (uint64_t)total;
}

/* GB-169: readv(19).  musl's stdio reads through readv(fd, iov, 2) — a buffered
   file read is a two-segment iovec — so with no arm every fread() returned
   -ENOSYS: DOOM opened doom1.wad and then rejected its magic ("doesn't have
   IWAD or PWAD id") even though the same file read()s fine.  Mirrors
   compat_writev's iovec walk and parked-pipe handling. */
static void compat_readv(struct trap_frame *tf) {
  struct process *c = current_process();
  int fd = (int)tf->regs[5];
  uint64_t iov = tf->regs[4];
  long cnt = (long)tf->regs[3];
  if (cnt < 0 || cnt > 1024) { tf->regs[0] = (uint64_t)(int64_t)-EINVAL; return; }
  if (cnt == 0) { tf->regs[0] = 0; return; }
  if (!sys_user_range_ok(iov, (uint64_t)cnt * 16)) { tf->regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
  int64_t total = 0;
  for (long i = 0; i < cnt; i++) {
    uint64_t base = 0, len = 0;
    if (vm_user_read(c, iov + (uint64_t)i * 16, &base, 8) != 0 ||
        vm_user_read(c, iov + (uint64_t)i * 16 + 8, &len, 8) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT; return;
    }
    if (len == 0) continue;
    if (len > COMPAT_MAX_RW_COUNT) len = COMPAT_MAX_RW_COUNT;
    if (!sys_user_range_ok(base, len)) { tf->regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
    int r = file_read(c, fd, (void *)(uintptr_t)base, (int)len, tf);
    if (r == -2) {
      if (total > 0) { tf->regs[0] = (uint64_t)total; return; }
      if ((tf->cs & 3) == 3) tf->elr -= 2;
      save_context(c, tf);
      schedule(tf, 0);
      return;
    }
    if (r < 0) { tf->regs[0] = (uint64_t)(int64_t)(total > 0 ? total : r); return; }
    total += r;
    if ((uint64_t)r < len) { tf->regs[0] = (uint64_t)total; return; }
  }
  tf->regs[0] = (uint64_t)total;
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
  char args[HO_FLAT_ARGS_LEN]; /* w5 (vi): flat args cap 1024 */
};

/* SEC-OBS-A (security-plan.md observation A): indexed by caller->pid, so
 * it MUST be at least the whole PID space; a hardcoded size silently goes
 * out of bounds the day MAX_PROCESSES grows past it. */
static struct sys_spawn_args spawn_args_pool[MAX_PROCESSES];
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
  int stdin_fd = (int)tf->regs[4]; // rsi
  int stdout_fd = (int)tf->regs[3]; // rdx
  int stderr_fd = (int)tf->regs[9]; // r10 — the 4th syscall arg.  (regs[2] is
  /* rcx, clobbered by the syscall instruction to hold the return address --
     reading stderr_fd from it handed the child a garbage fd (GB-095).) */
  const char *args_ptr = (const char *)tf->regs[7]; // r8

  struct process *caller = current_process();
  if (!caller) {
    tf->regs[0] = -1;
    return;
  }

  char kfile[32];
  if (u_strcpy((const char *)tf->regs[5], kfile, sizeof kfile)) { /* L73 */
    /* SEC-OBS-A: pid indexes spawn_args_pool; refuse a corrupt/out-of-range
     * pid before writing through it. */
    if (caller->pid < 0 || caller->pid >= MAX_PROCESSES) {
      tf->regs[0] = -1;
      return;
    }

    struct sys_spawn_args *args = &spawn_args_pool[caller->pid];
    int i = 0;
    while (kfile[i] && i < 31) {
      args->filename[i] = kfile[i];
      i++;
    }
    args->filename[i] = '\0';
    args->stdin_fd = stdin_fd;
    args->stdout_fd = stdout_fd;
    args->stderr_fd = stderr_fd;
    args->caller_pid = caller->pid;

    /* L73: same rule for the flat argument blob -- u_strcpy reads it through
       vm_user_read, so a hole answers "no args" instead of faulting here. */
    char kargs[HO_FLAT_ARGS_LEN];
    if (args_ptr && u_strcpy((const char *)args_ptr, kargs, sizeof kargs)) {
      int k = 0;
      while (kargs[k] && k < HO_FLAT_ARGS_LEN - 1) {
        args->args[k] = kargs[k];
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
    {
      extern uint64_t timer_get_ms(void);
      /* GB-063: bound the parked wait -- see SPAWN_WAIT_BUDGET_MS. */
      caller->spawn_deadline_ms = timer_get_ms() + SPAWN_WAIT_BUDGET_MS;
    }
    caller->state = PROC_STATE_WAIT_SPAWN;
    spinlock_release_irqrestore(&proc_lock, flags);

    extern int process_create_kernel_nowait(void (*entry)(void*), void *arg);
    extern uint64_t timer_get_ms(void);
    int wpid = process_create_kernel_nowait(sys_spawn_worker, args);
    /* GB-057: this used to retry the worker slot for up to ~30 minutes
       (18000 x 100 ms) of HOT SPIN on the calling CPU.  Under the boot
       test wave the process table is legitimately full, and every spawn
       caller parks in WAIT_SPAWN holding a PCB slot while its worker
       needs a free slot from that same table: waiting longer can never
       fix that — it is a circular wait, and seven cores burning it at
       once wedges the guest (watchdog: "console silent for more than
       20 s", 7 CPUs at age 350 s inside this syscall, loader starved).
       Wait while the free-slot count is MOVING (other processes are
       exiting and freeing rows — the transient case), and give up as soon
       as it stalls or the budget runs out; the failure path below is the
       documented -EAGAIN class, and spawn2 callers retry it with usleep()
       so they yield the CPU instead of stealing it. */
    uint64_t t_start = timer_get_ms();
    uint64_t last_progress = t_start;
    int free_before = -1;
    for (int attempt = 0; attempt < 500 && wpid < 0; attempt++) {
      uint64_t t0 = timer_get_ms();
      for (volatile int spin = 0; spin < 400000; spin++) {
        if (timer_get_ms() - t0 >= 10u) break;
      }
      wpid = process_create_kernel_nowait(sys_spawn_worker, args);
      if (wpid >= 0)
        break;
      int free_now = process_free_slots();
      uint64_t now = timer_get_ms();
      if (free_now != free_before) {
        free_before = free_now;
        last_progress = now;
      } else if (now - last_progress >= 2000u) {
        break; /* table stalled: waiting further cannot succeed */
      }
      if (now - t_start >= 5000u)
        break;
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

/* --- P5 S5 (D4, row 86): SYS_SPAWN_EX ------------------------------- */

static void sys_spawn_ex_worker(void *arg) {
  struct ho_spawn_ex *ex = (struct ho_spawn_ex *)arg;
  int r = load_and_run_spawn_ex(ex);

  struct process *caller = process_get_pcb(ex->caller_pid);
  if (caller) {
    extern spinlock_t proc_lock;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (caller->state == PROC_STATE_WAIT_SPAWN) {
      caller->spawn_retval = r;
      caller->context[0] = (uint64_t)(int64_t)r; /* return value in rax */
      caller->state = PROC_STATE_READY;
    }
    spinlock_release_irqrestore(&proc_lock, flags);
  }

  kernel_exit();
}

/* spawn_ex(path, argv, envp, fdmap, n): marshal the caller's pointers into
 * the caller's pool slot (still in the caller's context; x64 ABI: rdi,
 * rsi, rdx, r10, r8 -> regs[5], regs[4], regs[3], regs[9], regs[7]),
 * park in WAIT_SPAWN, run the spawn2 worker machinery, return pid |
 * -errno. */
static void sys_spawn_ex(struct trap_frame *tf) {
  char *const *argv = (char *const *)tf->regs[4];
  const char *envp = (const char *)tf->regs[3];
  const int *fdmap = (const int *)tf->regs[9];
  int n = (int)tf->regs[7];

  struct process *caller = current_process();
  if (!caller) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }
  /* L73: the path is a user string; copy it through vm_user_read, never with
     a raw NUL scan that can walk off the end of its region. */
  char kpath[32];
  if (!u_strcpy((const char *)tf->regs[5], kpath, sizeof kpath)) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  if (n < 0 || n > HO_SPAWN_EX_MAX_MAP) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL; /* D4.4: bad fdmap */
    return;
  }
  uint64_t fdm_addr = (uint64_t)fdmap;
  if (n > 0 &&
      (!fdmap || (fdm_addr & 3) || !sys_user_range(fdm_addr, (uint64_t)n * 8, 0))) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }

  /* SEC-OBS-A: pid indexes spawn_ex_pool; refuse a corrupt/out-of-range
   * pid before writing through it. */
  if (caller->pid < 0 || caller->pid >= MAX_PROCESSES) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }

  struct ho_spawn_ex *ex = &spawn_ex_pool[caller->pid];
  int i = 0;
  while (kpath[i] && i < 31) {
    ex->filename[i] = kpath[i];
    i++;
  }
  ex->filename[i] = '\0';
  ex->caller_pid = caller->pid;
  if (proc_marshal_argv(caller, argv, ex->argvblob, (int)sizeof ex->argvblob,
                        &ex->argc) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  if (process_read_envp(caller, (const char *const *)envp, ex->env,
                        (int)sizeof ex->env, &ex->envc) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  for (int k = 0; k < n; k++) {
    /* L73: read the pair through the safe accessor, not by dereferencing the
       user's fdmap twice per entry. */
    int pair[2] = {0, 0};
    if (vm_user_read(caller, fdm_addr + (uint64_t)k * 2 * sizeof(int), pair,
                     (int)sizeof pair) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return;
    }
    ex->fdmap_src[k] = pair[0];
    ex->fdmap_dst[k] = pair[1];
  }
  ex->fdmap_n = n;

  extern spinlock_t proc_lock;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  save_context(caller, tf);
  caller->spawn_retval = -EAGAIN; /* failure default until the worker reports */
  {
    extern uint64_t timer_get_ms(void);
    /* GB-063: bound the parked wait -- see SPAWN_WAIT_BUDGET_MS. */
    caller->spawn_deadline_ms = timer_get_ms() + SPAWN_WAIT_BUDGET_MS;
  }
  caller->state = PROC_STATE_WAIT_SPAWN;
  spinlock_release_irqrestore(&proc_lock, flags);

  extern int process_create_kernel_nowait(void (*entry)(void*), void *arg);
  extern uint64_t timer_get_ms(void);
  int wpid = process_create_kernel_nowait(sys_spawn_ex_worker, ex);
  /* GB-057: same circular wait as sys_spawn — see the comment there.
     Wait while the free-slot count moves, give up on a stall or budget. */
  uint64_t t_start = timer_get_ms();
  uint64_t last_progress = t_start;
  int free_before = -1;
  for (int attempt = 0; attempt < 500 && wpid < 0; attempt++) {
    uint64_t t0 = timer_get_ms();
    for (volatile int spin = 0; spin < 400000; spin++) {
      if (timer_get_ms() - t0 >= 10u) break;
    }
    wpid = process_create_kernel_nowait(sys_spawn_ex_worker, ex);
    if (wpid >= 0)
      break;
    int free_now = process_free_slots();
    uint64_t now = timer_get_ms();
    if (free_now != free_before) {
      free_before = free_now;
      last_progress = now;
    } else if (now - last_progress >= 2000u) {
      break;
    }
    if (now - t_start >= 5000u)
      break;
  }
  if (wpid < 0) {
    /* Release-on-worker-failure (D4.4): -EAGAIN, never a hang. */
    flags = spinlock_acquire_irqsave(&proc_lock);
    caller->spawn_retval = -EAGAIN;
    caller->context[0] = (uint64_t)(int64_t)-EAGAIN;
    caller->state = PROC_STATE_READY;
    spinlock_release_irqrestore(&proc_lock, flags);
    tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
  }

  schedule(tf, 0);

  /* Deliver the result once the caller runs again (same race handling as
     sys_spawn): read the worker's word into both the live frame and the
     saved context so either resume path returns it. */
  caller->context[0] = (uint64_t)caller->spawn_retval;
  tf->regs[0] = (uint64_t)caller->spawn_retval;
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
/* GX (docs/graphics-accel.md): damage-rect present (SYS_FLUSH_FB_RECTS).
 * Local mirror of the user ABI rect; the kernel copy is validated then
 * handed to the driver. */
struct virtio_gpu_xrect { int32_t x, y, w, h; };
extern void virtio_gpu_flush_rects(const struct virtio_gpu_xrect *rects, int count);
extern int virtio_gpu_flush_args_ok(const struct virtio_gpu_xrect *rects,
                                    int count); /* SEC-7 */

static void sys_map_fb(struct trap_frame *tf) {
  uint64_t phys_addr = (uint64_t)virtio_gpu_get_framebuffer();
  {
    /* icewm.md §9: an attached process maps its private surface. */
    int vd = vdisp_attached(current_process());
    if (vd) {
      tf->regs[0] = (uint64_t)vdisp_map_fb(current_process(), vd);
      return;
    }
  }
  struct process *cur = current_process();
  if (cur)
    cur = process_group(cur);
#if WD_X64_FLUSH_PROBE
  uart_puts("[WD] SYS_MAP_FB pid=");
  print_int(cur ? cur->pid : -1);
  uart_puts(" phys=");
  uart_print_hex(phys_addr);
#endif
  if (cur && cur->as) {
    /* P2.4 (S4, design 7.3): a v2 process maps the fb into its own AS at
       the reserved USER_FB_OFF slot (same physical frames, VMK_FB);
       idempotent per process. */
    int64_t va = vm_map_fb(cur, phys_addr);
#if WD_X64_FLUSH_PROBE
    uart_puts(" v2va=");
    uart_print_hex((uint64_t)va);
    uart_puts("\n");
#endif
    tf->regs[0] = (uint64_t)va;
    return;
  }
  mmu_map_user_framebuffer(phys_addr);
#if WD_X64_FLUSH_PROBE
  uart_puts(" v1va=60000000\n");
#endif
  tf->regs[0] = USER_FB_VIRT_BASE; // Return user virtual address
}

/* GB-025: /dev/fb0 ioctl (SYS_FB_IOCTL).  Registers: rdi=req, rsi=arg.
 * FBIOGET_VSCREENINFO/FSCREENINFO copy a Linux-layout struct OUT to arg
 * (validated first); FBIOBLANK takes a plain value (validated implicitly
 * by hb_fb_blank_ioctl).  Returns 0 | -1 (unknown request) | -EFAULT. */
/* GB-032: shared with the Linux-ABI ioctl arm in linux_compat_fd.c — that
   dispatcher runs before this file's number-translation switch, so the FBIO
   routing is called from both places. */
void sys_fb_ioctl(struct trap_frame *tf) {
  unsigned long req = (unsigned long)tf->regs[5];     /* rdi */
  void *arg = (void *)tf->regs[4];                    /* rsi */
  uint64_t smem_start = (uint64_t)virtio_gpu_get_framebuffer();

  size_t sz = hb_fb_ioctl_arg_size(req);
  if (sz == (size_t)-1) {
    tf->regs[0] = (uint64_t)(int64_t)-1;   /* unknown request */
    return;
  }
  if (sz == 0 && vdisp_attached(current_process())) {
    tf->regs[0] = 0; /* icewm.md §9: FBIOBLANK never reaches the real display */
    return;
  }
  /* FBIOBLANK (sz==0) takes a value, not a buffer: nothing to validate. */
  if (sz != 0 && !sys_user_range((uint64_t)arg, sz, 1)) { /* 1 = ok */
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)(int64_t)hb_fb_ioctl_route(req, arg, smem_start);
}

/* icewm.md §9: an attached process's present sleeps until its frame slot
 * (vdisp_flush_pace); same park as SYS_SLEEP, returning 0. */
static void vdisp_present_pace(struct trap_frame *tf, int vd) {
  int ms = vdisp_flush_pace(vd);
  tf->regs[0] = 0;
  struct process *cur = current_process();
  if (cur && ms > 0) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    cur->wake_ms = timer_get_ms() + ms;
    cur->state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
  }
}

static void sys_flush_fb(struct trap_frame *tf) {
#if WD_X64_FLUSH_PROBE
  /* WD probe: attribute every full flush to its process (used to
   * correlate a second mapper's blit with the kernel fb + scanout). */
  struct process *cur = current_process();
  int pid = cur ? cur->pid : -1;
  uart_puts("[WD] SYS_FLUSH_FB pid=");
  print_int(pid);
  uart_puts("\n");
#endif
  {
    int vd = vdisp_attached(current_process());
    if (vd) { /* icewm.md §9: record damage for the host, not the display */
      vdisp_flush(vd, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
      vdisp_present_pace(tf, vd);
      return;
    }
  }
  virtio_gpu_flush();
  tf->regs[0] = 0;
}

/* GX (docs/graphics-accel.md): damage-rect present.  Validates the user
 * rect array, copies it into a kernel buffer, and hands it to the driver
 * (which clamps/merges as it may -- covered pixels must end identical to a
 * full flush).  count 0 is a no-op; out-of-range counts are EINVAL. */
static void sys_flush_fb_rects(struct trap_frame *tf) {
  const struct virtio_gpu_xrect *urects = (const struct virtio_gpu_xrect *)tf->regs[5]; // rdi
  int count = (int)tf->regs[4]; // rsi
  if (count < 0 || count > 32) {
    tf->regs[0] = (uint64_t)-EINVAL;
    return;
  }
  if (count == 0) {
    tf->regs[0] = 0;
    return;
  }
  if (!urects || ((uint64_t)urects & 3) != 0 ||
      !sys_user_range_ok((uint64_t)urects, (uint64_t)count * sizeof(struct virtio_gpu_xrect))) {
    tf->regs[0] = (uint64_t)-EFAULT;
    return;
  }
  struct virtio_gpu_xrect krects[32];
  /* SEC-7: atomic validate+copy.  The old loop read user pages after a
     separate range check (validate-then-use TOCTOU) and handed raw
     attacker-shaped rects straight to the driver; vm_user_read validates
     each page under vm_lock as it copies, and nothing reaches
     virtio_gpu_flush_rects until it has passed boundary validation. */
  if (vm_user_read(current_process(), (uint64_t)urects, krects,
                   count * (int)sizeof(struct virtio_gpu_xrect)) != 0) {
    tf->regs[0] = (uint64_t)-EFAULT;
    return;
  }
  if (!virtio_gpu_flush_args_ok(krects, count)) {
    tf->regs[0] = (uint64_t)-EINVAL;
    return;
  }
  {
    int vd = vdisp_attached(current_process());
    if (vd) { /* icewm.md §9 */
      for (int i = 0; i < count; i++)
        vdisp_flush(vd, krects[i].x, krects[i].y, krects[i].w, krects[i].h);
      vdisp_present_pace(tf, vd);
      return;
    }
  }
  virtio_gpu_flush_rects(krects, count);
  tf->regs[0] = 0;
}

static void sys_get_events(struct trap_frame *tf) {
  void *buf = (void *)tf->regs[5]; // rdi
  int max_events = (int)tf->regs[4]; // rsi
  if (sys_user_range((uint64_t)buf,
                     max_events > 0 ? (uint64_t)max_events * 8 : 8, 1)) {
    int vd = vdisp_attached(current_process()); /* icewm.md §9 */
    if (vd)
      tf->regs[0] = max_events > 0
          ? (uint64_t)(int64_t)vdisp_get_events(vd, (struct vdisp_event *)buf, max_events)
          : 0;
    else
      tf->regs[0] = virtio_input_get_events(buf, max_events);
  } else {
    tf->regs[0] = -1;
  }
}

/* GB-027: /dev/input/event* ioctl (SYS_INPUT_IOCTL).  Registers: rdi=req,
 * rsi=arg.  EVIOCGVERSION/NAME/GBIT/GABS copy their Linux-ABI reply OUT
 * to arg (validated to hb_input_ioctl_arg_size() first).  0 | -1
 * (unknown request) | -EFAULT (bad arg pointer). */
static void sys_input_ioctl(struct trap_frame *tf) {
  unsigned long req = (unsigned long)tf->regs[5];     /* rdi */
  void *arg = (void *)tf->regs[4];                    /* rsi */
  size_t sz = hb_input_ioctl_arg_size(req);
  if (sz == (size_t)-1) {
    tf->regs[0] = (uint64_t)(int64_t)-1;   /* unknown request */
    return;
  }
  if (sz != 0 && !sys_user_range((uint64_t)arg, sz, 1)) { /* 1 = ok */
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  tf->regs[0] = (uint64_t)(int64_t)hb_input_ioctl_route(req, arg);
}

/* GB-027: non-blocking /dev/input/event* read (SYS_INPUT_READ).  Registers:
 * rdi=buf, rsi=max.  Drains the virtio_input ring and writes up to max
 * Linux-ABI (24 B) input_event records out to buf.  Returns the count read
 * (0 if nothing pending — non-blocking), -EFAULT on a bad buffer. */
static void sys_input_read(struct trap_frame *tf) {
  struct hb_input_event *ubuf = (struct hb_input_event *)tf->regs[5]; /* rdi */
  int max = (int)tf->regs[4];                                        /* rsi */
  if (max < 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }
  if (max == 0) {
    tf->regs[0] = 0;
    return;
  }
  if (!sys_user_range((uint64_t)ubuf, /* returns 1 when the range is OK */
                     (uint64_t)max * sizeof(struct hb_input_event), 1)) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  extern uint64_t timer_get_ms(void);
  struct virtio_input_event raw[32];
  int want = max > 32 ? 32 : max;         /* bounded scratch: non-blocking */
  int vdr = vdisp_attached(current_process()); /* icewm.md §9 */
  int got = vdr ? vdisp_get_events(vdr, (struct vdisp_event *)raw, want)
                : virtio_input_get_events(raw, want);
  uint64_t t = timer_get_ms();
  uint64_t sec = t / 1000u, usec = (t % 1000u) * 1000u;
  for (int i = 0; i < got; i++)
    hb_input_fill_event(&ubuf[i], raw[i].type, raw[i].code, raw[i].value,
                        sec, usec);
  tf->regs[0] = (uint64_t)got;
}

/* icewm.md §9: SYS_VDISPLAY (op, a1, a2) -- see src/include/vdisplay.h. */
static void sys_vdisplay(struct trap_frame *tf) {
  int op = (int)tf->regs[5];
  uint64_t a1 = tf->regs[4];
  uint64_t a2 = tf->regs[3];
  struct process *cur = current_process();
  int64_t r;
  if (op == VDISP_OP_ATTACH) {
    r = vdisp_sys_attach(cur, (int)a1);
  } else if (op == VDISP_OP_DETACH) {
    r = vdisp_sys_detach(cur);
  } else if (op == VDISP_OP_INJECT) {
    int n = (int)a2;
    struct vdisp_event k[64];
    if (n < 0 || n > 64) {
      r = -EINVAL;
    } else if (n == 0) {
      r = 0;
    } else if (!a1 || (a1 & 3) ||
               vm_user_read(cur, a1, k, n * (int)sizeof(struct vdisp_event)) != 0) {
      r = -EFAULT;
    } else {
      r = vdisp_sys_inject(cur, k, n);
    }
  } else if (op == VDISP_OP_STATUS) {
    if (!a1 || (a1 & 3) || !sys_user_range(a1, sizeof(struct vdisp_status), 1)) {
      r = -EFAULT;
    } else {
      struct vdisp_status st;
      r = vdisp_sys_status(cur, &st);
      if (r == 0) {
        struct vdisp_status *u = (struct vdisp_status *)a1;
        u->flush_seq = st.flush_seq;
        u->dx = st.dx; u->dy = st.dy; u->dw = st.dw; u->dh = st.dh;
        u->mapped = st.mapped;
        u->reads = st.reads;
        u->read_age_ms = st.read_age_ms;
        u->queued = st.queued;
      }
    }
  } else {
    r = -EINVAL;
  }
  tf->regs[0] = (uint64_t)r;
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

static void sys_getdents64(struct trap_frame *tf) {
  int fd = (int)tf->regs[5];             // rdi = fd
  void *buf = (void *)tf->regs[4];       // rsi = dirp
  int count = (int)tf->regs[3];          // rdx = count

  struct process *caller = current_process();
  if (sys_user_range((uint64_t)buf, count > 0 ? (uint64_t)count : 0, 1)) {
    extern int file_getdents64(struct process *p, int fd, void *ubuf, int n);
    int ret = file_getdents64(caller, fd, buf, count);
    tf->regs[0] = (uint64_t)ret;         /* bytes (>=0) or negative errno */
  } else {
    tf->regs[0] = -EFAULT;
  }
}

static void sys_mkdir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  struct process *caller = current_process();
  char kpath[VFS_PATH_MAX];
  int plen = copy_user_path(path, kpath, sizeof kpath);
  if (plen < 0) {
    tf->regs[0] = (uint64_t)(int64_t)plen;
    return;
  }
  extern int file_mkdir(struct process *cur, const char *path);
  int r = file_mkdir(caller, kpath);
  tf->regs[0] = r < 0 ? -EEXIST : r;
}

/* W2-14 (selfhost): SYS_RMDIR -- remove an EMPTY directory.  The kernel
 * layer returns precise negative errnos (fat16_rmdir: -ENOENT/-ENOTDIR/
 * -ENOTEMPTY/-EBUSY; vfs_rmdir maps a routed read-only mount to -EROFS),
 * passed through verbatim -- no coarse -ENOENT folding here. */
static void sys_rmdir(struct trap_frame *tf) {
  char kpath[VFS_PATH_MAX];
  if (!u_strcpy((const char *)tf->regs[5], kpath, sizeof kpath)) { /* L73 */
    tf->regs[0] = -EFAULT;
    return;
  }
  extern int vfs_rmdir(const char *path);
  int r = vfs_rmdir(kpath);
  tf->regs[0] = r < 0 ? (uint64_t)r : r;
}

/* mount(source, target): mount an NFS export ("A.B.C.D:/export") at a
 * directory of the FAT volume.  The mount point is created when missing. */
static void sys_mount(struct trap_frame *tf) {
  char ksrc[VFS_PATH_MAX], ktgt[VFS_PATH_MAX];
  if (!u_strcpy((const char *)tf->regs[5], ksrc, sizeof ksrc) || /* L73 */
      !u_strcpy((const char *)tf->regs[4], ktgt, sizeof ktgt)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  extern int vfs_mount(const char *source, const char *target);
  int r = vfs_mount(ksrc, ktgt);
  tf->regs[0] = r < 0 ? -EINVAL : r;
}

/* vfs_bind(from, to): alias the subtree at `from` onto the path `to` (M5c).
 * Registers a prefix translation, not a second mount: only path resolution
 * changes, so mmap of a bound file uses the target filesystem's objects. */
static void sys_vfs_bind(struct trap_frame *tf) {
  char kfrom[VFS_PATH_MAX], kto[VFS_PATH_MAX];
  if (!u_strcpy((const char *)tf->regs[5], kfrom, sizeof kfrom) || /* L73 */
      !u_strcpy((const char *)tf->regs[4], kto, sizeof kto)) {
    tf->regs[0] = -EFAULT;
    return;
  }
  extern int vfs_bind_add(const char *from, const char *to);
  int r = vfs_bind_add(kfrom, kto);
  tf->regs[0] = r < 0 ? -EINVAL : r;
}

/* umount(target): unmount the NFS export mounted exactly at `target`. */
static void sys_umount(struct trap_frame *tf) {
  char ktgt[VFS_PATH_MAX];
  if (!u_strcpy((const char *)tf->regs[5], ktgt, sizeof ktgt)) { /* L73 */
    tf->regs[0] = -EFAULT;
    return;
  }
  extern int vfs_umount(const char *target);
  int r = vfs_umount(ktgt);
  tf->regs[0] = r < 0 ? -EINVAL : r;
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

/* GB-016: Linux uname(2), number 63.  rdi = struct utsname *.
   The identity fill is linux_fill_utsname() in src/kernel/linux_uname.c
   (merged with PR #61 and already pinned by abi_t2_test.c); what was missing --
   and what the compat probe's uname_* rows have been waiting for -- is this
   dispatch.  Linux returns 0, or -EFAULT when the pointer is not writable.
   The struct is linux_utsname_t (6 x 65 bytes, 390 total), so the range check
   covers the whole thing rather than just the first field. */
static void sys_uname(struct trap_frame *tf) {
  uint64_t up = tf->regs[5];                 /* rdi */
  if (!sys_user_range(up, sizeof(linux_utsname_t), 1)) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }
  linux_utsname_t u;
  linux_fill_utsname(&u);
  char *dst = (char *)(uintptr_t)up;
  const char *src = (const char *)&u;
  for (unsigned i = 0; i < sizeof(u); i++) dst[i] = src[i];
  tf->regs[0] = 0;
}

/* P6.1: SYS_FTRUNCATE (row 33) -- plain (fd, length) pair. */
static void sys_ftruncate(struct trap_frame *tf) {
  struct process *caller = current_process();
  tf->regs[0] = (uint64_t)file_ftruncate(caller, (int)tf->regs[5],   // rdi
                                         (int64_t)tf->regs[4]);      // rsi
}

static void sys_stat(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5];  // rdi
  struct k_stat *st = (struct k_stat *)tf->regs[4];  // rsi
  char kpath[256];
  int plen = copy_user_path(path, kpath, sizeof kpath);
  if (plen < 0 || !sys_user_range((uint64_t)st, sizeof(struct k_stat), 1)) {
    tf->regs[0] = (uint64_t)(int64_t)(plen < 0 ? plen : -EFAULT);
    return;
  }
  struct process *caller = current_process();
  int err = 0;
  extern int file_stat_path(struct process *p, const char *path,
                            struct k_stat *st, int *errp);
  if (file_stat_path(caller, kpath, st, &err) == 0)
    tf->regs[0] = 0;
  else
    tf->regs[0] = (uint64_t)(-err);
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

/* GB-164: Linux-ABI stat family.  musl calls newfstatat(262) for stat()/lstat()
   and fstat(5) for fstat(); the compat table had no arm for any of them, so a
   musl program could open a file but never see its size (measured: DOOMW4's
   IWAD search -> -ENOSYS -> "No IWAD file was found").  The kernel's k_stat is a
   different, smaller shape than Linux x86_64's struct stat, so translate rather
   than copy. */
struct linux_stat_x64 {
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, __pad0;
  uint64_t st_rdev, st_size, st_blksize, st_blocks;
  int64_t st_atime_sec, st_atime_nsec;
  int64_t st_mtime_sec, st_mtime_nsec;
  int64_t st_ctime_sec, st_ctime_nsec;
  int64_t __unused[3];
};

static void linux_stat_x64_fill(struct linux_stat_x64 *o, const struct k_stat *k) {
  o->st_dev = k->st_dev;
  o->st_ino = k->st_ino;
  o->st_nlink = (uint64_t)k->st_nlink;
  o->st_mode = k->st_mode;
  o->st_uid = k->st_uid;
  o->st_gid = k->st_gid;
  o->__pad0 = 0;
  o->st_rdev = k->st_rdev;
  o->st_size = (uint64_t)k->st_size;
  o->st_blksize = (uint64_t)k->st_blksize;
  o->st_blocks = (uint64_t)k->st_blocks;
  o->st_atime_sec = k->st_atime; o->st_atime_nsec = 0;
  o->st_mtime_sec = k->st_mtime; o->st_mtime_nsec = 0;
  o->st_ctime_sec = k->st_ctime; o->st_ctime_nsec = 0;
  o->__unused[0] = o->__unused[1] = o->__unused[2] = 0;
}

static void compat_fstat(struct trap_frame *tf) {
  int fd = (int)tf->regs[5];                                  /* rdi */
  struct linux_stat_x64 *out = (struct linux_stat_x64 *)tf->regs[4]; /* rsi */
  if (!sys_user_range((uint64_t)out, sizeof *out, 1)) {
    tf->regs[0] = (uint64_t)(int64_t)(-EFAULT);
    return;
  }
  struct k_stat k;
  int err = 0;
  extern int file_stat_fd(struct process *, int, struct k_stat *, int *);
  if (file_stat_fd(current_process(), fd, &k, &err) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)(-err);
    return;
  }
  linux_stat_x64_fill(out, &k);
  tf->regs[0] = 0;
}

static void compat_stat_path(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5];                /* rdi */
  struct linux_stat_x64 *out = (struct linux_stat_x64 *)tf->regs[4]; /* rsi */
  char kpath[256];
  int plen = copy_user_path(path, kpath, sizeof kpath);
  if (plen < 0 || !sys_user_range((uint64_t)out, sizeof *out, 1)) {
    tf->regs[0] = (uint64_t)(int64_t)(plen < 0 ? plen : -EFAULT);
    return;
  }
  struct k_stat k;
  int err = 0;
  extern int file_stat_path(struct process *, const char *, struct k_stat *, int *);
  if (file_stat_path(current_process(), kpath, &k, &err) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)(-err);
    return;
  }
  linux_stat_x64_fill(out, &k);
  tf->regs[0] = 0;
}

/* newfstatat(dirfd, path, buf, flags): only AT_FDCWD (-100) is honoured; a
   real dirfd is not used by musl's stat()/lstat()/fstatat() on a path string
   here, and returning EBADF is honest for anything else. */
static void compat_newfstatat(struct trap_frame *tf) {
  int dirfd = (int)tf->regs[5];                                /* rdi */
  const char *path = (const char *)tf->regs[4];                /* rsi */
  struct linux_stat_x64 *out = (struct linux_stat_x64 *)tf->regs[3]; /* rdx */
  if (dirfd != -100) {                                         /* AT_FDCWD */
    tf->regs[0] = (uint64_t)(int64_t)(-EBADF);
    return;
  }
  tf->regs[5] = (uint64_t)(uintptr_t)path;
  tf->regs[4] = (uint64_t)(uintptr_t)out;
  compat_stat_path(tf);
}

/* GB-165: openat(dirfd, path, flags, mode).  The clang/musl-target build of
   DOOMW4 reaches the kernel through openat(257), not open(2): 126 `mov $0x101`
   sites in the ELF, none in a musl-gcc build of the same probe.  Only
   AT_FDCWD is honoured (that is all musl's fopen passes); a real dirfd is
   refused rather than silently resolved against the wrong directory. */
static void compat_openat(struct trap_frame *tf) {
  int dirfd = (int)tf->regs[5];                                  /* rdi */
  const char *path = (const char *)tf->regs[4];                  /* rsi */
  int flags = (int)tf->regs[3];                                  /* rdx */
  if (dirfd != -100) {                                           /* AT_FDCWD */
    tf->regs[0] = (uint64_t)(int64_t)(-EBADF);
    return;
  }
  tf->regs[5] = (uint64_t)(uintptr_t)path;
  tf->regs[4] = (uint64_t)(uintptr_t)(int64_t)flags;
  compat_open(tf);
}

static void sys_chdir(struct trap_frame *tf) {
  const char *path = (const char *)tf->regs[5]; // rdi
  struct process *caller = process_group(current_process()); /* P1 (D7) */
  char kpath[VFS_PATH_MAX];
  int plen = copy_user_path(path, kpath, sizeof kpath);
  if (caller && plen >= 0) {
    extern int vfs_chdir(const char *path, char *out_new_cwd, int cap);
    char new_cwd[128];
    if (vfs_chdir(kpath, new_cwd, sizeof new_cwd) == 0) {
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
    /* A bad string is -EFAULT/-ENAMETOOLONG from the copy; a NULL group is
       -EFAULT (upstream's rule, kept verbatim). */
    tf->regs[0] = (uint64_t)(int64_t)(plen < 0 ? plen : -EFAULT);
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
 * PROC_STATE_WAIT_CHILD; result delivered into the saved context).
 * W2-5: the shared core reads the CANONICAL regs[0..2] positions (the
 * ARM layout), but x64 syscall args land scattered (rdi->regs[5],
 * rsi->regs[4], rdx->regs[3]); remap here so real userland waitpid calls
 * -- and the blocking save_context below -- carry the right values. */
static void sys_waitpid(struct trap_frame *tf) {
  /* icewm.md fix log: never rewrite regs[1..3] (rbx/rcx/rdx) -- regs[1] is
     the caller's callee-saved rbx; the old remap returned it holding the
     status pointer (clang -mcmodel=large code keeps addresses in rbx). */
  tf->regs[0] = (uint64_t)(int64_t)process_wait_args(
      tf, (int)tf->regs[5], (int *)tf->regs[4], (int)tf->regs[3], 0);
}

/* W2-5 (selfhost): SYS_WAIT4 -- wait4(pid, status, options, rusage):
 * rdi->regs[5], rsi->regs[4], rdx->regs[3], r10->regs[9]. */
static void sys_wait4(struct trap_frame *tf) {
  tf->regs[0] = (uint64_t)(int64_t)process_wait_args(
      tf, (int)tf->regs[5], (int *)tf->regs[4], (int)tf->regs[3],
      tf->regs[9]);
}

/* SYS_EXEC: replace the current image. Shared loader helper in
 * program_loader.c; here we only marshal the user path/argv.
 * P2.5 (S5 flip): v2-aware via sys_user_range_ok (the old window test
 * rejected every v2 string pointer). */
/* Copy a NUL-terminated user string into kernel memory with full bounds
 * checking. Returns 1 on success (dst NUL-terminated), 0 on bad ptr.
 * w8-torture: validate PER PAGE, not the whole cap up front — a short
 * string near the TOP of a mapped region was dropped wholesale once the
 * w6-run cap bump made the full-range check reach past the region end
 * (mirror of pl_strcpy in program_loader.c + arch/arm/trap.c).
 * SEC-3 (security-plan.md #3): the per-page validate-then-read split was a
 * TOCTOU — between validating page N and reading src[i] a peer could unmap
 * it, faulting the kernel-mode read.  vm_user_read now validates AND reads
 * each byte atomically under vm_lock; only the current byte's page is ever
 * touched, so the w8-torture property (short strings near a region top)
 * still holds. */
int u_strcpy(const char *src, char *dst, int cap) {
  if (!src || cap <= 1)
    return 0;
  struct process *p = current_process();
  for (int i = 0; i < cap - 1; i++) {
    char c;
    if (vm_user_read(p, ((uint64_t)&src[i]), &c, 1) != 0)
      return 0;
    dst[i] = c;
    if (c == 0)
      return 1;
  }
  dst[cap - 1] = '\0';
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

  /* w6-run (mirror of arch/arm/trap.c): 4096-wide buffers — one argv
   * element can be a whole `sh -c` command line (make recipe lines; the
   * S2 kernel link line is ~1.3 KB) and the old 64-per-arg/256-total caps
   * truncated it mid-token.  Matches HO_EXEC_ARGV_LEN; the eargv blob
   * path (HO_EXEC_ARGV_LEN) is primary, this flat string the fallback. */
  char argbuf[HO_EXEC_ARGV_LEN];
  int alen = 0;
  argbuf[0] = '\0';
  if (argv) {
    for (int ai = 1; alen < HO_EXEC_ARGV_LEN - 1 &&
                     sys_user_range_ok((uint64_t)(argv + ai), sizeof(char *)) &&
                     argv[ai] != 0; ai++) {
      char one[HO_EXEC_ARGV_LEN];
      if (!u_strcpy((const char *)argv[ai], one, sizeof one))
        break;
      if (alen)
        argbuf[alen++] = ' ';
      for (int k = 0; one[k] && alen < HO_EXEC_ARGV_LEN - 1; k++)
        argbuf[alen++] = one[k];
    }
  }
  argbuf[alen] = '\0';

  /* P5 (D3-order): argv[] elements live in the OLD image -- marshal them
     into kernel memory before the load clobbers it. */
  char argvblob[HO_EXEC_ARGV_LEN];
  int argvc = 0;
  proc_marshal_argv(cur, (char *const *)argv, argvblob, HO_EXEC_ARGV_LEN,
                    &argvc);

  /* P5 (D3.1/D3.5): marshal envp (rdx, regs[3]) into the kernel env blob; a
     bad pointer anywhere is -EFAULT (argv keeps its truncating behavior). */
  char envbuf[HO_ENV_LEN];
  for (int i = 0; i < HO_ENV_LEN; i++)
    envbuf[i] = 0;
  int envc = 0;
  if (process_read_envp(cur, (const char *const *)tf->regs[3], envbuf,
                        HO_ENV_LEN, &envc) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return;
  }

  /* Success redirects elr + sets regs[0]=0 (the new program never reads
     the exec return); the argv/env blobs were installed kernel-side. */
  int r = process_exec_current(tf, pathbuf, argbuf, namebuf, envbuf, envc,
                               argvblob, argvc);
  if (r < 0)
    tf->regs[0] = (uint64_t)r;  /* success redirects elr + sets regs[0]=0 */
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
  int signum = (int)tf->regs[5];            // rdi
  const uint8_t *uact = (const uint8_t *)tf->regs[4]; // rsi
  uint8_t *uold = (uint8_t *)tf->regs[3];   // rdx
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
  int idx = (int)tf->regs[5];              // rdi
  char *buf = (char *)tf->regs[4];         // rsi
  int size = (int)tf->regs[3];             // rdx
  if (idx >= 0 && size > 0) {
    if (!cur || process_user_ok(cur, (uint64_t)buf, (uint64_t)size, 1) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return;
    }
  }
  tf->regs[0] = (uint64_t)(int64_t)sys_readenv(cur, idx, buf, size);
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

/* GB-018: Linux select(23) and pselect6(270) pass a struct timeval / struct
 * timespec POINTER, while SYS_SELECT takes a millisecond count in r8.  Convert
 * the pointer to milliseconds before handing the (unchanged) fd-set registers
 * to sys_select().  A NULL pointer means "wait forever" (-1), matching Linux;
 * a negative field is EINVAL; a usec/nsec field >= 1e6/1e9 is EINVAL; the
 * timeout rounds UP to the next millisecond, as Linux does.
 *
 * pselect6's sixth argument (r9, a pointer to {sigmask, sigsetsize}) is not
 * applied: the compat surface has no signal-mask swap, and the T4 probe passes
 * an empty set.  That limitation is stated here rather than silently assumed. */
static int compat_timeout_ms(uint64_t ptr, int is_timespec, int *err) {
  struct process *c = current_process();
  int64_t a = 0, b = 0;
  *err = 0;
  if (!ptr) return -1;
  if (!c || !sys_user_range_ok(ptr, 16)) { *err = EFAULT; return 0; }
  if (vm_user_read(c, ptr, &a, 8) != 0 ||
      vm_user_read(c, ptr + 8, &b, 8) != 0) { *err = EFAULT; return 0; }
  if (a < 0 || b < 0) { *err = EINVAL; return 0; }
  if (is_timespec) {
    if (b >= 1000000000LL) { *err = EINVAL; return 0; }
    return (int)(a * 1000 + (b + 999999) / 1000000);
  }
  if (b >= 1000000LL) { *err = EINVAL; return 0; }
  return (int)(a * 1000 + (b + 999) / 1000);
}

/* GB-168: musl's usleep() lands on nanosleep({sec,nsec}, rem).  The kernel's
   sleep syscall (SYS_SLEEP) takes a millisecond count in rdi, so convert the
   timespec and hand it over.  `rem` is left untouched: the call is not
   restarted.  Without this arm usleep() returns -ENOSYS at once and the W4
   tick loop busy-spins (DG_SleepMs was a no-op too). */
static void compat_nanosleep(struct trap_frame *tf) {
  uint64_t req = tf->regs[5];
  int err = 0;
  int ms = compat_timeout_ms(req, 1, &err);
  if (err) {
    tf->regs[0] = (uint64_t)(int64_t)(-err);
    return;
  }
  if (ms <= 0) {
    tf->regs[0] = 0;
    return;
  }
  tf->regs[5] = (uint64_t)(uint32_t)ms;
  tf->regs[0] = 0; /* Linux's nanosleep returns 0; sys_sleep's blocking path
                      leaves rax holding the syscall number otherwise */
  sys_sleep(tf);
}

/* GB-170: musl's nanosleep() — and therefore usleep() — does NOT use syscall
   35.  Measured in the rebuilt musl DoomW4.elf: nanosleep() calls musl's
   clock_nanosleep, whose body loads 0xe6 (230) into rax.  The compat table had
   neither arm, so usleep() returned -ENOSYS instantly.  Convert the relative
   form into the kernel's millisecond SYS_SLEEP; TIMER_ABSTIME is rejected
   (SYS_SLEEP has no absolute deadline) and unknown clock ids are -EINVAL
   rather than a silent wrong sleep. */
static void compat_clock_nanosleep(struct trap_frame *tf) {
  int clk = (int)tf->regs[5];        /* rdi: clockid */
  int flags = (int)tf->regs[4];      /* rsi: flags   */
  uint64_t req = tf->regs[3];        /* rdx: req     */
  int err = 0;
  if (flags & 1) { /* TIMER_ABSTIME */
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }
  /* CLOCK_REALTIME(0), CLOCK_MONOTONIC(1), CLOCK_BOOTTIME(4). */
  if (clk != 0 && clk != 1 && clk != 4) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return;
  }
  int ms = compat_timeout_ms(req, 1, &err);
  if (err) {
    tf->regs[0] = (uint64_t)(int64_t)(-err);
    return;
  }
  if (ms <= 0) {
    tf->regs[0] = 0;
    return;
  }
  tf->regs[5] = (uint64_t)(uint32_t)ms;
  tf->regs[0] = 0; /* clock_nanosleep returns 0 on success (restart-safe) */
  sys_sleep(tf);
}

static void compat_select(struct trap_frame *tf) {
  int err = 0;
  int ms = compat_timeout_ms(tf->regs[7], 0, &err);
  if (err) { tf->regs[0] = (uint64_t)(int64_t)(-err); return; }
  tf->regs[7] = (uint64_t)(int64_t)ms;
  sys_select(tf);
}

static void compat_pselect6(struct trap_frame *tf) {
  int err = 0;
  int ms = compat_timeout_ms(tf->regs[7], 1, &err);
  if (err) { tf->regs[0] = (uint64_t)(int64_t)(-err); return; }
  tf->regs[7] = (uint64_t)(int64_t)ms;
  sys_select(tf);
}

/* GB-148: musl's mkdir()/mkdirat() both use mkdirat(258) (dirfd, path, mode).
 * HobbyOS SYS_MKDIR takes only (path) in rdi, so shift rsi -> rdi.  Only
 * AT_FDCWD (-100) is supported: there is no per-directory fd resolution for
 * mkdir on this surface, and a real dirfd answers -EBADF rather than silently
 * creating the path relative to the process cwd.  mode is ignored by the FAT
 * layer (existing behaviour of SYS_MKDIR). */
static void compat_mkdirat(struct trap_frame *tf) {
  int dirfd = (int)tf->regs[5];       /* rdi */
  uint64_t path = tf->regs[4];        /* rsi */
  if (dirfd != -100) {                /* AT_FDCWD */
    tf->regs[0] = (uint64_t)(int64_t)(-EBADF);
    return;
  }
  tf->regs[5] = path;
  sys_mkdir(tf);
}

/* Last syscall each core entered; 0 = not in a syscall.  A core wedged
   inside a syscall leaves this set, which the stall watchdog prints. */
static volatile int core_in_syscall[MAX_CPUS];

/* L73: name the kernel path that dereferenced a user VA.
 *
 * The L72 ladder died at t~560 s on a kernel-mode #PF with cr2=0x11CB280000 --
 * exactly `base+len` of the 16 MiB mmap the log had just recorded -- and the
 * register row (rax=0x14F, rcx=0x150, r8=0x13F ...) could not name a syscall
 * because by fault time the argument registers are whatever the faulting code
 * left in them.  So: latch the entry tuple at syscall entry (sc_arg/sc_rip)
 * and print it here, with the kernel return addresses from this CPU's stack.
 * Resolve rip and the frames offline against the exact `hobbyos.elf` of the
 * run (tools/rw/l73-ksym.py) -- the kernel carries no symbol table.
 *
 * Two rules for this instrument:
 *   - it must never allocate, take vm_lock, or call into anything that can
 *     fault again: it runs on a stack that is about to be abandoned.
 *   - when it reports absence, prove it can report presence: the [KPF-STK]
 *     row is empty only if the scan found no kernel text address, which the
 *     `frames=` counter states.
 */
/* L73: the only way a report may touch a user pointer.
 *
 * A fault report is produced for a process that is being killed for touching
 * an address it must not touch, so every user value in it (the frame pointer
 * above all) is a candidate for exactly that address.  Reading it with a plain
 * `*(uint64_t *)va` turns the report into a second, KERNEL-mode #PF on the same
 * hole: the process dies with "[KERNEL] Kernel-mode #PF in user address space"
 * and the row that says what it actually did is never printed.
 *
 * 0 = both words delivered; -1 = the range is not wholly readable (hole,
 * guard, or a straddle past the end of its region).  Unit test:
 * test_l73_user_peek_hole.
 */
int kpf_user_peek2(struct process *p, uint64_t va, uint64_t out[2]) {
  out[0] = 0;
  out[1] = 0;
  if (!p || !va)
    return -1;
  return vm_user_read(p, va, out, 2 * (int)sizeof(uint64_t)) == 0 ? 0 : -1;
}

static void kpf_user_report(struct trap_frame *tf, struct process *p,
                            uint64_t cr2, const char *why) {
  struct addr_space *as = p ? p->as : 0;
  struct kline kl;

  kl_init(&kl);
  kl_s(&kl, "[KPF-SYS] cr2=");
  kl_hex(&kl, (unsigned long)cr2);
  kl_s(&kl, " err=");
  kl_i(&kl, (long)(tf->error_code & 7));
  kl_s(&kl, " why=");
  kl_s(&kl, why ? why : "?");
  kl_s(&kl, " rip=");
  kl_hex(&kl, (unsigned long)tf->elr);
  kl_s(&kl, " rsp=");
  kl_hex(&kl, (unsigned long)tf->lr);
  kl_s(&kl, " cpu=");
  kl_i(&kl, (long)get_cpuid());
  kl_s(&kl, " pid=");
  kl_i(&kl, (long)(p ? p->pid : -1));
  kl_s(&kl, " nr=");
  kl_i(&kl, (long)(p ? p->last_syscall : -1));
  kl_s(&kl, "\n");
  kl_emit(&kl);

  /* Which user code issued the syscall: sc_rip is the PC just past the
     `syscall` instruction, and its containing region names the DSO. */
  kl_init(&kl);
  kl_s(&kl, "[KPF-ARG] upc=");
  kl_hex(&kl, (unsigned long)(p ? p->sc_rip : 0));
  if (as && p) {
    struct vm_region *ur = vm_region_find(as, p->sc_rip);
    kl_s(&kl, " umod=");
    if (ur) {
      kl_hex(&kl, (unsigned long)ur->base);
      kl_s(&kl, "+");
      kl_hex(&kl, (unsigned long)(p->sc_rip - ur->base));
    } else {
      kl_s(&kl, "none");
    }
  }
  for (int i = 0; i < 3; i++) {
    kl_s(&kl, " a");
    kl_i(&kl, i);
    kl_s(&kl, "=");
    kl_hex(&kl, (unsigned long)(p ? p->sc_arg[i] : 0));
  }
  kl_s(&kl, "\n");
  kl_emit(&kl);

  kl_init(&kl);
  kl_s(&kl, "[KPF-AR2]");
  for (int i = 3; i < 6; i++) {
    kl_s(&kl, " a");
    kl_i(&kl, i);
    kl_s(&kl, "=");
    kl_hex(&kl, (unsigned long)(p ? p->sc_arg[i] : 0));
  }
  kl_s(&kl, " ring=");
  if (p)
    for (int i = 0; i < 8; i++) {
      int ix = (p->sys_ring_at - 1 - i) & 31;
      kl_i(&kl, (long)p->sys_ring[ix]);
      kl_s(&kl, " ");
    }
  kl_s(&kl, "\n");
  kl_emit(&kl);

  /* The mapping the address fell out of: cr2 is a hole, so find the region
     ENDING at or just below it.  base+len == cr2 is the "one past the end"
     shape of an over-long copy; a gap names the guard. */
  if (as) {
    struct vm_region *r = cr2 ? vm_region_find(as, cr2 - 1) : 0;
    kl_init(&kl);
    kl_s(&kl, "[KPF-RGN] below=");
    if (r) {
      kl_hex(&kl, (unsigned long)r->base);
      kl_s(&kl, "+");
      kl_hex(&kl, (unsigned long)r->len);
      kl_s(&kl, " end=");
      kl_hex(&kl, (unsigned long)(r->base + r->len));
      kl_s(&kl, " kind=");
      kl_i(&kl, (long)r->kind);
      kl_s(&kl, " prot=");
      kl_i(&kl, (long)r->prot);
      kl_s(&kl, " flags=");
      kl_hex(&kl, (unsigned long)r->flags);
    } else {
      kl_s(&kl, "none");
    }
    r = vm_region_above(as, cr2, 0x40000000ULL);
    kl_s(&kl, " above=");
    if (r) {
      kl_hex(&kl, (unsigned long)r->base);
      kl_s(&kl, " gap=");
      kl_hex(&kl, (unsigned long)(r->base - cr2));
      kl_s(&kl, " kind=");
      kl_i(&kl, (long)r->kind);
    } else {
      kl_s(&kl, "none");
    }
    kl_s(&kl, "\n");
    kl_emit(&kl);
  }

  /* The top 8 kernel return addresses at or above the faulting RSP.  The
     kernel text window is [0x70000000,0x70100000): the watchdog's older scan
     stops at 0x70080000 and would report nothing for a fault above it, which
     is where this kernel's .text actually ends. */
  kl_init(&kl);
  kl_s(&kl, "[KPF-STK]");
  int frames = 0;
  {
    uint64_t cpu = get_cpuid();
    uint64_t top = (cpu < MAX_CPUS) ? cpu_locals[cpu].kernel_stack : 0;
    uint64_t start = (uint64_t)tf->lr;
    if (top && start >= 0x70000000ULL && start < top) {
      for (uint64_t a = start; a < top && frames < 8; a += 8) {
        uint64_t v = *(volatile uint64_t *)a;
        if (v >= 0x70000000ULL && v < 0x70100000ULL) {
          kl_s(&kl, " ");
          kl_hex(&kl, (unsigned long)v);
          frames++;
        }
      }
    }
  }
  kl_s(&kl, " frames=");
  kl_i(&kl, (long)frames);
  kl_s(&kl, "\n");
  kl_emit(&kl);
}

void sync_lower_handler_c(struct trap_frame *tf) {
  /* GB-014: latch the user PC on every trap from user mode so the exit report
     can name where a program actually got to. A silently-stopping program
     otherwise reports its entry point as its last PC, because the PCB context
     is only saved on preemption, not as user code runs. */
  {
    struct process *lp0 = current_process();
    if (lp0 && !lp0->is_kernel_process && tf->elr >= 0x1000ULL)
      lp0->last_user_pc = tf->elr;
  }
  /* SEC-10 (security-plan.md #10): the kernel stacks are contiguous static
     64 KiB per-CPU slices — an overflow faults nowhere, it corrupts the
     adjacent core's stack or kernel data.  Refuse to run a handler when the
     slice is already below the deepest-path margin; halt loudly instead of
     continuing on a doomed stack. */
  if (kernel_stack_low(8u * 1024u)) {
    /* GB-057: this panic must be able to SPEAK FROM A DOOMED STACK.  The
       ordinary uart_puts takes uart_lock, and when two cores reach here at
       once (measured: CPU#3 and CPU#7 both at the 'jmp $' below) neither can
       print, so the machine halts in total silence -- which is why this shape
       went unidentified for a week.  Use the lock-free raw path, and name the
       syscall that overran: elr is the user PC that made the call, pid says
       which process, and the depth says how far past the margin we are. */
    char pb[160];
    int pi = 0;
    const char *pre = "[KPANIC] SEC-10 exhausted kernel stack at syscall entry: pid=";
    while (*pre && pi < 110) pb[pi++] = *pre++;
    {
      struct process *kp = current_process();
      int v = (kp ? (int)kp->pid : -1);
      if (v < 0) { pb[pi++] = '-'; v = -v; }
      char td[12];
      int ti = 0;
      if (v == 0) td[ti++] = '0';
      while (v > 0 && ti < 11) { td[ti++] = (char)('0' + (v % 10)); v /= 10; }
      while (ti > 0 && pi < 126) pb[pi++] = td[--ti];
    }
    const char *mid = " elr=0x";
    while (*mid && pi < 134) pb[pi++] = *mid++;
    {
      static const char hx[] = "0123456789ABCDEF";
      uint64_t v = tf->elr;
      for (int sh = 60; sh >= 0 && pi < 150; sh -= 4)
        pb[pi++] = hx[(v >> sh) & 0xF];
    }
    pb[pi++] = '\n';
    pb[pi] = 0;
    panic_puts(pb);
    for (;;)
      ;
  }

  /* L9 liveness (the x64 lane's proven scratch fix): a syscall entry proves
     this core is executing, so refresh the ownership heartbeat at entry.
     Timeout-driven loads (the 86 MB WebProcess) make the owner core spend
     seconds in user mode or IRQ-off on the console path between timer
     ticks; the idle reaper on another core must never treat that
     genuinely-running owner as a stale dead claim and dispose its process.
     The interrupt handler already refreshes on every IRQ (watchdog_tick);
     this closes the syscall-only, IRQ-starved window on the same clock.
     Writes only; process.c owns the array. */
  extern volatile uint64_t cpu_heartbeat_ms[];
  extern uint64_t timer_get_ms(void);
  cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();

  uint64_t syscall_num = tf->regs[0]; // rax
  core_in_syscall[get_cpuid()] = (int)syscall_num;
  {
    struct process *sc = current_process();
    if (sc) {
      sc->last_syscall = (int64_t)syscall_num;
      /* L73: the whole entry tuple, so a fault INSIDE this syscall can name
         the call and its arguments instead of guessing from a clobbered
         register set.  Same argument order as l_arg(): (rdi,rsi,rdx,r10,r8,r9)
         = regs[5],[4],[3],[9],[7],[8]. */
      {
        static const unsigned char aidx[6] = {5, 4, 3, 9, 7, 8};
        for (int ai = 0; ai < 6; ai++)
          sc->sc_arg[ai] = tf->regs[aidx[ai]];
        sc->sc_rip = tf->elr; /* user PC just past the `syscall` instruction */
      }
      if (sc->abi_flags & PROC_FLAG_LINUX_ABI) {
        sc->sys_ring[sc->sys_ring_at & 31] = (uint16_t)syscall_num;
        sc->sys_ring_at = (uint8_t)((sc->sys_ring_at + 1) & 31);
      }
    }
  }

  if (syscall_num == 0xFF) {
    schedule(tf, 1);
    return;
  }
  /* GB-014 (2026-10-21): route a Linux-ABI process by translating its syscall
     NUMBER to the HobbyOS number, then falling through to the existing handlers.
     Arguments need no shuffle: the archive layout is regs[0]=rax, [3]=rdx,
     [4]=rsi, [5]=rdi, so Linux's (rdi,rsi,rdx) already lands where the HobbyOS
     handlers read their first three arguments.
     This was the last joint in the ELF chain: PROC_FLAG_LINUX_ABI had six
     references in the whole tree and exactly one of them was an assignment, so
     nothing ever routed on it. A musl write(1,...) arrived as number 1 -- which
     collides with SYS_WRITE_CONSOLE, a different calling convention -- and exit
     arrived as 60, which matched nothing. Measured consequence: a static-pie
     probe ran, exited cleanly and printed nothing.
     Unknown Linux numbers return -ENOSYS (38) rather than falling through to a
     HobbyOS handler that would do something unrelated. */
  {
    struct process *lp = current_process();
    if (lp && (lp->abi_flags & PROC_FLAG_LINUX_ABI)) {
      /* Compatibility-table dispatch (docs/linux-abi.md, src/include/
         linux_compat.h): a subsystem handler that owns the number answers it
         with LINUX semantics and writes tf->regs[0] itself.  The switch below
         is the older number-translation path and stays as the fallback; a
         handler that returns 0 means "not mine", not "fail". */
      if (linux_compat_dispatch(lp, tf, syscall_num)) {
        /* Linux delivers a signal raised BY a syscall at syscall exit, so the
           frame the handler sees carries the syscall's return value in rax.
           Returning straight to user mode skipped the leader's return-to-user
           boundary below (the process_signal_trap_exit(tf) at the end of this
           function), which deferred such a signal to the next 100 Hz tick --
           measured by the proc lane as handler entry up to one tick late.  The
           hook is a no-op unless a deliverable handler signal is pending. */
        process_signal_trap_exit(tf);
        return;
      }
      switch (syscall_num) {
        case 0:  syscall_num = SYS_READ;  break;   /* read(fd, buf, n)   */
        case 1:  syscall_num = SYS_WRITE; break;   /* write(fd, buf, n)  */
        /* GB-014's arch_prctl(158) is l_thr_arch_prctl in l_tab_thr (see the
           note below): linux_compat_dispatch above claims 158 before this
           switch, so an inline copy here could never run and would only give
           thread state a second owner.
           open(2): GB-201 resolves a staged glibc SONAME to the 8.3 name the
           loader staged; every other path reaches file_open unchanged. */
        case 2:  compat_open(tf);         return;   /* open(path, fl, md) */
        case 3:  syscall_num = SYS_CLOSE; break;   /* close(fd)          */
        /* GB-165: lseek(8).  DOOM's W_AddFile seeks to the WAD directory; with
           no arm the seek returned -ENOSYS and every lump lookup failed
           ("W_GetNumForName: PNAMES not found!" at a WAD it had just opened).
           Linux and HobbyOS agree on (fd, offset, whence). */
        case 8:  syscall_num = SYS_LSEEK; break;   /* lseek(fd, off, wh) */
        /* GB-169: fsync(74) on an /dev/fb0 fd presents the mapped surface — the
           "commit" verb a framebuffer program needs, since the native path
           (SYS_FLUSH_FB) is not reachable from a Linux-ABI process.  Any other
           fd is a success no-op: there is no writeback cache to drain. */
        case 74: {
          extern int file_fd_is_devfb(struct process *, int);
          if (file_fd_is_devfb(current_process(), (int)tf->regs[5]))
            virtio_gpu_flush();
          tf->regs[0] = 0;
          return;
        }
        case 60: syscall_num = SYS_EXIT;  break;   /* exit(code)         */
        /* clone(56) / futex(202) / arch_prctl(158) / set_robust_list(273)
           live in l_tab_thr (src/kernel/linux_compat_thr.c) now: the M2
           implementation is a table like every other compat subsystem, and a
           second copy here would let the two disagree about thread state. */
        /* GB-021: musl's pthread plumbing touches these; an ENOSYS return
           pollutes the caller's errno even when the thread operation succeeds
           (measured: tls_main_errno_untouched=38). */
        case 186:                            /* gettid() -> this thread's pid */
          tf->regs[0] = (uint64_t)(int64_t)(current_process() ? current_process()->pid : -1);
          return;
        case 228: syscall_num = SYS_GETTIME; break; /* clock_gettime(clk,ts) */
        case 35:  compat_nanosleep(tf); return;    /* nanosleep(req, rem)   */
        case 230: compat_clock_nanosleep(tf); return; /* clock_nanosleep(clk,fl,req,rem) */
        case 100: {                          /* gettimeofday(timeval*, tz*) */
          /* M6/L43: CLOCK_REALTIME, not uptime (see l_core_gettimeofday; the
             same bug broke Mono's absolute sem_timedwait deadlines).  The
             timeval is 16 bytes on x86-64: tv_sec is a 64-bit long and so is
             tv_usec, so write both as 64-bit words. */
          uint64_t tv = tf->regs[5];
          extern uint64_t hb_clock_realtime_ms(void);
          uint64_t ms = hb_clock_realtime_ms();
          if (!tv) { tf->regs[0] = 0; return; }
          if (sys_user_range(tv, 16, 1) != 0) {
            tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
          }
          ((volatile uint64_t *)tv)[0] = ms / 1000;
          ((volatile uint64_t *)tv)[1] = (ms % 1000) * 1000;
          tf->regs[0] = 0;
          return;
        }
        case 24:  tf->regs[0] = 0; return;   /* sched_yield(): voluntary no-op */
        /* sched_getaffinity(204) / sched_setaffinity(203) used to live here as
           a raw 8-byte 0xFF fill.  They are TABLE rows now (l_tab_proc): the
           table version implements the 128-byte cpu_set_t shape, zeroes the
           caller's buffer, returns the byte count Linux returns, and answers
           setaffinity too.  Keeping a second copy here would let the two
           disagree. */
        /* GB-141 (V evidence): musl's _Exit calls exit_group(231), not exit(60)
           when the program has a thread/group — the compat table had NO 231, so it
           fell to the default -ENOSYS and the probe "died inside _Exit" (GB-113
           signature: exit-0 observed as a signal death, last_user_pc in _Exit),
           rows never flushed to write(1).  exit_group(code) with code in rdi. */
        case 231: syscall_num = SYS_EXIT;  break;  /* exit_group(code)  */
        /* GB-141 (V2 mechanism): musl stdio calls writev(20), not write — the compat
           table had no case 20, so every printf byte hit -ENOSYS. */
        case 20: compat_writev(tf); return;
        /* GB-169: the read half — musl's fread() is readv(19) with a two-segment
           iovec; without it a file opens but every buffered read fails, which is
           how DOOM rejected a WAD it had just opened. */
        case 19: compat_readv(tf); return;
        /* GB-020: getdents64(fd, dirp, count).  Linux number 217. */
        case 217: syscall_num = SYS_GETDENTS64; break;
        /* GB-019 clause 4: Linux fcntl(72)(fd, cmd, arg) SHARES file_fcntl with the
           ioctl(16) F_GETFL/F_SETFL dispatch.  cmd=3/4 (K_F_GETFL/K_F_SETFL are 3/4)
           route to file_fcntl; a non-3/4 cmd is file_fcntl's own decision.  regs:
           rdi=fd, rsi=cmd, rdx=arg. */
        case 72: syscall_num = SYS_FCNTL; break;   /* fcntl(fd, cmd, arg) */
        /* GB-016: uname(2), Linux number 63.  rdi = struct utsname *.
           Handled inline (no HobbyOS equivalent), and the identity itself comes
           from linux_fill_utsname() so the T2-pinned strings and the syscall
           can never drift apart. */
        case 63: sys_uname(tf); return;
        /* GB-018: the pipe/poll/select/pselect6/socketpair block the T4 probe
           measures.  Linux numbers: poll 7, pipe 22, select 23, socketpair 53,
           pselect6 270, pipe2 293, getrandom 318.  poll/socketpair/pipe share
           the HobbyOS argument order exactly (rdi/rsi/rdx/r10); select and
           pselect6 need the timeout POINTER converted to the millisecond int
           sys_select expects (compat_select/compat_pselect6).  pipe2 is mapped
           for its flags=0 form (musl's pipe() always uses pipe2(fd, 0)); a
           non-zero flags word is not applied yet and is recorded on GB-018. */
        case 7:   syscall_num = SYS_POLL;       break;  /* poll(fds, n, ms) */
        case 22:  syscall_num = SYS_PIPE;       break;  /* pipe(fds)        */
        case 23:  compat_select(tf);            return; /* select(..., tv*) */
        case 53:  syscall_num = SYS_SOCKETPAIR; break;  /* socketpair(...)  */
        case 270: compat_pselect6(tf);          return; /* pselect6(..., ts*) */
        case 293: syscall_num = SYS_PIPE;       break;  /* pipe2(fds, 0)    */
        case 318: syscall_num = SYS_GETRANDOM;  break;  /* getrandom(...)   */
        /* GB-183 (rebase of PR #255): the ten Linux-ABI numbers main still
           lacked after GB-141/149/164/165/169/178 landed the rest.  Each
           native handler already speaks this argument layout -- x64 syscall
           args land rdi->regs[5], rsi->regs[4], rdx->regs[3], r10->regs[9] --
           and returns -errno, so these are number translations, not shims.
           The three that are not a bare register shuffle are called out:
             fork(57): no args.  process_fork() returns the child pid in the
               parent and 0 in the child (child->context[0] = 0), -EAGAIN on
               failure -- exactly Linux fork semantics.
             execve(59): rdi=path, rsi=argv, rdx=envp.  sys_exec() marshals
               rdx into the group env blob (P5 D3.1/D3.5) before the load, so
               the Linux layout maps 1:1; failure returns -errno.
             wait4(61): rdi=pid, rsi=status, rdx=options, r10=rusage.
               process_wait_args() takes all four (rusage in regs[9]) and
               fills the 144-byte rusage on reap, so the order matches.
           dup/dup2/getpid/getppid/kill/getcwd/chdir are the plain
           (fd)/(old,new)/()/()/(pid,sig)/(buf,size)/(path) shapes. */
        case 32:  syscall_num = SYS_DUP;       break;  /* dup(fd)              */
        case 33:  syscall_num = SYS_DUP2;      break;  /* dup2(oldfd, newfd)   */
        case 39:  syscall_num = SYS_GETPID;    break;  /* getpid()             */
        case 57:  syscall_num = SYS_FORK;      break;  /* fork()               */
        case 59:  syscall_num = SYS_EXEC;      break;  /* execve(path,argv,envp) */
        case 61:  syscall_num = SYS_WAIT4;     break;  /* wait4(pid,st,opt,ru) */
        case 62:  syscall_num = SYS_KILL;      break;  /* kill(pid, sig)       */
        case 79:  syscall_num = SYS_GETCWD;    break;  /* getcwd(buf, size)    */
        case 80:  syscall_num = SYS_CHDIR;     break;  /* chdir(path)          */
        case 110: syscall_num = SYS_GETPPID;   break;  /* getppid()            */
        /* GB-148: mkdir(83) is (path, mode) = rdi, rsi — SYS_MKDIR takes rdi.
           mkdirat(258) is (dirfd, path, mode); musl uses it for BOTH mkdir and
           mkdirat, so the dent block's mkdir() was -ENOSYS without this. */
        case 83:  syscall_num = SYS_MKDIR;      break;  /* mkdir(path, mode) */
        case 258: compat_mkdirat(tf);           return; /* mkdirat(dirfd, path, mode) */
        /* GB-178: unlink(87) was not in the compat table at all, so the
           bad-pointer probe saw -ENOSYS instead of -EFAULT.  rdi=path. */
        case 87:  syscall_num = SYS_UNLINK;     break;  /* unlink(path)      */
        /* GB-164: stat family.  musl's stat()/lstat() go through newfstatat(262)
           with AT_FDCWD and fstat is 5; with no arm the caller gets -ENOSYS and
           a file it can open still has no size (DOOMW4's IWAD search). */
        case 4:   compat_stat_path(tf);         return; /* stat(path, buf)  */
        case 6:   compat_stat_path(tf);         return; /* lstat (FAT: same) */
        case 5:   compat_fstat(tf);             return; /* fstat(fd, buf)   */
        case 262: compat_newfstatat(tf);        return; /* newfstatat(...)  */
        /* GB-165: openat(257) — the clang/musl-target build of DOOMW4 uses it
           for every fopen; without an arm its IWAD search could not open the
           file that `open(2)` was happy to serve. */
        case 257: compat_openat(tf);            return; /* openat(...)      */
        /* GB-149: brk/mmap for the musl allocator.  SYS_MMAP is already the
           6-arg Linux shape (sys_mmap6); SYS_BRK takes rdi.  Mapped here to
           measure the guest's heap setup before committing to a fix. */
        case 9:   syscall_num = SYS_MMAP;       break;  /* mmap(...)        */
        case 11:  syscall_num = SYS_MUNMAP;     break;  /* munmap(addr,len) */
        case 10:  syscall_num = SYS_MPROTECT;   break;  /* mprotect(...)    */
        case 12:  syscall_num = SYS_BRK;        break;  /* brk(addr)        */
        /* GB-019 (ioctl subset, real request dispatch).  Linux ioctl(16): regs[5]=rdi
           =fd, regs[4]=rsi=request, regs[3]=rdx=arg.  This is a genuine dispatch on
           the request number, and every non-ENOTTY outcome is falsifiable:
           - F_GETFL (3) / F_SETFL (4): route to file_fcntl (K_F_GETFL/SETFL are the
             same Linux numbers, K_F_GETFL=3, K_F_SETFL=4), which returns the fd's
             flags / applies O_APPEND etc — real, mutable state. Mis-routing these two
             changes the result, so the gate's falsifier has a mapping to break.
           - TCGETS (0x5401) / TIOCGWINSZ (0x5413): a tty-class request.  It takes a
             struct arg; a bad pointer is -EFAULT.  The compat O0 surface's nodes are
             not ttys (/dev/null is the probe's deterministic source), so Linux answers
             ENOTTY (25) here — honestly, after validating the arg, not by skipping it.
           - any other request: ENOTTY (the node does not implement it).
           - bad fd: EBADF. */
        case 16: {
          struct process *c16 = current_process();
          int cfd = (int)tf->regs[5];         /* rdi: fd      */
          int32_t ireq = (int32_t)tf->regs[4]; /* rsi: request */
          uint64_t iarg = tf->regs[3];         /* rdx: arg     */
          if (!c16 || cfd < 0 || cfd >= MAX_OPEN_FDS) { tf->regs[0] = (uint64_t)(int64_t)(-9); return; } /* EBADF */
          if (ireq == 3 || ireq == 4) {  /* F_GETFL(3) / F_SETFL(4) → route to the real fcntl handler */
            tf->regs[0] = (uint64_t)file_fcntl(c16, cfd, ireq, iarg);
            return;
          }
          if (ireq == 0x5401 || ireq == 0x5413) {  /* TCGETS / TIOCGWINSZ (measured: linux_syscall.h L_TCGETS/L_TIOCGWINSZ) */
            /* GB-019 (V3 ordering): Linux answers ENOTTY for a tty-class ioctl on a
               NON-tty WITHOUT dereferencing the arg — the pointer is never read, so a
               NULL/garbage arg is still ENOTTY, not EFAULT.  The compat surface has no
               tty (the probe's /dev/null is the deterministic source), so the honest
               answer is ENOTTY regardless of iarg; validating the pointer first made a
               non-tty return EFAULT and failed the gate's clause-2 rows. */
            tf->regs[0] = (uint64_t)(int64_t)(-25);     /* -ENOTTY */
            return;
          }
          /* GB-167: the FBIO family on /dev/fb0.  Only an fb node answers these;
             any other fd keeps the honest ENOTTY.  sys_fb_ioctl() takes its
             request/arg in rdi/rsi, so shuffle the Linux (fd, req, arg) into it. */
          if (ireq == 0x4600 || ireq == 0x4601 || ireq == 0x4611) {
            extern int file_fd_is_devfb(struct process *, int);
            if (file_fd_is_devfb(c16, cfd)) {
              tf->regs[5] = (uint64_t)(int64_t)ireq;
              tf->regs[4] = iarg;
              sys_fb_ioctl(tf);
              return;
            }
            tf->regs[0] = (uint64_t)(int64_t)(-25);     /* -ENOTTY: not an fb node */
            return;
          }
          tf->regs[0] = (uint64_t)(int64_t)(-25);       /* -ENOTTY: node lacks this request */
          return;
        }
        default:
          /* Nothing owns this number: name it on the console once, then answer
             -ENOSYS as Linux does.  Bring-up of a real Linux program is this
             line plus the next handler, never a guess. */
          linux_compat_unhandled(lp, syscall_num, tf->elr, tf->regs[5]);
          tf->regs[0] = (uint64_t)(int64_t)(-38);  /* -ENOSYS */
          return;
      }
    }
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
  } else if (syscall_num == SYS_SPAWN_EX) {
    sys_spawn_ex(tf);
  } else if (syscall_num == SYS_MAP_FB) {
    sys_map_fb(tf);
  } else if (syscall_num == SYS_FLUSH_FB) {
    sys_flush_fb(tf);
  } else if (syscall_num == SYS_FLUSH_FB_RECTS) {
    sys_flush_fb_rects(tf);
  } else if (syscall_num == SYS_FB_IOCTL) {
    sys_fb_ioctl(tf);
  } else if (syscall_num == SYS_GET_CPUID) {
    sys_get_cpuid(tf);
  } else if (syscall_num == SYS_PIPE) {
    sys_pipe(tf);
  } else if (syscall_num == SYS_GET_EVENTS) {
    sys_get_events(tf);
  } else if (syscall_num == SYS_INPUT_IOCTL) {
    sys_input_ioctl(tf);
  } else if (syscall_num == SYS_INPUT_READ) {
    sys_input_read(tf);
  } else if (syscall_num == SYS_EVDEV_PUSH) {
    /* GB-173: desktop -> owner of /dev/input/event0 (rdi=code, rsi=value). */
    extern int hb_evdev_push(unsigned short, int);
    tf->regs[0] = (uint64_t)(int64_t)hb_evdev_push((unsigned short)tf->regs[5],
                                                   (int)tf->regs[4]);
  } else if (syscall_num == SYS_AVAILABLE) {
    sys_available(tf);
  } else if (syscall_num == SYS_READ_DIR) {
    sys_read_dir(tf);
  } else if (syscall_num == SYS_GETDENTS64) {
    sys_getdents64(tf);
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
  } else if (syscall_num == SYS_RMDIR) {   /* W2-14 (selfhost) */
    sys_rmdir(tf);
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
  } else if (syscall_num == SYS_FTRUNCATE) {
    sys_ftruncate(tf);
  } else if (syscall_num == SYS_MOUNT) {
    sys_mount(tf);
  } else if (syscall_num == SYS_VFS_BIND) {
    sys_vfs_bind(tf);
  } else if (syscall_num == SYS_UMOUNT) {
    sys_umount(tf);
  } else if (syscall_num == SYS_GETPID) {
    sys_getpid(tf);
  } else if (syscall_num == SYS_GETPPID) {
    sys_getppid(tf);
  } else if (syscall_num == SYS_WAITPID) {
    sys_waitpid(tf);
  } else if (syscall_num == SYS_WAIT4) {    /* W2-5 (selfhost) */
    sys_wait4(tf);
  } else if (syscall_num == SYS_EXEC) {
    sys_exec(tf);
  } else if (syscall_num == SYS_GETPROGNAME) {
    sys_get_progname(tf);
  } else if (syscall_num == SYS_GETARGV) {
    /* WX: pre-commit the user argv buffer before sys_readargv's direct
       store — the same write-validate guard SYS_GETENV uses.  The argv
       destination can sit in the not-yet-resident tail of a lazily
       demand-loaded image (observed: the WK-3 WEBPROC.BIN argv buffer at
       0x1005FD23B0 faulting in sys_readargv, RIP 0x70020EB6): without
       this, the kernel-mode store #PFs and the trap handler prints a
       "FATAL" dump (the L9 materialize path then recovers it, but the
       dump is alarming and serial noise on every x64 webproc boot). */
    struct process *av = current_process();
    char *avbuf = (char *)tf->regs[4];       /* rsi: buf */
    int avsz = (int)tf->regs[3];             /* rdx: size */
    if ((int)tf->regs[5] >= 0 && avsz > 0 && /* rdi: idx; writes only then */
        (!av || process_user_ok(av, (uint64_t)avbuf, (uint64_t)avsz, 1) != 0)) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    } else {
      tf->regs[0] = (uint64_t)sys_readargv(av, (int)tf->regs[5], avbuf, avsz);
    }
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
  } else if (syscall_num == SYS_GETTIME) {   /* L8 ABI ext (87) */
    sys_gettime(tf);
  } else if (syscall_num == SYS_GETSTACK) {  /* L8 ABI ext (88) */
    sys_getstack(tf);
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
    process_sigreturn(tf);
  } else if (syscall_num == SYS_GETENV) {
    sys_getenv(tf);
  } else if (syscall_num == SYS_POLL) {
    sys_poll(tf);
  } else if (syscall_num == SYS_SOCKETPAIR) {
    sys_socketpair(tf);
  } else if (syscall_num == SYS_SENDMSG) {
    sys_sendmsg(tf);
  } else if (syscall_num == SYS_RECVMSG) {
    sys_recvmsg(tf);
  } else if (syscall_num == SYS_BIND) {         /* IceWM M1 (96-102) */
    sys_bind(tf);
  } else if (syscall_num == SYS_LISTEN) {
    sys_listen(tf);
  } else if (syscall_num == SYS_ACCEPT4) {
    sys_accept4(tf);
  } else if (syscall_num == SYS_CONNECT_ADDR) {
    sys_connect_addr(tf);
  } else if (syscall_num == SYS_GETSOCKNAME) {
    sys_getsockname(tf);
  } else if (syscall_num == SYS_GETPEERNAME) {
    sys_getpeername(tf);
  } else if (syscall_num == SYS_SHUTDOWN) {
    sys_shutdown(tf);
  } else if (syscall_num == SYS_VDISPLAY) {
    sys_vdisplay(tf);
  } else {
    uart_puts("Unknown System Call Invoked!\n");
    tf->regs[0] = -ENOSYS;
  }
  {
    struct process *sc = current_process();
    if (sc)
      sc->last_ret = (int64_t)tf->regs[0];
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
static volatile uint64_t wd_last_seen_rflags[MAX_CPUS];
static volatile uint64_t wd_last_vector[MAX_CPUS];
static volatile uint64_t wd_last_err[MAX_CPUS];
static volatile uint64_t wd_last_cr2[MAX_CPUS];
static volatile int wd_last_seen_pid[MAX_CPUS];
static volatile uint64_t wd_next_report_ms = 0;
/* L41: served by the scheduler's idle loop (process.c), which can take
   proc_lock safely; the timer IRQ only increments this.  A park that never
   produces a 20000-round idle stretch (the desktop polls and resets the
   counter) still gets its thread table this way. */
volatile int wd_dump_request = 0;
/* Full last frame per core (copied at every tick) so the stall dump can
   show the register state + live stack pointer of every core, not just the
   interrupted RIP.  Key offsets (see save_context / the asm wrapper): the
   frame mirrors the push order of the interrupt wrapper — for x64 the
   generic layout is signed-off in trap.S; we record the whole struct. */
static struct trap_frame wd_last_frame[MAX_CPUS];
static volatile uint64_t wd_last_tfp[MAX_CPUS]; /* stack addr of that frame */
static volatile uint64_t wd_ticks[MAX_CPUS];     /* interrupt count per core */

/* Is `top` a plausible kernel-stack address for the watchdog's caller-chain
   scans?  Cores that never powered on leave cpu_locals[c].kernel_stack == 0
   (or stale), and scanning downward from (top - 8) would dereference
   negative addresses and fault the watchdog itself (x64 KVM Vector 14 at
   CR2 = -8 while a long v2 load keeps the console silent).  Mirrors the
   guard already applied to wd_last_tfp below. */
int watchdog_stack_scan_ok(uint64_t top);
void watchdog_stack_arena(uint64_t *lo, uint64_t *hi);

/* GB-057: written by the CPU itself, immediately before enter_user_space, so the
   watchdog can compare "what this core is resuming" against the trap frame it
   last recorded.  Observation only. */
volatile int64_t resume_mark_pid[MAX_CPUS];
volatile uint64_t resume_mark_sp[MAX_CPUS];

void resume_mark(int pid, uint64_t target_sp) {
  uint32_t c = get_cpuid();
  if (c < MAX_CPUS) {
    resume_mark_pid[c] = pid;
    resume_mark_sp[c] = target_sp;
  }
}

int watchdog_stack_scan_ok(uint64_t top) {
  /* GB-057: the old bounds were hardcoded and BOTH were wrong, so this scan
     never once ran.  Measured on this build:
       __stack_top      = 0x755ffbf0     -> guard demanded top < 0x75000000
       last .text addr  = 0x70042cc0     -> scan demanded v   < 0x7001c000
     i.e. every CPU's stack was rejected as "uninitialized" while it was
     perfectly initialised, and had the guard passed, the frame filter would
     have discarded every return address past 0x1c000.  Derive the arena from
     the linker symbol instead of guessing it, so a layout change cannot make
     the instrument silently blind again. */
  uint64_t lo, hi;
  watchdog_stack_arena(&lo, &hi);
  if (top == 0)
    return 0; /* core never powered on */
  return top <= hi && top >= lo;
}

/* The arena, in one place, so the unit test can assert the guard's contract
   (accept the arena, reject its neighbours) instead of hardcoding addresses
   that a linker-script change invalidates -- which is exactly how the previous
   bounds became wrong and stayed wrong. */
void watchdog_stack_arena(uint64_t *lo, uint64_t *hi) {
  extern uint64_t __stack_top;
  uint64_t top = (uint64_t)&__stack_top;
  *hi = top;
  *lo = top - (uint64_t)(MAX_CPUS * 64 * 1024 + 4096);
}

/* GB-057: is this VA present in the CURRENT page tables?  A direct walk, no side
   effects -- unlike vm_range_ok/vm_touch, which materialise the page and would
   destroy the very measurement we want.  The kernel direct-maps physical memory,
   so a page-table frame address is usable as a pointer. */
int kpml_present(uint64_t va) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  uint64_t *pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
  uint64_t e = pml4[(va >> 39) & 511];
  if (!(e & 1))
    return 0;
  uint64_t *pdpt = (uint64_t *)(e & ~0xFFFULL);
  e = pdpt[(va >> 30) & 511];
  if (!(e & 1))
    return 0;
  if (e & (1ULL << 7))
    return 1; /* 1 GiB page */
  uint64_t *pd = (uint64_t *)(e & ~0xFFFULL);
  e = pd[(va >> 21) & 511];
  if (!(e & 1))
    return 0;
  if (e & (1ULL << 7))
    return 1; /* 2 MiB page */
  uint64_t *pt = (uint64_t *)(e & ~0xFFFULL);
  e = pt[(va >> 12) & 511];
  return (e & 1) ? 1 : 0;
}

static void watchdog_tick(uint32_t cpu, struct trap_frame *tf) {
  extern volatile uint64_t uart_last_activity_ms;
  extern volatile uint64_t lock_wait_addr[MAX_CPUS];
  extern volatile uint64_t lock_wait_caller[MAX_CPUS];
  extern volatile uint64_t lock_wait_caller2[MAX_CPUS];
  /* Stall-watchdog prints must be able to diagnose a wedge in uart_lock
     itself, so they go out through the lock-free raw sink. */
  extern void panic_puts(const char *s);
  extern void wd_hex(uint64_t v);
  extern void wd_int(int val);
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
  /* GB-057: RFLAGS at the last tick.  A core that stops taking timer interrupts
     while sitting in kernel asm cannot be diagnosed from RIP alone: if IF is
     clear here, the timer could not have preempted it and 'stuck in the frame
     copy' is a scheduling/masking defect, not a memory one.  Measured (intel,
     parked cores) the only way to tell the two apart without a monitor. */
  wd_last_seen_rflags[cpu] = tf->spsr;
  /* GB-057: the fault that put this core where it is.  A parked core whose
     RFLAGS carries RF (0x10000) is being RE-RUN after a fault, and RIP alone
     cannot say which fault: measured rflags=0x10046 on three parked cores with
     IF clear, i.e. a retry loop with interrupts masked.  Vector/error/CR2 name
     the class without another hypothesis. */
  wd_last_vector[cpu] = tf->vector;
  wd_last_err[cpu] = tf->error_code;
  if (tf->vector == 14) {
    uint64_t cr2v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2v));
    wd_last_cr2[cpu] = cr2v;
  }
  wd_last_seen_pid[cpu] = cur ? cur->pid : -1;
  wd_last_frame[cpu] = *tf;
  wd_last_tfp[cpu] = (uint64_t)tf;
  wd_ticks[cpu]++;

  if (now < wd_next_report_ms) {
    return;
  }
  /* L34: a launch that is alive but silent is not necessarily stuck -- the
   * game writes its log to a file, so the console says nothing for minutes
   * while it grinds through NFS reads.  A periodic counter row (30 s) is the
   * difference between "hung" and "reading at 17 KiB/s".  It runs before the
   * silence gate and does not reset it. */
  {
    static uint64_t next_nfsio_ms;
    if (now >= next_nfsio_ms) {
      extern volatile uint64_t nfs_io_bytes, nfs_io_ops;
      next_nfsio_ms = now + 30000;
      wd_puts("[NFSIO] bytes=0x");
      wd_hex(nfs_io_bytes);
      wd_puts(" rpcs=0x");
      wd_hex(nfs_io_ops);
      wd_puts(" ms=0x");
      wd_hex(now);
      wd_puts("\n");
    }
  }
  if (now - uart_last_activity_ms < WATCHDOG_SILENCE_MS) {
    return;
  }
  wd_next_report_ms = now + WATCHDOG_COOLDOWN_MS;
  wd_puts("[WATCHDOG] console silent for more than 20s\n");
  wd_dump_request++;
  {   /* L34: is the launch making progress at all, and at what rate? */
    extern volatile uint64_t nfs_io_bytes, nfs_io_ops;
    wd_puts("  [NFSIO] bytes=0x");
    wd_hex(nfs_io_bytes);
    wd_puts(" rpcs=0x");
    wd_hex(nfs_io_ops);
    wd_puts(" uptime_ms=0x");
    wd_hex(now);
    wd_puts("\n");
  }
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    wd_puts("  CPU");
    wd_int((int)c);
    wd_puts(" rip=");
    wd_hex(wd_last_seen_rip[c]);
    wd_puts(" rflags=");
    wd_hex(wd_last_seen_rflags[c]);
    if (wd_last_seen_pid[c] >= 0 && wd_last_vector[c] != 0) {
      wd_puts(" vec=");
      wd_int((int)wd_last_vector[c]);
      wd_puts(" err=");
      wd_hex(wd_last_err[c]);
      if (wd_last_cr2[c]) {
        wd_puts(" cr2=");
        wd_hex(wd_last_cr2[c]);
      }
    }
    wd_puts(" pid=");
    wd_int(wd_last_seen_pid[c]);
    /* GB-057: the resume mark the CPU itself wrote immediately before
       enter_user_space.  If this names a different pid than the watchdog's
       `pid=`, the RIP above is a stale trap frame and every "parked in the
       frame copy" conclusion drawn from it is an artefact. */
    if (resume_mark_pid[c] != 0) {
      wd_puts(" resume_pid=");
      wd_int((int)resume_mark_pid[c]);
      wd_puts(" resume_sp=");
      wd_hex(resume_mark_sp[c]);
    }
    wd_puts(" age_ms=");
    wd_int((int)(now - wd_last_seen_ms[c]));
    if (core_in_syscall[c] != 0) {
      wd_puts(" syscall=");
      wd_int(core_in_syscall[c]);
    }
    if (lock_wait_addr[c] != 0) {
      wd_puts(" wait_lock=");
      wd_hex(lock_wait_addr[c]);
      wd_puts(" caller=");
      wd_hex(lock_wait_caller[c]);
      if (lock_wait_caller2[c] != 0) {
        wd_puts(" caller2=");
        wd_hex(lock_wait_caller2[c]);
      }
    }
    wd_puts("\n");
  }

  /* Same dump on COM2 — a side-channel that never interleaves with the
     console, so the stall forensics come out byte-exact. */
  extern void panic_puts2(const char *s);
  extern void panic_print_hex2(uint64_t v);
  extern void print_int_raw2(int val);
  extern void wd_dump_proc_table(void);
  panic_puts2("[WD2] stall dump\n");
  wd_dump_proc_table();
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    panic_puts2(" CPU");
    print_int_raw2((int)c);
    panic_puts2(" rip=");
    panic_print_hex2(wd_last_seen_rip[c]);
    panic_puts2(" pid=");
    print_int_raw2(wd_last_seen_pid[c]);
    panic_puts2(" tks=");
    print_int_raw2((int)wd_ticks[c]);
    panic_puts2(" cs=");
    panic_print_hex2(wd_last_frame[c].cs);
    panic_puts2(" fl=");
    panic_print_hex2(wd_last_frame[c].spsr);
    panic_puts2(" age=");
    print_int_raw2((int)(now - wd_last_seen_ms[c]));
    panic_puts2(" vec=");
    print_int_raw2((int)wd_last_frame[c].vector);
    if (lock_wait_addr[c] != 0) {
      panic_puts2(" wl=");
      panic_print_hex2(lock_wait_addr[c]);
      panic_puts2(" cl=");
      panic_print_hex2(lock_wait_caller[c]);
      if (lock_wait_caller2[c] != 0) {
        panic_puts2(" c2=");
        panic_print_hex2(lock_wait_caller2[c]);
      }
    }
    panic_puts2("\n");
    /* Full frame regs from the stored last frame (the mapping of
       regs[0..9] := rax..r9 as pushed by the trap wrapper). */
    panic_puts2("   fr:");
    for (int r = 0; r < 10; r++) {
      panic_puts2(" ");
      panic_print_hex2(wd_last_frame[c].regs[r]);
    }
    panic_puts2("\n");
    /* The last interrupt's kernel stack region (below the frame) — the
       caller chain of what the core was actually executing. */
    uint64_t frp = wd_last_tfp[c];
    panic_puts2("  deep:");
    if (frp >= 0x70000000ULL && frp < 0x75000000ULL) {
      for (uint64_t a = frp + 8; a < frp + 8 + 96; a += 8) {
        uint64_t v = *(volatile uint64_t *)a;
        panic_puts2(" ");
        panic_print_hex2(v);
      }
    }
    panic_puts2("\n");
    /* Full stack chain: every .text return address, deepest first.
       Guard against cores that never powered on (kernel_stack == 0): the
       loop start (top - 8) would dereference negative addresses and fault
       the very watchdog that is supposed to report the stall. */
    uint64_t top = cpu_locals[c].kernel_stack;
    int hits = 0;
    panic_puts2("  stk");
    print_int_raw2((int)c);
    panic_puts2(":");
    if (watchdog_stack_scan_ok(top)) {
      for (uint64_t a = top - 8; a > (top - 24576) && hits < 20; a -= 8) {
        uint64_t v = *(volatile uint64_t *)a;
        if (v >= 0x70000000ULL && v < 0x70080000ULL) {
          panic_puts2(" ");
          panic_print_hex2(v);
          hits++;
        }
      }
    }
    panic_puts2("\n");
  }

  /* Deep-freeze forensics: for any core silent >60s, scan its kernel stack for
     return addresses into .text ([0x70000000,0x7001c000)).  The lowest hit
     approximates the deepest (newest) frame = where the core sits right now. */
  for (uint32_t c = 1; c < MAX_CPUS; c++) {
    if (now - wd_last_seen_ms[c] < 60000) continue;
    uint64_t top = cpu_locals[c].kernel_stack;
    /* Same guard as the WD2 stall dump: a core that never powered on has
       kernel_stack == 0, and the scan below would read (top - 8) = -8. */
    if (!watchdog_stack_scan_ok(top)) {
      wd_puts("  CSTK cpu");
      wd_int((int)c);
      wd_puts(": (uninitialized kstack, skip)\n");
      continue;
    }
    uint64_t last_hit = 0;
    int hits = 0;
    wd_puts("  CSTK cpu");
    wd_int((int)c);
    wd_puts(":");
    for (uint64_t a = top - 8; a > (top - 24576) && hits < 16; a -= 8) {
      uint64_t v = *(volatile uint64_t *)a;
      if (v >= 0x70000000ULL && v < 0x70080000ULL) {
        wd_puts(" ");
        wd_hex(v);
        last_hit = a;
        hits++;
      }
    }
    wd_puts("\n");
    if (last_hit) {
      wd_puts("  CDATA cpu");
      wd_int((int)c);
      wd_puts(" cur~=");
      wd_hex(last_hit);
      wd_puts(":");
      for (int q = 0; q < 12; q++) {
        wd_puts(" ");
        wd_hex(*(volatile uint64_t *)(last_hit + 8 * q));
      }
      wd_puts("\n");
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

/* GB-068 (2026-10-19): a fault record must be ONE write.
   `uart_puts` takes uart_lock for the whole string, so assembling the line in
   a buffer and emitting it once makes the record atomic on the wire.  The
   previous ~15 separate uart_puts/uart_print_hex calls each took and released
   the lock, so under 8 CPUs the record interleaved at every call boundary and
   ~43% of fault lines were unparseable -- which is how several wrong readings
   of this fault survived: the evidence itself was being shredded. */
struct fl_buf { char b[704]; int n; };
static void fl_s(struct fl_buf *f, const char *s2) {
  while (*s2 && f->n < (int)sizeof f->b - 1) f->b[f->n++] = *s2++;
}
static void fl_c(struct fl_buf *f, char c) {
  if (f->n < (int)sizeof f->b - 1) f->b[f->n++] = c;
}
static void fl_hex(struct fl_buf *f, uint64_t v) {
  static const char hx[] = "0123456789ABCDEF";
  char t[16];
  int k = 0, i2;
  if (!v) { fl_s(f, "0x0000000000000000"); return; }
  while (v && k < 16) { t[k++] = hx[v & 0xF]; v >>= 4; }
  fl_s(f, "0x");
  for (i2 = 0; i2 < 16 - k; i2++) fl_c(f, '0');
  while (k) fl_c(f, t[--k]);
}
static void fl_dec(struct fl_buf *f, int v) {
  char t[12];
  int k = 0;
  if (v < 0) { fl_c(f, '-'); v = -v; }
  if (!v) { fl_c(f, '0'); return; }
  while (v && k < 11) { t[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) fl_c(f, t[--k]);
}

/* M6/L37: the kill path used to print rbx/rdi/rsi/rbp only.  That set cannot
 * tell "a pointer was computed from a clobbered STACK SLOT" from "a wild
 * write": the source of a skewed index is a stack qword, so the record has to
 * carry the stack.  This dump is the evidence format the launch log quotes:
 * every GPR, the interrupted RSP/RIP, 48 qwords spanning
 * [rsp-0x100, rsp+0x80), and the region rows for rsp / cr2 / the 0x2FF.. probe
 * window.  Rate-limited to the first four faults of a boot; one uart_puts per
 * line so SMP cannot shred it (rule: a fault record must be one write).
 */
static void fl_qwords(struct fl_buf *f, struct process *p, uint64_t va, int n) {
  uint64_t q[16];
  if (n > 16) n = 16;
  if (vm_user_read(p, va, q, n * 8) != 0) {
    fl_s(f, " <unreadable>");
    return;
  }
  for (int i = 0; i < n; i++) {
    fl_c(f, ' ');
    fl_hex(f, q[i]);
  }
}

#ifdef RW_SC_RING
/* L85: print the last SC_HIST_N complete compat syscalls of the faulting
 * thread, oldest first.  One line per call so a single uart_puts (and one
 * uart_lock hold) carries the record: a fault record must never interleave.
 *
 * `mod=` resolves the issuing user PC against the process's own regions, so a
 * call made from libc's read wrapper reads `mod=<libc base>+off` and one made
 * from UnityPlayer reads `mod=<UnityPlayer base>+off`.  The bases are the same
 * ones [SIGDUMP] mods= lists in the launcher's dump.
 */
static void sc_hist_dump(struct process *cur) {
  if (!cur)
    return;
  struct fl_buf f;
  f.n = 0;
  fl_s(&f, "[SCH] pid=");
  fl_dec(&f, cur->pid);
  fl_s(&f, " tgid=");
  fl_dec(&f, cur->tgid);
  fl_s(&f, " nm=");
  fl_s(&f, cur->name);
  fl_s(&f, " ring=");
  fl_dec(&f, SC_HIST_N);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);
  for (int i = 0; i < SC_HIST_N; i++) {
    int ix = (cur->sc_hist_at - SC_HIST_N + i) & (SC_HIST_N - 1);
    struct sc_hist_ent *h = &cur->sc_hist[ix];
    if (h->nr == 0 && h->rip == 0)
      continue; /* never written */
    f.n = 0;
    fl_s(&f, "[SCH] ");
    fl_dec(&f, i - (SC_HIST_N - 1)); /* -23 .. 0, 0 = the newest */
    fl_s(&f, " nr=");
    fl_dec(&f, (int)h->nr);
    fl_s(&f, h->done ? " done" : " INFLIGHT");
    for (int a = 0; a < 4; a++) {
      fl_s(&f, " a");
      fl_dec(&f, a);
      fl_c(&f, '=');
      fl_hex(&f, h->arg[a]);
    }
    fl_s(&f, " -> ");
    fl_hex(&f, (uint64_t)h->ret);
    if (h->ret < 0) {
      fl_s(&f, "(");
      fl_dec(&f, (int)(-h->ret));
      fl_c(&f, ')');
    }
    fl_s(&f, " rip=");
    fl_hex(&f, h->rip);
    if (cur->as) {
      extern struct vm_region *vm_region_find(struct addr_space *as,
                                             uint64_t va);
      struct vm_region *r = vm_region_find(cur->as, h->rip);
      if (r) {
        fl_s(&f, " mod=");
        fl_hex(&f, r->base);
        fl_c(&f, '+');
        fl_hex(&f, h->rip - r->base);
      } else {
        fl_s(&f, " mod=NONE");
      }
    }
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
  }
}
#endif

static void compat_fault_dump(struct process *cur, struct trap_frame *tf,
                              uint64_t cr2) {
  static int fd_rows;
  if (fd_rows >= 4)
    return;
  fd_rows++;
#ifdef RW_SC_RING
  /* L85: two rows that name a small-integer fault address.
   *
   * (1) [SLEAF] asks the page tables whether cr2 is present.  The fault row's
   *     `err` bits are the hardware's answer; region=NONE is the region
   *     table's.  `present=1` with `region=NONE` means a live translation with
   *     no mapping behind it (unmap raced a running thread, or the address
   *     space was torn down under it), while `present=0` means the program
   *     simply computed an address nothing ever mapped.
   *
   * (2) [SALIAS] tests the truncation hypothesis directly: for every region,
   *     compare cr2 with the low 32 bits of [base, base+len).  A hit prints
   *     the region and the offset, i.e. "cr2 IS the low half of an address
   *     inside this mapping" -- the measured shape of a 32-bit truncation of
   *     a valid heap pointer (0x11502F992F -> 0x2F992F).
   *
   * (3) [SRGN] lists the regions (bounded) so the dump can be read offline
   *     without the address space.
   */
  {
    uint64_t leaf = 0;
    int have = (cur->as && vm_arch_walk(cur->as, cr2, &leaf) == 0);
    struct fl_buf f2;
    f2.n = 0;
    fl_s(&f2, "[SLEAF] cr2=");
    fl_hex(&f2, cr2);
    fl_s(&f2, " present=");
    fl_dec(&f2, have ? 1 : 0);
    fl_s(&f2, " leaf=");
    fl_hex(&f2, leaf);
    fl_c(&f2, '\n');
    f2.b[f2.n] = 0;
    uart_puts(f2.b);
  }
  if (cr2 && cr2 < 0x100000000ULL && cur->as) {
    int ahits = 0;
    for (int i = 0; i < cur->as->nr && ahits < 4; i++) {
      struct vm_region *r = &cur->as->regions[i];
      uint64_t lo = r->base & 0xFFFFFFFFULL;
      uint64_t hi = (r->base + r->len) & 0xFFFFFFFFULL;
      if (hi <= lo)
        continue; /* the region's low half wrapped: not comparable */
      if (cr2 >= lo && cr2 < hi) {
        struct fl_buf f3;
        f3.n = 0;
        fl_s(&f3, "[SALIAS] cr2=");
        fl_hex(&f3, cr2);
        fl_s(&f3, " region=");
        fl_hex(&f3, r->base);
        fl_c(&f3, '+');
        fl_hex(&f3, r->len);
        fl_s(&f3, " off=");
        fl_hex(&f3, cr2 - lo);
        fl_s(&f3, " prot=");
        fl_dec(&f3, (int)r->prot);
        fl_s(&f3, " kind=");
        fl_dec(&f3, (int)r->kind);
        fl_s(&f3, " -> cr2 IS the low 32 bits of an address in this region\n");
        f3.b[f3.n] = 0;
        uart_puts(f3.b);
        ahits++;
      }
    }
    if (!ahits) {
      struct fl_buf f3;
      f3.n = 0;
      fl_s(&f3, "[SALIAS] cr2=");
      fl_hex(&f3, cr2);
      fl_s(&f3, " no region's low-32 window contains it (n=");
      fl_dec(&f3, cur->as->nr);
      fl_s(&f3, ")\n");
      f3.b[f3.n] = 0;
      uart_puts(f3.b);
    }
  }
  if (cur->as) {
    int n = cur->as->nr;
    struct fl_buf f4;
    for (int i = 0; i < n && i < 48; i++) {
      struct vm_region *r = &cur->as->regions[i];
      f4.n = 0;
      fl_s(&f4, "[SRGN] i=");
      fl_dec(&f4, i);
      fl_s(&f4, " base=");
      fl_hex(&f4, r->base);
      fl_s(&f4, " len=");
      fl_hex(&f4, r->len);
      fl_s(&f4, " prot=");
      fl_dec(&f4, (int)r->prot);
      fl_s(&f4, " kind=");
      fl_dec(&f4, (int)r->kind);
      fl_c(&f4, '\n');
      f4.b[f4.n] = 0;
      uart_puts(f4.b);
    }
    f4.n = 0;
    fl_s(&f4, "[SRGN] total=");
    fl_dec(&f4, n);
    fl_c(&f4, '\n');
    f4.b[f4.n] = 0;
    uart_puts(f4.b);
  }
#endif
  uint64_t rsp = tf->lr;
  struct fl_buf f;
  f.n = 0;
  fl_s(&f, "[SIGDUMP] pid=");
  fl_dec(&f, cur->pid);
  fl_s(&f, " tgid=");
  fl_dec(&f, cur->tgid);
  fl_s(&f, " nm=");
  fl_s(&f, cur->name);
  fl_s(&f, " rip=");
  fl_hex(&f, tf->elr);
  fl_s(&f, " rsp=");
  fl_hex(&f, rsp);
  fl_s(&f, " cs=");
  fl_hex(&f, tf->cs);
  fl_s(&f, " vec=");
  fl_dec(&f, (int)tf->vector);
  fl_s(&f, " err=");
  fl_hex(&f, tf->error_code);
  fl_s(&f, " cr2=");
  fl_hex(&f, cr2);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  f.n = 0;
  fl_s(&f, "[SGPR]");
  fl_s(&f, " rax="); fl_hex(&f, tf->regs[0]);
  fl_s(&f, " rbx="); fl_hex(&f, tf->regs[1]);
  fl_s(&f, " rcx="); fl_hex(&f, tf->regs[2]);
  fl_s(&f, " rdx="); fl_hex(&f, tf->regs[3]);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  f.n = 0;
  fl_s(&f, "[SGPR]");
  fl_s(&f, " rsi="); fl_hex(&f, tf->regs[4]);
  fl_s(&f, " rdi="); fl_hex(&f, tf->regs[5]);
  fl_s(&f, " rbp="); fl_hex(&f, tf->regs[6]);
  fl_s(&f, " r8="); fl_hex(&f, tf->regs[7]);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  f.n = 0;
  fl_s(&f, "[SGPR]");
  fl_s(&f, " r9="); fl_hex(&f, tf->regs[8]);
  fl_s(&f, " r10="); fl_hex(&f, tf->regs[9]);
  fl_s(&f, " r11="); fl_hex(&f, tf->regs[10]);
  fl_s(&f, " r12="); fl_hex(&f, tf->regs[11]);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  f.n = 0;
  fl_s(&f, "[SGPR]");
  fl_s(&f, " r13="); fl_hex(&f, tf->regs[12]);
  fl_s(&f, " r14="); fl_hex(&f, tf->regs[13]);
  fl_s(&f, " r15="); fl_hex(&f, tf->regs[14]);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  if (rsp >= 0x200) {
    f.n = 0;
    fl_s(&f, "[STK-100]");
    fl_qwords(&f, cur, rsp - 0x100, 16);
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
    f.n = 0;
    fl_s(&f, "[STK-80]");
    fl_qwords(&f, cur, rsp - 0x80, 16);
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
    /* The 0x50 slot is where UnityPlayer+0x91b10e loads its index; print it
     * by name so the launch log's claim and this row are the same bytes. */
    uint64_t q50 = 0;
    if (vm_user_read(cur, rsp - 0x50, &q50, 8) == 0) {
      f.n = 0;
      fl_s(&f, "[SLOT-50] va=");
      fl_hex(&f, rsp - 0x50);
      fl_s(&f, " val=");
      fl_hex(&f, q50);
      fl_s(&f, " ==rsp=");
      fl_dec(&f, q50 == rsp);
      fl_s(&f, " off=");
      if (q50 > rsp) fl_hex(&f, q50 - rsp);
      else fl_hex(&f, rsp - q50);
      fl_c(&f, '\n');
      f.b[f.n] = 0;
      uart_puts(f.b);
    }
  }
  f.n = 0;
  fl_s(&f, "[STK+00]");
  fl_qwords(&f, cur, rsp, 16);
  fl_c(&f, '\n');
  f.b[f.n] = 0;
  uart_puts(f.b);

  /* Where does rsp live, and is the fault address inside any region at all?
   * `region=NONE` for cr2 is the "in=HOLE" verdict in numbers. */
  {
    extern struct vm_region *vm_region_find(struct addr_space *, uint64_t);
    /* VMK_* from src/include/vm.h: 1 IMG, 2 HEAP, 3 ANON, 4 SHARED,
       5 STACK, 6 FB, 7 LIB. */
    static const char *kinds[] = {"?", "img", "heap", "anon", "shared",
                                  "stack", "fb", "lib"};
    uint64_t probes[4] = {rsp, rsp - 0x100, rsp - 0x1000, cr2};
    for (int i = 0; i < 4; i++) {
      struct vm_region *r = cur->as ? vm_region_find(cur->as, probes[i]) : 0;
      f.n = 0;
      fl_s(&f, "[SREG] va=");
      fl_hex(&f, probes[i]);
      if (!r) {
        fl_s(&f, " region=NONE");
      } else {
        fl_s(&f, " base=");
        fl_hex(&f, r->base);
        fl_s(&f, " len=");
        fl_hex(&f, r->len);
        fl_s(&f, " off=");
        fl_hex(&f, probes[i] - r->base);
        fl_s(&f, " prot=");
        fl_dec(&f, (int)r->prot);
        fl_s(&f, " kind=");
        fl_s(&f, (r->kind < 8) ? kinds[r->kind] : "?");
      }
      fl_c(&f, '\n');
      f.b[f.n] = 0;
      uart_puts(f.b);
    }
    /* The 0x2FFF.. window the launch log calls "a stack VA": count how many
     * 1 MiB probes in [0x2F000000, 0x30000000) are mapped.  0 means the
     * decoded value is not an address in this process at all. */
    int hits = 0;
    for (uint64_t va = 0x2F000000ULL; va < 0x30000000ULL; va += 0x100000ULL)
      if (cur->as && vm_region_find(cur->as, va))
        hits++;
    f.n = 0;
    fl_s(&f, "[SREG2F] probes=256 mapped=");
    fl_dec(&f, hits);
    fl_s(&f, " (window 0x2F000000..0x30000000)");
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
  }
  /* A user-mode resume leaves a fingerprint: the iretq stamping writes RIP at
   * dest+280, CS=0x1B at dest+288, RFLAGS at dest+296, RSP at dest+304 and
   * SS=0x23 at dest+312.  CS and SS are therefore THREE qwords apart, not
   * adjacent (the first version of this scan looked for an adjacent pair and
   * could never fire — a broken instrument reads exactly like "no image").
   * cs_off is the CS slot's offset from rsp: for a resume at target_sp == rsp
   * it is -(skip+0x20) and -0x120 pre-fix, where the image covered
   * [rsp-0x140, rsp) and tf->lr landed at rsp-0x50. */
  {
    uint64_t q[0x100];
    int n = 0, hits = 0, first = 0;
    uint64_t lo = (rsp > 0x400) ? rsp - 0x400 : 0;
    if (vm_user_read(cur, lo, q, (int)sizeof q) == 0) {
      n = (int)(sizeof q / 8);
      for (int i = 0; i + 3 < n; i++) {
        if (q[i] == 0x1B && q[i + 3] == 0x23) {
          if (!hits)
            first = (int)((lo + (uint64_t)i * 8) - rsp);
          hits++;
        }
      }
    }
    f.n = 0;
    fl_s(&f, "[SIGSIG] trapframe_images=");
    fl_dec(&f, hits);
    fl_s(&f, " first_cs_off=");
    if (hits < 0) fl_s(&f, "-");
    else {
      fl_s(&f, first < 0 ? "-" : "+");
      fl_hex(&f, (uint64_t)(first < 0 ? -first : first));
    }
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
  }
  /* Was this thread resumed at (about) this SP?  The user-mode resume copies
   * its 296-byte frame to [target_sp-320, target_sp-24) on the USER stack; if
   * the fault rsp is the resume's target_sp, the copy wrote this thread's own
   * SP over the qwords at [rsp-0x50 .. rsp-0x140]. */
  {
    extern int resume_ring_probe(int pid, uint64_t sp, uint64_t *dist,
                                 uint64_t *ms);
    uint64_t dist = 0, ms = 0;
    int hit = resume_ring_probe(cur->pid, rsp, &dist, &ms);
    f.n = 0;
    fl_s(&f, "[SRESUME] pid=");
    fl_dec(&f, cur->pid);
    fl_s(&f, " hit=");
    fl_dec(&f, hit);
    fl_s(&f, " dist=");
    fl_hex(&f, dist);
    fl_s(&f, " age_ms=");
    fl_dec(&f, (int)ms);
    fl_c(&f, '\n');
    f.b[f.n] = 0;
    uart_puts(f.b);
  }
}

/* SEC-OBS-B debug hook: rank checks do not apply inside IRQ dispatch (see
 * lock.h).  Wrapper keeps the asm-visible symbol; inner is the original
 * handler body unchanged. */
static void general_interrupt_inner(struct trap_frame *tf);

/* GB-146-A: the fatal dump can print a frame whose vector/CS slots already
   hold data (measured census: vector=4096 rip=0x97).  Remember the frame the
   CPU actually handed the handler at entry, and remember the last
   enter_user_space frame copy per CPU (its destination window is where a
   parked task's live frame sits).  At the fatal dump the record can then say
   either "the slot was already foreign at entry" or "it changed after entry,
   in this pid's handler window", and can name a concurrent frame copy whose
   window covers the slot -- pid + VA, the two things the card asks for. */
struct badvec_stamp {
  uint64_t frame;
  uint64_t vector;
  uint64_t cs;
  int64_t pid;
};
static struct badvec_stamp badvec_at_entry[MAX_CPUS];

struct badvec_eu_note {
  uint64_t src;
  uint64_t dst;
  uint64_t pc;
  int64_t pid;
};
static struct badvec_eu_note badvec_eu[MAX_CPUS];

/* GB-146-A (bounce 2026-11-08T17:56Z): the entry stamp can only say "the
   slot differs from entry"; it cannot name the writing instruction.  A
   hardware write watchpoint on the vector slot can.  Armed in DR0 for frames
   that live in USER address space (where the census frame is), it fires #DB on
   the first write to that slot and the #DB frame's saved RIP names the
   instruction after the write.  One shot per CPU: the #DB handler disarms. */
static int badvec_watch_armed[MAX_CPUS];
static uint64_t badvec_watch_slot[MAX_CPUS];
static int badvec_writer_prints;

/* Is this frame in the window the two census dumps used?  User stacks are at
   0x25FFxxxx; the kernel image and its .bss live at 0x70000000+, which is also
   below USER_VA_BASE (6 GiB), so the base alone is not a discriminator.  Only
   arm/watch below the kernel image. */
static int badvec_frame_window(uint64_t p) {
  return p >= 0x1000ULL && p < 0x70000000ULL;
}

static void badvec_watch_arm(uint64_t slot) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS)
    return;
  __asm__ volatile("mov %0, %%dr0" ::"r"(slot));
  __asm__ volatile("mov %0, %%dr6" ::"r"(0UL));
  /* L0=1, RW0=01 (write), LEN0=10 (8 bytes) -- same encoding as rw_watch.c. */
  __asm__ volatile("mov %0, %%dr7" ::"r"(0x00090001UL));
  badvec_watch_armed[c] = 1;
  badvec_watch_slot[c] = slot;
}

static void badvec_watch_disarm(void) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS)
    return;
  __asm__ volatile("mov %0, %%dr7" ::"r"(0UL));
  __asm__ volatile("mov %0, %%dr0" ::"r"(0UL));
  badvec_watch_armed[c] = 0;
}

static int64_t badvec_pid(void) {
  extern struct process *current_process(void);
  struct process *p = current_process();
  return p ? (int64_t)p->pid : -1;
}

/* Called from enter_user_space (asm) with rdi=source frame, rsi=target SP. */
void enter_user_space_note(uint64_t src, uint64_t dst) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS)
    return;
  badvec_eu[c].src = src;
  badvec_eu[c].dst = dst;
  badvec_eu[c].pc = (uint64_t)__builtin_return_address(0);
  badvec_eu[c].pid = badvec_pid();
}

void general_interrupt_handler(struct trap_frame *tf) {
#ifdef KERNEL_MODE_UNIT_TEST
  extern void lockdbg_irq_enter(void);
  extern void lockdbg_irq_exit(void);
  lockdbg_irq_enter();
  general_interrupt_inner(tf);
  lockdbg_irq_exit();
#else
  general_interrupt_inner(tf);
#endif
}

static void general_interrupt_inner(struct trap_frame *tf) {
  extern void gic_set_current_vector(uint32_t cpu, uint32_t vector);
  extern uint32_t gic_acknowledge_interrupt(void);
  extern void gic_end_interrupt(uint32_t intid);
  extern int virtio_net_irq;

  uint32_t cpu = get_cpuid();
  if (cpu < MAX_CPUS) {
    badvec_at_entry[cpu].frame = (uint64_t)tf;
    badvec_at_entry[cpu].vector = tf->vector;
    badvec_at_entry[cpu].cs = tf->cs;
    badvec_at_entry[cpu].pid = badvec_pid();
    /* GB-146-A: a frame below the kernel image is the census case; watch its
       vector slot for the first write and let #DB name the instruction.  Cap
       the number of records so a pathological re-arm cannot flood the sink. */
    if (!badvec_watch_armed[cpu] && badvec_writer_prints < 8 &&
        badvec_frame_window((uint64_t)tf))
      badvec_watch_arm((uint64_t)&tf->vector);
  }
  /* GB-101: a TORN trap frame read by the handler.  The x86 IDT has 256 gates
     (vectors 0-31 exceptions, 32-255 IRQ/syscall); a `vector` > 0xFF (measured:
     vector=20000, rip=0x97) cannot have been pushed by the CPU, so the frame is
     corrupt.  This alone misses the family's below-32 garbage vectors (measured:
     vector=6 rip=0x7, vector=13 rip=0x7009, vector=6 rip=0x1000007) whose frames
     are also torn.  The GB-101-V gate's fix direction: a FRAME-COHERENCE test,
     not a wider threshold.  A real x86-64 entry ALWAYS has the CPU push CS (0x08
     kernel / 0x1B user -- SS/RSP are only pushed on a privilege change, so SS is
     NOT authoritative on same-privilege kernel traps and must not be judged here);
     a torn/overwritten frame that a garbage vector rode in on carries a garbage
     CS slot.  Never dispatch on garbage: emit a line containing "FAIL" (the
     tier's FAIL-line grep names it), halt-record (GB-060, so a lock a stopped
     CPU held is recoverable and other cores don't spin behind it forever), and
     stop THIS CPU rather than run an unknown-vector path on a bogus frame. */
  {
    extern void panic_puts(const char *s);
    extern void panic_print_hex(uint64_t v);
    extern void print_int_raw(int val);
    extern void cpu_halt_record(uint32_t cpu);
    uint64_t frame_cs = tf->cs;
    int bad_vec = ((uint64_t)tf->vector > 0xFFu);
    int bad_cs  = (frame_cs != 0x08u && frame_cs != 0x1Bu);
    if (bad_vec || bad_cs) {
      panic_puts("[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=");
      print_int_raw((int)(uint64_t)tf->vector);
      panic_puts(" rip="); panic_print_hex((uint64_t)tf->elr);
      panic_puts(" cs=");   panic_print_hex(frame_cs);
      panic_puts(" (impossible vector="); print_int_raw((int)(uint64_t)tf->vector);
      panic_puts(" or invalid cs; a real gate entry pushes cs 0x08/0x1B\n");
      panic_puts("\n");
      cpu_halt_record(cpu);
      extern int spinlock_release_if_held_by_me(spinlock_t *lock);
      extern spinlock_t proc_lock;
      if (spinlock_release_if_held_by_me(&proc_lock))
        panic_puts("[KERNEL] released proc_lock before halting (GB-060)\n");
      while (1);
    }
  }
  /* GB-146-A: our DR0 write watchpoint on a user-space frame's vector slot
     fired.  Consume the #DB, name the instruction (saved RIP points just past
     the write), and disarm; the handler returns into the interrupted path. */
  if (tf->vector == 1) {
    extern void panic_puts(const char *s);
    extern void panic_print_hex(uint64_t v);
    extern void print_int_raw(int val);
    uint32_t wc = get_cpuid();
    if (wc < MAX_CPUS && badvec_watch_armed[wc]) {
      panic_puts("[BADVEC] writer rip_after=");
      panic_print_hex(tf->elr);
      panic_puts(" slot=");
      panic_print_hex(badvec_watch_slot[wc]);
      panic_puts(" pid=");
      print_int_raw((int)badvec_pid());
      panic_puts(" cpu=");
      print_int_raw((int)wc);
      panic_puts("\n");
      badvec_writer_prints++;
      badvec_watch_disarm();
      return;
    }
  }
  gic_set_current_vector(cpu, tf->vector);
  watchdog_tick(cpu, tf);

  /* RW-WATCH: DR0 data-write breakpoint.  Two hooks, both compiled out
     without -DRW_WATCH_ADDR:
       (a) every trap arms this CPU's DR0/DR7 once (the registers are per-CPU
           and this scheduler does not context-switch them).  Arming on ANY
           vector, not just the timer range: measured, -smp 8 armed only cpu 0
           when this was gated to vectors 32..47 -- the other cores take IPIs
           and syscalls, so their writes stayed invisible;
       (b) #DB is consumed here, before the generic exception path (which
           would otherwise kill the process for an "unknown" vector 1). */
  rw_watch_arm_cpu();
  if (tf->vector == 1 && rw_watch_db(tf)) return;

  /* SEC-OBS-B: drain deferred lost-wakeup requests here rather than at
   * the virtio-blk completion that raised them (see process.h). */
  process_wake_all_drain();

  if (tf->vector == 32) {
    // PIT/LAPIC timer interrupt
    uint32_t intid = gic_acknowledge_interrupt();

    if (cpu == 0) {
      // The PIT is the global time base and is wired to the boot core only.
      timer_reload();

      /* M6/L59: 5 s thread census, [THRS] (docs/rimworld-launch-log.md §46).
         The 600 s run climbs in live threads and dies at ~410 s; the curve is
         the evidence, and 5 s costs one row per live compat group. */
      {
        extern void rw_thr_sample(void);
        static uint32_t thr_ticks;
        if (++thr_ticks >= 500) {
          thr_ticks = 0;
          rw_thr_sample();
          /* M6/L72: the display mode is the kernel's, and a lost mode is
             silent from inside the guest (presents still "work").  Read the
             dispi/CRTC state every 5 s and restore the shared desktop mode if
             it was reset; one row only when the state changes. */
          {
            extern void virtio_gpu_health_tick(void);
            if (virtio_gpu_health_tick)
              virtio_gpu_health_tick();
          }
        }
      }

      // Broadcast rescheduling IPI (vector 0x81) to all other cores.
      // Bit 14 stays clear: it is defined for level-triggered delivery only.
      *(volatile uint32_t*)(0xFEE00300) = 0x000C0081;
#ifdef PANIC_IPI_TEST
      /* GB-207: fire the two-CPU panic injection once the wave is running
         (a secondary sits in an IF=0 wait before there is a process). */
      extern void gb207_tick(void);
      gb207_tick();
#endif
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
      /* P5 S3 (D8.4a): schedule() returns only when this process kept the
         CPU; a switch resumes via enter_user_space's own boundary. */
      process_signal_trap_exit(tf);
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
#ifdef PANIC_IPI_TEST
  } else if (tf->vector == 0x83) {
    // GB-207: test-only two-CPU panic injection.  EOI first (LAPIC-delivered),
    // then the claim/record path in uart.c; absent from default kernels.
    lapic_send_eoi();
    extern void gb207_ipi_handler(void);
    gb207_ipi_handler();
#endif
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
      const char *why = "PROT";
      /* M6/L72: name the instruction that walked into a fresh anon page
         ([ANONF] in vm.c).  Read by the kernel only; no behaviour change. */
      {
        extern uint64_t vm_dbg_fault_rip;
        vm_dbg_fault_rip = tf->elr;
      }
      /* A present page can still fault legally: a MAP_PRIVATE file page is
         installed read-only so the first write copies it (so-vm lane A), and
         a just-mapped page can race the TLB.  vm_handle_fault distinguishes
         those from a real protection violation and answers "PROT" for the
         latter, so both classes go through it now — the old !pf_present gate
         sent every COW write straight to the kill path (measured: the
         interpreter's first relocation write to LIBC.SO). */
      int frc = vm_handle_fault(process_group(cur), cr2, pf_w, pf_x, &why);
      if (frc == 0) {
        return; /* mapping is live; iretq re-executes the instruction */
      }
      /* M6/L69: a REFUSED PAGE IS AN I/O ERROR ON A MAPPING, NOT A
       * PROTECTION VIOLATION.  vm_handle_fault returns 2 when the
       * file-backed page could not be delivered (the exec gate refused a page
       * of zeros, a short read never completed, the mapping is already marked
       * VM_MAP_IOERR).  Linux answers that shape with SIGBUS/BUS_ADRERR, and
       * it is the answer a compat program can act on: Mono installs a SIGSEGV
       * handler for null checks, so delivering SIGSEGV here retries the
       * instruction forever (measured: pid 40 looped until rt_sigreturn's own
       * frame was corrupt, then died with status 11 and no diagnostic).
       * SIGBUS runs the runtime's hardware-exception path, which aborts the
       * failed file load — and the region stays marked, so no later fault
       * re-reads the device. */
      if (frc == 2) {
        struct fl_buf fe;
        fe.n = 0;
        fl_s(&fe, "[EIO] pid=");
        fl_dec(&fe, cur->pid);
        fl_s(&fe, " (");
        fl_s(&fe, cur->name);
        fl_s(&fe, ") page-in failed VA=");
        fl_hex(&fe, cr2);
        fl_s(&fe, " PC=");
        fl_hex(&fe, tf->elr);
        fl_s(&fe, " rw=");
        fl_c(&fe, pf_w ? 'w' : 'r');
        fl_c(&fe, pf_x ? 'x' : ' ');
        fl_s(&fe, " region=");
        {
          extern struct vm_region *vm_region_find(struct addr_space *as,
                                                 uint64_t va);
          struct vm_region *er = cur->as ? vm_region_find(cur->as, cr2) : 0;
          if (!er) {
            fl_s(&fe, "NONE");
          } else {
            fl_hex(&fe, er->base);
            fl_c(&fe, '+');
            fl_hex(&fe, er->len);
            fl_s(&fe, " prot=");
            fl_dec(&fe, (int)er->prot);
            fl_s(&fe, " kind=");
            fl_dec(&fe, (int)er->kind);
            fl_s(&fe, " ooff=");
            fl_hex(&fe, er->obj_off);
          }
        }
        fl_s(&fe, " live=");
        fl_dec(&fe, process_group(cur)->live_threads);
        fl_s(&fe, " -> SIGBUS (EIO, not a protection fault)\n");
        fe.b[fe.n] = 0;
        uart_puts(fe.b);
        process_fault_exit(tf, 7); /* SIGBUS: a failed file load, not SIGSEGV */
        return;
      }
      {
        struct fl_buf fl;
        fl.n = 0;
        fl_s(&fl, "[KERNEL] pid=");
        fl_dec(&fl, cur->pid);
        fl_s(&fl, " (");
        fl_s(&fl, cur->name);
        fl_s(&fl, ") memory fault VA=");
        fl_hex(&fl, cr2);
        fl_s(&fl, " PC=");
        fl_hex(&fl, tf->elr);
        fl_s(&fl, " rw=");
        fl_c(&fl, pf_w ? 'w' : 'r');
        fl_c(&fl, pf_x ? 'x' : ' ');
        fl_s(&fl, " in=");
        fl_s(&fl, why);
        {
          extern struct vm_region *vm_region_find(struct addr_space *as, uint64_t va);
          struct vm_region *fr = cur->as ? vm_region_find(cur->as, cr2) : 0;
          fl_s(&fl, " region=");
          if (!fr) {
            fl_s(&fl, "NONE");
            /* M6/L59: a hole with a mapped region just above it is a guard
               page or a stack that walked past its bottom -- the stack-overflow
               verdict.  `below` is the distance to that region's base, so a
               value under 0x1000 with a writable anon/stack region is a
               one-page overrun, not a wild pointer. */
            {
              extern struct vm_region *vm_region_above(struct addr_space *, uint64_t, uint64_t);
              struct vm_region *nb =
                  cur->as ? vm_region_above(cur->as, cr2, 0x200000) : 0;
              if (nb) {
                fl_s(&fl, " near=");
                fl_hex(&fl, nb->base);
                fl_s(&fl, "+");
                fl_hex(&fl, nb->len);
                fl_s(&fl, " below=");
                fl_hex(&fl, nb->base - cr2);
                fl_s(&fl, " nkind=");
                fl_dec(&fl, (int)nb->kind);
                fl_s(&fl, " nprot=");
                fl_dec(&fl, (int)nb->prot);
              }
            }
          } else {
            fl_hex(&fl, fr->base);
            fl_c(&fl, '+');
            fl_hex(&fl, fr->len);
            fl_s(&fl, " prot=");
            fl_dec(&fl, (int)fr->prot);
            fl_s(&fl, " kind=");
            fl_dec(&fl, (int)fr->kind);
          }
        }
        /* M6/L59: the two numbers §45.8 lacked.  `live` is the group's thread
           count at the fault instant (the [THRS] curve gives the shape, this
           gives the value), `rsp` says whether the faulting stack pointer was
           already outside any region, and `inH` says the fault happened inside
           a signal handler (the one case where no handler dump is possible). */
        fl_s(&fl, " rsp=");
        fl_hex(&fl, tf->lr);
        fl_s(&fl, " live=");
        fl_dec(&fl, process_group(cur)->live_threads);
        fl_s(&fl, " inH=");
        fl_dec(&fl, cur->sig_in_handler);
        {
          uint64_t cur_cr3;
          __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
          fl_s(&fl, " cr3frame=");
          fl_hex(&fl, cur_cr3 & ~0xFFFULL);
          fl_s(&fl, " asroot=");
          fl_hex(&fl, cur->as ? cur->as->root_phys : cur->user_phys_base);
          fl_s(&fl, " sys=");
          fl_dec(&fl, (int)cur->last_syscall);
          fl_s(&fl, " ret=");
          fl_hex(&fl, (uint64_t)cur->last_ret);
          /* wk: essentials only.
           *
           * L73: the frame pointer is a USER value and this is the dump for a
           * user fault, so it is exactly the pointer most likely to be bad:
           * measured, `mov (%rbp)` faulted at cr2 == rbp == 0x11CB280000 (one
           * past the end of a 16 MiB anon region) and this block then read
           * *(uint64_t*)rbp and *(uint64_t*)(rbp+8) straight -- a second,
           * KERNEL-mode #PF on the same hole.  The process died with
           * "[KERNEL] Kernel-mode #PF in user address space" and the row that
           * would have said what the game actually did (this one) was never
           * printed, because it is emitted after these reads.  Never
           * dereference a user pointer in a report: vm_user_read is the
           * accessor that validates and copies per page, and a hole answers
           * <unreadable> instead of killing the kernel's own console path. */
          fl_s(&fl, " rbx=");
          fl_hex(&fl, tf->regs[1]);
          fl_s(&fl, " rdi=");
          fl_hex(&fl, tf->regs[5]);
          fl_s(&fl, " rsi=");
          fl_hex(&fl, tf->regs[4]);
          fl_s(&fl, " rbp=");
          fl_hex(&fl, tf->regs[6]);
          {
            uint64_t uq[2];
            if (kpf_user_peek2(cur, tf->regs[6], uq) == 0) {
              fl_s(&fl, " rbp0=");
              fl_hex(&fl, uq[0]);
              fl_s(&fl, " ret=");
              fl_hex(&fl, uq[1]);
            } else {
              fl_s(&fl, " rbp0=<unreadable>");
            }
          }
        }
        fl_s(&fl, " -> killed");
        fl_c(&fl, '\n');
        fl.b[fl.n] = 0;
        uart_puts(fl.b);
      }
      /* M6/L37: the full record, before the process dies (compat targets only;
       * a native fault keeps the compact row). */
      if (cur->abi_flags & PROC_FLAG_LINUX_ABI) {
#ifdef RW_SC_RING
        /* L85: what this thread was doing on the way in, with arguments and
           return values -- the provenance row for a bogus pointer the game
           may have been handed by a syscall. */
        sc_hist_dump(cur);
#endif
        compat_fault_dump(cur, tf, cr2);
      }
      process_fault_exit(tf, 11); /* SIGSEGV status byte */
      return;
    }
    if (cur && !cur->is_kernel_process && (tf->cs & 3) == 3) {
      /* Report in ONE uart_puts. This line was four separate calls, so under
         SMP it interleaved with other CPUs' output and became unreadable --
         an actual #GP read as "Vector: [ARRIVED] 13 RIP: 0T4PIE.BINx" and went
         unnoticed for several sessions of investigation. One buffer, one lock
         hold, one record. */
      {
        char fb[96];
        int fi = 0;
        const char *p1 = "[KERNEL] User process fault! Vector: ";
        while (*p1 && fi < 60) fb[fi++] = *p1++;
        unsigned v = (unsigned)tf->vector;
        if (v >= 10) fb[fi++] = (char)('0' + v / 10);
        fb[fi++] = (char)('0' + v % 10);
        const char *p2 = " RIP: 0x";
        while (*p2 && fi < 80) fb[fi++] = *p2++;
        uint64_t rv = tf->elr;
        int st = 0;
        for (int sh = 60; sh >= 0 && fi < 88; sh -= 4) {
          unsigned d = (unsigned)((rv >> sh) & 0xF);
          if (!d && !st && sh) continue;
          st = 1;
          fb[fi++] = "0123456789abcdef"[d];
        }
        fb[fi++] = '\n';
        fb[fi] = 0;
        uart_puts(fb);
      }
      /* M6/L59: WHICH mapping the faulting PC belongs to, and whether it is
         the JIT arena.  §45.8's ~410 s death is a #UD (vector 6) at a PC that
         is not in any DSO; a "#UD in executable anon memory" is a JIT code
         fault (a freed/reused code chunk) and is a completely different fix
         from "#UD in a DSO" (an unsupported instruction).  The region and the
         arena bounds decide it in one row. */
      {
        extern void panic_puts(const char *s);
        extern void panic_print_hex(uint64_t v);
        extern void print_int_raw(int val);
        struct vm_region *rg = cur->as ? vm_region_find(cur->as, tf->elr) : 0;
        panic_puts("[KERNEL] fault-pc region=");
        if (!rg) {
          panic_puts("NONE");
        } else {
          panic_print_hex(rg->base);
          panic_puts("+");
          panic_print_hex(rg->len);
          panic_puts(" prot=");
          print_int_raw((int)rg->prot);
          panic_puts(" kind=");
          print_int_raw((int)rg->kind);
        }
        panic_puts(" jit=");
        panic_puts(cur->as && cur->as->jit_base &&
                              tf->elr >= cur->as->jit_base &&
                              tf->elr < cur->as->jit_end
                          ? "1"
                          : "0");
        panic_puts(" live=");
        print_int_raw(process_group(cur)->live_threads);
        panic_puts(" err=");
        panic_print_hex((uint64_t)tf->error_code);
        panic_puts(" rsp=");
        panic_print_hex((uint64_t)tf->lr);
        panic_puts("\n");
      }
      /* M6/L59: the register file at the fault, printed by the KERNEL before
         any delivery attempt.  §45.8's ~410 s death is a #UD (vector 6) whose
         RIP is not on an instruction boundary of the file (measured: run 2
         landed two bytes into a `call`, run 3 on a byte that decodes as
         `push %rbx`), which is what a corrupted control-flow target looks
         like: execution started in the middle of a function and desynchronised
         into an invalid opcode.  The signal frame could not be planted for
         that fault, so no user-space dump exists; this row is the only place
         the jump target's provenance (a register or a stack slot) is visible. */
      {
        extern void uart_puts(const char *s);
        extern void uart_print_hex(uint64_t v);
        extern void print_int(int v);
        static int kr_rows;
        if ((cur->abi_flags & PROC_FLAG_LINUX_ABI) && kr_rows < 12) {
          kr_rows++;
          struct fl_buf kr;
          kr.n = 0;
          fl_s(&kr, "[KREG] pid=");
          fl_dec(&kr, cur->pid);
          fl_s(&kr, " vec=");
          fl_dec(&kr, (int)tf->vector);
          fl_s(&kr, " erc=");
          fl_hex(&kr, (uint64_t)tf->error_code);
          fl_s(&kr, " rip=");
          fl_hex(&kr, (uint64_t)tf->elr);
          fl_s(&kr, " rsp=");
          fl_hex(&kr, (uint64_t)tf->lr);
          static const char *rn[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                       "rbp", "r8",  "r9",  "r10", "r11", "r12",
                                       "r13", "r14", "r15", "rsp0"};
          for (int i = 0; i < 15; i++) {
            fl_s(&kr, " ");
            fl_s(&kr, rn[i]);
            fl_s(&kr, "=");
            fl_hex(&kr, (uint64_t)tf->regs[i]);
          }
          fl_s(&kr, "\n");
          kr.b[kr.n] = 0;
          uart_puts(kr.b);
        }
      }
      /* M6/L72: THE BYTE UNDER THE PATCH.
       *
       * `x86-codegen.h:403` is the `default: g_assert(0)` arm of the opcode
       * switch in `mono_x86_patch_inline`, reached when the byte at `code` is
       * not one the table knows.  In the ud2 probe build the assert's
       * `lea file,%rdi` is replaced by `ud2`, so the #UD arrives with the
       * function's live registers: rdi = the code pointer being patched and
       * rdx's low byte = the opcode it read.  Printing the eight bytes AT rdi
       * turns "the switch failed" into "this instruction byte, in this
       * sequence": a clean `mov 0x..(%rip),%rax` means a legitimate opcode the
       * table does not implement; random bytes sewn into a call's middle mean
       * the code buffer was corrupted.  Same 12-row budget as [KREG]. */
      if (tf->vector == 6 && cur->as) {
        extern int l_copy_from_user(struct process *, void *, uint64_t, uint64_t);
        static int udx_rows;
        uint64_t cp = (uint64_t)tf->regs[5]; /* rdi */
        if (cp && udx_rows < 12 && (cp & 0xFFF) <= 0xFF8) {
          struct vm_region *cr = vm_region_find(cur->as, cp);
          if (cr && (cr->prot & VM_PROT_EXEC)) {
            unsigned char b[8];
            if (l_copy_from_user(cur, b, cp, sizeof b) == 0) {
              udx_rows++;
              struct fl_buf kc;
              kc.n = 0;
              fl_s(&kc, "[UDX] code=");
              fl_hex(&kc, cp);
              for (int i = 0; i < 8; i++) {
                fl_s(&kc, " ");
                fl_hex(&kc, (uint64_t)b[i] & 0xFF);
              }
              fl_s(&kc, " op=");
              fl_hex(&kc, (uint64_t)((uint64_t)tf->regs[3] & 0xFF)); /* dl */
              fl_s(&kc, " rgn=");
              fl_hex(&kc, (uint64_t)cr->base);
              fl_s(&kc, "+");
              fl_hex(&kc, (uint64_t)cr->len);
              fl_s(&kc, " kind=");
              fl_dec(&kc, (int)cr->kind);
              fl_s(&kc, "\n");
              kc.b[kc.n] = 0;
              uart_puts(kc.b);
            }
          }
        }
      }
      /* M6/L59: when the fault is a #UD inside a file-backed executable
         region, compare the bytes in memory with the bytes in the file.  A
         match means the CPU was pointed at a valid-but-wrong instruction (a
         corrupted control-flow target); a difference means this kernel served
         a text page that does not hold the file's bytes (a VM/lifetime bug).
         Those are different fixes, and this row is the only thing that
         separates them. */
      if (tf->vector == 6 && cur->as) {
        struct vm_region *ur = vm_region_find(cur->as, tf->elr);
        if (ur && ur->obj && ur->obj->kind == VM_OBJ_FILE &&
            (ur->prot & VM_PROT_EXEC) && ur->obj->file_start_cluster) {
          /* M6/L60: the L59 probe read the file into a .bss array and the page
             into the faulting VA, and printed `pagecmp=DIFF mem=00..` — but
             both halves were suspect (fat16_read_direct_pos wants a PHYSICAL
             destination, and a compare against a page that another thread may
             already have recycled says nothing about what the CPU fetched).
             This probe allocates a real frame for the file read, walks the
             FAULTING page tables for the leaf the CPU actually translates
             through, asks the frame allocator whether that frame is still
             owned, and asks the object which frame IT has cached for the page.
             `alloc=0` is a use-after-free; `pa != objfr` with exec bytes is a
             private copy (expected for text since L59); a `pg=` that is not
             the file's page says the fill published the wrong bytes. */
          uint64_t page_va = tf->elr & ~0xFFFULL;
          uint64_t uoff = page_va - ur->base + ur->obj_off;
          uint64_t leaf = 0;
          int have_leaf = vm_arch_walk(cur->as, page_va, &leaf) == 0;
          uint64_t pa = have_leaf ? vm_arch_leaf_phys(leaf) : 0;
          /* Order matters: ask the allocator and snapshot the executed bytes
             BEFORE frame_alloc() below.  If the leaf points at an already
             freed frame, frame_alloc() can hand back THAT frame and the file
             read would overwrite the evidence before it is read. */
          int pa_alloc = pa ? frame_is_alloced(pa) : -1;
          uint64_t objfr = vm_object_peek(ur->obj, uoff);
          const uint8_t *mem = (const uint8_t *)tf->elr;
          const uint8_t *memp = (const uint8_t *)page_va;
          int inpage = (int)(tf->elr & 0xFFF);
          uint8_t memsnap[16], pgsnap[12];
          for (int i = 0; i < 16; i++)
            memsnap[i] = mem[i];
          for (int i = 0; i < 12; i++)
            pgsnap[i] = memp[i];
          uint64_t fpage = frame_alloc();
          int fn = fpage ? fat16_read_direct_pos(ur->obj->file_start_cluster,
                                                (uint32_t)ur->obj->size,
                                                fpage, FRAME_SIZE,
                                                (uint32_t)uoff)
                         : -1;
          /* Two controls, because a single wrong `file=` row cannot say
             whether the FILE read failed or the PAGE failed:
               ctl  = a fresh read of a page this file certainly has code in
                      (obj_off+0, the .init prologue) through the same call;
               objb = the bytes in the vm_object's own cached frame for THIS
                      offset (the fill source), if it is still owned. */
          uint64_t cpage = frame_alloc();
          int cn = -1;
          if (cpage)
            cn = fat16_read_direct_pos(ur->obj->file_start_cluster,
                                       (uint32_t)ur->obj->size, cpage,
                                       FRAME_SIZE, (uint32_t)ur->obj_off);
          const uint8_t *cbt = (const uint8_t *)cpage;
          int obj_ok = objfr && frame_is_alloced(objfr);
          const uint8_t *ob = (const uint8_t *)(obj_ok ? objfr : 0);
          const uint8_t *fp = (const uint8_t *)fpage;
          int same = 1;
          if (fn != (int)FRAME_SIZE) {
            same = 0;
          } else {
            for (int i = 0; i < 16; i++)
              if (memsnap[i] != fp[inpage + i]) {
                same = 0;
                break;
              }
          }
          static int udx_rows;
          if (udx_rows < 4) {
            udx_rows++;
            struct fl_buf uc;
            uc.n = 0;
            fl_s(&uc, "[UDX] rip=");
            fl_hex(&uc, tf->elr);
            fl_s(&uc, " off=");
            fl_hex(&uc, uoff);
            fl_s(&uc, " rd=");
            fl_dec(&uc, fn);
            fl_s(&uc, " leaf=");
            fl_hex(&uc, leaf);
            fl_s(&uc, " pa=");
            fl_hex(&uc, pa);
            fl_s(&uc, " alloc=");
            fl_dec(&uc, pa_alloc);
            fl_s(&uc, " objfr=");
            fl_hex(&uc, objfr);
            fl_s(&uc, " same=");
            fl_s(&uc, same ? "1" : "0");
            fl_s(&uc, " wf=");
            fl_dec(&uc, frame_watch_count());
            fl_c(&uc, '\n');
            uc.b[uc.n] = 0;
            uart_puts(uc.b);

            uc.n = 0;
            fl_s(&uc, "[UDX] objb=");
            for (int i = 0; i < 8 && ob; i++) {
              fl_hex(&uc, (uint64_t)ob[i]);
              fl_c(&uc, ' ');
            }
            fl_s(&uc, " ctl=");
            fl_dec(&uc, cn);
            fl_c(&uc, ' ');
            for (int i = 0; i < 8; i++) {
              fl_hex(&uc, (uint64_t)cbt[i]);
              fl_c(&uc, ' ');
            }
            fl_s(&uc, " want=");
            for (int i = 0; i < 8; i++) {
              fl_hex(&uc, (uint64_t)fp[i]);
              fl_c(&uc, ' ');
            }

            uc.n = 0;
            fl_s(&uc, "[UDX] pghd=");
            for (int i = 0; i < 12; i++) {
              fl_hex(&uc, (uint64_t)pgsnap[i]);
              fl_c(&uc, ' ');
            }
            fl_s(&uc, " ripb=");
            for (int i = 0; i < 12; i++) {
              fl_hex(&uc, (uint64_t)memsnap[i]);
              fl_c(&uc, ' ');
            }
            fl_s(&uc, " file=");
            for (int i = 0; i < 12 && fn == (int)FRAME_SIZE; i++) {
              fl_hex(&uc, (uint64_t)fp[inpage + i]);
              fl_c(&uc, ' ');
            }
            fl_c(&uc, '\n');
            uc.b[uc.n] = 0;
            uart_puts(uc.b);

            uc.n = 0;
            fl_s(&uc, "[UDX] stk=");
            const volatile uint64_t *sp = (const volatile uint64_t *)tf->lr;
            for (int i = 0; i < 8; i++) {
              fl_hex(&uc, sp ? sp[i] : 0);
              fl_c(&uc, ' ');
            }
            fl_c(&uc, '\n');
            uc.b[uc.n] = 0;
            uart_puts(uc.b);
          }
          if (fpage)
            frame_free(fpage);
          if (cpage)
            frame_free(cpage);
        }
      }
      /* w5 (ii): a user-mode exception (BRK/#UD/#GP/...) kills with a
         signal status, never a stale-x0 exit code (rc=0 false pass). */
      process_fault_exit(tf, 11); /* SIGSEGV status byte */
    } else {
      /* Kernel-mode fault.  ALL of this dump goes to COM1 via the
         lock-free raw sink (NOT the 0xE9 debugcon, which QEMU usually
         never maps — the pre-fix handler's output was invisible and the
         death looked like a silent triple fault).  In a #GP/#PF the frame
         may be garbage, so the stack window below is range-guarded: an
         unchecked dereference of tf[38] was itself faulting (#PF -> #DF
         -> CPU reset) before the dump could finish. */
      extern void panic_puts(const char *s);
      extern void panic_print_hex(uint64_t v);
      extern void print_int_raw(int val);
      extern int panic_claim(void);
      extern void panic_leave(void);
      if (!panic_claim()) {
        /* GB-204: another CPU owns the fatal record.  Count the drop (done in
           panic_claim), write nothing, and stop this CPU rather than
           interleave fields into the one record. */
        for (;;) __asm__ volatile("hlt");
      }
      /* GB-146-A: find the entry stamp for the frame the dump is about to
         print.  Scan the stamps rather than trusting the current CPU index:
         get_cpuid() itself reads GS, and a GS mix-up is one of the ways a
         frame arrives torn, so the dump must still find (or miss) its entry
         frame when the CPU index is what broke.  Same frame + different
         vector/CS slots means the word was written AFTER entry, inside a
         handler window; no stamp at all says the dump frame was never handed
         to any entry (and prints the frames that were).  Both forms name the
         slot VA and the pid, and a concurrent enter_user_space copy whose
         window covers the slot is printed with its pid and src/dst VA. */
      {
        struct badvec_stamp *st = 0;
        int st_cpu = -1;
        uint64_t tfu = (uint64_t)tf;
        int frame_user = badvec_frame_window(tfu);
        for (uint32_t i = 0; i < MAX_CPUS; i++) {
          if (badvec_at_entry[i].frame == tfu) {
            st = &badvec_at_entry[i];
            st_cpu = (int)i;
            break;
          }
        }
        panic_puts("[BADVEC] fatal frame=");
        panic_print_hex(tfu);
        panic_puts(" in_user=");
        print_int_raw(frame_user);
        panic_puts(" vector_slot=");
        panic_print_hex((uint64_t)&tf->vector);
        panic_puts(" cs_slot=");
        panic_print_hex((uint64_t)&tf->cs);
        panic_puts("\n");
        if (!st) {
          panic_puts("[BADVEC] fatal dump has no entry stamp: frame=");
          panic_print_hex((uint64_t)tf);
          panic_puts(" now_v=");
          print_int_raw((int)tf->vector);
          panic_puts(" now_cs=");
          panic_print_hex(tf->cs);
          panic_puts(" now_pid=");
          print_int_raw((int)badvec_pid());
          panic_puts("\n");
        } else if (st->vector != tf->vector || st->cs != tf->cs) {
          panic_puts("[BADVEC] frame slots changed after entry: frame=");
          panic_print_hex((uint64_t)tf);
          panic_puts(" entry_frame=");
          panic_print_hex(st->frame);
          panic_puts("\n  vector_slot=");
          panic_print_hex((uint64_t)&tf->vector);
          panic_puts(" entry=");
          print_int_raw((int)st->vector);
          panic_puts(" now=");
          print_int_raw((int)tf->vector);
          panic_puts("\n  cs_slot=");
          panic_print_hex((uint64_t)&tf->cs);
          panic_puts(" entry=");
          panic_print_hex(st->cs);
          panic_puts(" now=");
          panic_print_hex(tf->cs);
          panic_puts(" cpu=");
          print_int_raw(st_cpu);
          panic_puts(" entry_pid=");
          print_int_raw((int)st->pid);
          panic_puts(" now_pid=");
          print_int_raw((int)badvec_pid());
          panic_puts("\n");
          for (uint32_t oc = 0; oc < MAX_CPUS; oc++) {
            uint64_t slot = (uint64_t)&tf->vector;
            uint64_t dst = badvec_eu[oc].dst;
            if (badvec_eu[oc].pid >= 0 && dst && slot + 8 >= dst - 320 &&
                slot <= dst + 320) {
              panic_puts("  [BADVEC] enter_user_space copy cpu=");
              print_int_raw((int)oc);
              panic_puts(" pid=");
              print_int_raw((int)badvec_eu[oc].pid);
              panic_puts(" src=");
              panic_print_hex(badvec_eu[oc].src);
              panic_puts(" dst=");
              panic_print_hex(dst);
              panic_puts(" pc=");
              panic_print_hex(badvec_eu[oc].pc);
              panic_puts("\n");
            }
          }
        } else {
          panic_puts("[BADVEC] entry slots unchanged: frame=");
          panic_print_hex((uint64_t)tf);
          panic_puts(" v=");
          print_int_raw((int)tf->vector);
          panic_puts(" cs=");
          panic_print_hex(tf->cs);
          panic_puts(" entry_pid=");
          print_int_raw((int)st->pid);
          panic_puts(" cpu=");
          print_int_raw(st_cpu);
          panic_puts("\n");
        }
      }
      panic_puts("[KERNEL] FATAL: Exception in Kernel Mode! Vector: ");
      print_int_raw((int)tf->vector);
      panic_puts(" Error Code: ");
      panic_print_hex(tf->error_code);
      panic_puts("\n");
      panic_puts("  RIP: ");
      panic_print_hex(tf->elr);
      panic_puts("  RSP: ");
      panic_print_hex(((uint64_t*)tf)[38]);
      panic_puts("\n");
      panic_puts("  CS:  ");
      panic_print_hex(tf->cs);
      panic_puts("  SS:  ");
      panic_print_hex(tf->ss);
      panic_puts("  RFLAGS: ");
      panic_print_hex(tf->spsr);
      panic_puts("\n");
      panic_puts("  RAX: ");
      panic_print_hex(tf->regs[0]);
      panic_puts("  RBX: ");
      panic_print_hex(tf->regs[1]);
      panic_puts("  RCX: ");
      panic_print_hex(tf->regs[2]);
      panic_puts("  RDX: ");
      panic_print_hex(tf->regs[3]);
      panic_puts("\n");
      panic_puts("  RDI: ");
      panic_print_hex(tf->regs[5]);
      panic_puts("  RSI: ");
      panic_print_hex(tf->regs[4]);
      panic_puts("  RBP: ");
      panic_print_hex(tf->regs[6]);
      panic_puts("\n");
      panic_puts("  ERR: ");
      panic_print_hex(tf->error_code);
      panic_puts("  R8:  ");
      panic_print_hex(tf->regs[7]);
      panic_puts("  R9:  ");
      panic_print_hex(tf->regs[8]);
      panic_puts("  R10: ");
      panic_print_hex(tf->regs[9]);
      panic_puts("\n");
      panic_puts("  CPU: ");
      print_int_raw((int)get_cpuid());
      struct process *curproc = current_process();
      panic_puts("  pid: ");
      print_int_raw(curproc ? curproc->pid : -1);
      panic_puts("\n");
      /* Raw stack window: even when RIP is garbage, the return addresses
         on the stack tell where the fault came from. */
      uint64_t fsp = ((uint64_t*)tf)[38];
      if (fsp >= 0x00000000ULL && fsp < 0x80000000ULL) {
        for (int si = 0; si < 16; si++) {
          panic_puts("  STACK[");
          print_int_raw(si);
          panic_puts("]: ");
          panic_print_hex(((volatile uint64_t *)fsp)[si]);
          panic_puts("\n");
        }
      } else {
        panic_puts("  STACK: skipped (RSP outside identity-mapped RAM)\n");
      }
      panic_leave(); /* GB-097: the dump is over; anything after here returns */
      /* WK-3 (draft 0007, x64 mirror of the ARM EL1 fix): a kernel-mode
         #PF (vector 14) whose faulting address references USER address
         space while a process is current is normally the process's bug
         (the WK-3 fork's unclamped exit-diag stack walk read one VA past
         the main-stack region during teardown).  The kernel only touches
         user v2 pages after sys_user_range/vm_touch pre-commits them in
         the process AS (CR3 = process AS), so a kernel-mode #PF inside
         the user window is a real overrun: kill the offending process
         cleanly instead of halting the whole OS.  L9 (pagein) refines
         this: the v2 IMAGE region is now demand-paged, so a kernel path
         marshalling a user buffer can touch a not-yet-faulted LEGAL user
         page; materialize it with the same demand fill a user fault runs
         and let iretq retry (restoring the residency the eager loader
         used to provide).  Genuine overruns (holes / guards / prot
         violations) still kill below.  Only #PF has a trustworthy
         faulting address (CR2); every other kernel-mode vector keeps the
         raw dump + halt below. */
      if (tf->vector == 14 && cur) {
        uint64_t kcr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(kcr2));
        if (kcr2 >= USER_VA_BASE) {
          /* GB-057: a kernel-mode #PF at the SAME address that keeps being
             "handled" is a retry loop, not a fix: measured live, a CPU sat on
             the `rep movsq` inside enter_user_space (RIP 0x7003B1CF) for 14
             MINUTES re-executing it, with vm_handle_fault returning success each
             time.  Nothing counted the repeats, so nothing ever reported it.
             Cap it here and turn the silence into one record plus a kill. */
          static uint64_t kpf_last_cr2[MAX_CPUS];
          static uint32_t kpf_repeats[MAX_CPUS];
          uint32_t kc = get_cpuid();
          if (kc >= MAX_CPUS)
            kc = 0;
          uint32_t kpf_n;
          if (kcr2 == kpf_last_cr2[kc]) {
            kpf_n = ++kpf_repeats[kc];
          } else {
            kpf_last_cr2[kc] = kcr2;
            kpf_n = kpf_repeats[kc] = 1;
          }
          int kpf_looping = (kpf_n > 8);
          if (kpf_n == 9) {
            panic_puts("[KPFLOOP] kernel-mode #PF repeating: cpu=");
            print_int_raw((int)kc);
            panic_puts(" cr2=");
            panic_print_hex(kcr2);
            panic_puts(" rip=");
            panic_print_hex(tf->elr);
            panic_puts(" pid=");
            print_int_raw(cur ? (int)cur->pid : -1);
            panic_puts(" -> killing, the mapping is not advancing\n");
          }
          if (cur->as && !kpf_looping) {
            /* vm_handle_fault may do device I/O; the write-fallback to a
               read-only materialization is gated on a read fault (a real
               write to a read-only page must keep faulting -> kill). */
            const char *why = "PROT";
            int pf_w = (int)((tf->error_code >> 1) & 1);
            /* GB-057: ground truth for the resume #PF.
               inside a WRITABLE, covering region (prot=3, covers=1) and the page
               is simply not materialised, so this is ordinary demand paging that
               is not being resolved.  Read the PTE straight out of the current
               CR3 before and after the handler; a direct walk has no side
               effects, unlike vm_range_ok which would materialise the page and
               destroy the measurement. */
            extern int kpml_present(uint64_t va);
            int pte_before = kpml_present(kcr2);
            int kh = vm_handle_fault(process_group(cur), kcr2, 1, 0, &why);
            int pte_after = kpml_present(kcr2);
            {
              static int kpf_diag;
              if (kpf_diag < 5) {
                extern void uart_puts(const char *s);
                kpf_diag++;
                char db[144];
                int di = 0;
                const char *q1 = "[KPF-DIAG] cr2=0x";
                while (*q1 && di < 12) db[di++] = *q1++;
                static const char hx2[] = "0123456789ABCDEF";
                uint64_t xq = kcr2;
                for (int sh = 60; sh >= 0 && di < 30; sh -= 4)
                  db[di++] = hx2[(xq >> sh) & 0xF];
                const char *q2 = " pte_before=";
                while (*q2 && di < 45) db[di++] = *q2++;
                db[di++] = pte_before ? '1' : '0';
                const char *q3 = " handled=";
                while (*q3 && di < 58) db[di++] = *q3++;
                db[di++] = kh ? '0' : '1'; /* handled == (ret == 0) */
                const char *q4 = " pte_after=";
                while (*q4 && di < 72) db[di++] = *q4++;
                db[di++] = pte_after ? '1' : '0';
                const char *q5 = " why=";
                while (*q5 && di < 80) db[di++] = *q5++;
                while (*why && di < 138) db[di++] = *why++;
                db[di++] = '\n';
                db[di] = 0;
                uart_puts(db);
              }
            }
            if (kh == 0 ||
                (!pf_w && vm_handle_fault(process_group(cur), kcr2, 0, 0,
                                          &why) == 0)) {
              return; /* PTE live; iretq retries the faulting instruction */
            }
            /* L73: unresolved.  Name the path before the process dies. */
            kpf_user_report(tf, cur, kcr2, why);
          }
          panic_puts("[KERNEL] Kernel-mode #PF in user address space -> killing pid=");
          print_int_raw(cur->pid);
          panic_puts(" cr2=");
          panic_print_hex(kcr2);
          panic_puts(" rip=");
          panic_print_hex((uint64_t)tf->elr);
          panic_puts(" rsp=");
          panic_print_hex((uint64_t)tf->lr);
          panic_puts(" live=");
          print_int_raw(process_group(cur)->live_threads);
          panic_puts("\n");
          process_fault_exit(tf, 11); /* SIGSEGV status byte */
          /* process_fault_exit -> process_exit -> schedule() never
             returns (it resumes another process or idles forever). */
          while (1);
        }
      }
      /* GB-057: NEVER halt silently.
         This path is reached when the kernel cannot diagnose a kernel-mode
         fault.  The previous behaviour was a bare `while (1);` -- which left a
         CPU spinning forever with proc_lock held and not one byte on the
         console.  Five probe runs produced five different-looking freezes for
         exactly this reason: the fault was never the mystery, the silence was.
         The raw sink takes no lock (it cannot deadlock the console it is
         reporting on), so this costs nothing on the happy path and names the
         fault for whoever reads the log next. */
      panic_puts("[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=");
      print_int_raw((int)tf->vector);
      panic_puts(" rip=");
      panic_print_hex((uint64_t)tf->elr);
      panic_puts(" cr2=");
      {
        uint64_t fatal_cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fatal_cr2));
        panic_print_hex(fatal_cr2);
      }
      panic_puts(" pid=");
      print_int_raw(cur ? cur->pid : -1);
      /* GB-069 discriminator: a #PF at cr2=0x1FC001008 lands INSIDE the 2 MiB page
         cpu_pd7[480] installs (0x9B = P|RW|PWT|PCD|PS, VA 0x1FC000000-0x1FDFFFFF), so
         "the mapping is too small" is excluded by arithmetic.  The three survivors are
         PD-entry reuse, a wrong CR3, and a protection fault; these fields name which.
         err bit0=P (0 = not present), bit1=W/R, bit2=U/S.  pd480 should still equal
         (phys|0x9B) for the BAR page to exist in this table. */
      panic_puts(" err=");
      print_int_raw((int)(tf->error_code & 7));
      panic_puts(" cr3=");
      {
        uint64_t fatal_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(fatal_cr3));
        panic_print_hex(fatal_cr3);
      }
      panic_puts(" pd480=");
      {
        extern uint64_t cpu_pd7[512];
        panic_print_hex(cpu_pd7[480]);
      }
      panic_puts("\n");
      /* GB-060: same rule as the resume-return halt -- if this CPU is holding
         proc_lock, give it back before stopping, so the survivors are not left
         spinning behind a CPU that will never run again.  Only if the lock
         records us as owner: freeing somebody else's lock would be worse than
         the stall. */
      {
        extern int spinlock_release_if_held_by_me(spinlock_t *lock);
        extern spinlock_t proc_lock;
        if (spinlock_release_if_held_by_me(&proc_lock))
          panic_puts("[KERNEL] released proc_lock before halting (GB-060)\n");
      }
      /* GB-060: and record the halt, so a CPU waiting on any OTHER lock this one
         still holds can take it over rather than spinning behind a stopped core
         forever.  The release above only helps for proc_lock; this covers the
         rest (a blk_cache bucket was the last one measured). */
      {
        extern void cpu_halt_record(uint32_t cpu);
        extern uint32_t get_cpuid(void);
        cpu_halt_record(get_cpuid());
      }
      while (1);
    }
  }

  /* P5 S3 (D8.4a): the vector 0x80 (syscall) and 0x81 (yield) branches
     fall through to here — the leader's return-to-user boundary.  A no-op
     unless a deliverable handler signal is pending (D8.5: never while
     sig_in_handler; kernel-mode frames never deliver). */
  process_signal_trap_exit(tf);
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
  /* GB-152: mark this core as having run init, so the unit suite measures the
     cores that exist instead of hardcoding four.  AP bring-up is sequential
     (0, 1, 2, ...), so smp_init_count() also bounds the id range it asserts. */
  cpu_locals[cpu].initialized = 1;

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

/* GB-152: how many cores have actually run trap_init_core_with_id.  The T2
 * suite runs before the scheduler, so on a 1-CPU run this is 1 — the old test
 * hardcoded four and asserted cpu_id == i for cores that had not been brought
 * up, which is the red baseline this card fixes. */
uint32_t smp_init_count(void) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < MAX_CPUS; i++)
    if (cpu_locals[i].initialized) n++;
  return n;
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
EXCEPTION_NO_ERR(20);EXCEPTION_ERR(21);   EXCEPTION_NO_ERR(22);EXCEPTION_NO_ERR(23);
EXCEPTION_NO_ERR(24);EXCEPTION_NO_ERR(25);EXCEPTION_NO_ERR(26);EXCEPTION_NO_ERR(27);
EXCEPTION_NO_ERR(28);EXCEPTION_ERR(29);   EXCEPTION_ERR(30);   EXCEPTION_NO_ERR(31);

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
#ifdef PANIC_IPI_TEST
EXCEPTION_NO_ERR(131); // 0x83 (GB-207 two-CPU panic-injection vehicle)
#endif

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
"    \n"
"    /* GB-123: the frame check must not push into the frame it inspects.\n"
"       Its two saves and the call's return address used to land at\n"
"       [rsp+264]/[rsp+272]/[rsp+256] -- the vector, error and spsr slots\n"
"       (measured live: [EXITFRAME] spsr_slot=0x7003D87F).  Push below the\n"
"       frame, run the check, then advance past the frame. */\n"
"    push rax\n"
"    push rcx\n"
"    lea rdi, [rsp + 296]\n"
"    call trap_exit_frame_check\n"
"    pop rcx\n"
"    pop rax\n"
"    add rsp, 264\n"
"    add rsp, 16\n"
"    /* restore the process's rdi from its saved slot: the frame_check\n"\
"       call above clobbered rdi to &frame, and iretq hands the user that\n"\
"       kernel-frame address (kstack_top-0x28) unless reloaded -> user\n"\
"       faults at the trap frame's own RIP slot (0x755xDBxx). */\n"\
"    mov rdi, [rsp - 240]\n"
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
"    /* GB-146-A: record the frame-copy window (source frame, target SP) for\n"
"       the fatal frame dump before any C call clobbers rdi/rsi. */\n"
"    mov rdi, r12\n"
"    mov rsi, r13\n"
"    call enter_user_space_note\n"
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
"    /* User-mode resume: NEVER write below the resumed user RSP.\n"
"       [target_sp-320, target_sp) is exactly the interrupted function's\n"
"       128-byte psABI red zone plus the head of its frame, so 296 bytes of\n"
"       trap-frame registers written there corrupts LIVE locals of a leaf\n"
"       function that a timer tick happened to interrupt.  The corruption is\n"
"       deterministic (the values are one fixed trap frame): the wild-pointer\n"
"       deaths show the same garbage register in every run of one workload.\n"
"       The iretq frame does not need to live in user memory at all - iretq\n"
"       loads RSP from the frame's own RSP slot - so the scratch lives on THIS\n"
"       CPU's kernel stack, where IF=0 makes the copy interrupt-atomic.  r13\n"
"       (target_sp) is still what the tail writes into the frame's RSP slot. */\n"
"    mov rax, rsp\n"
"    sub rax, 384\n"
"    and rax, -16\n"
"    mov rdi, rax\n"
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
"    /* GB-123: see the common_trap_exit note -- the check pushes below the\n"
"       frame, never into it; advance past the frame only afterwards. */\n"
"    push rax\n"
"    push rcx\n"
"    lea rdi, [rsp + 296]\n"
"    call trap_exit_frame_check\n"
"    pop rcx\n"
"    pop rax\n"
"    add rsp, 264\n"
"    add rsp, 16\n"
"    /* restore the process's rdi from its saved slot: the frame_check\n"\
"       call above clobbered rdi to &frame, and iretq hands the user that\n"\
"       kernel-frame address (kstack_top-0x28) unless reloaded -> user\n"\
"       faults at the trap frame's own RIP slot (0x755xDBxx). */\n"\
"    mov rdi, [rsp - 240]\n"
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
#ifdef PANIC_IPI_TEST
  // GB-207: test vehicle vector 0x83 (kernel-only gate, 0x8E).
  idt_set_gate(131, (uint64_t)exception_131, 0x08, 0x8E);
#endif
}

#ifdef KERNEL_MODE_UNIT_TEST
/* L73: the path-taking entry points are static (one handler per syscall, never
 * a table).  The unit suite drives them through this shim so a hole or a
 * straddling path can be asserted to answer -EFAULT instead of walking the
 * kernel into the hole past the end of the caller's mapping. */
void l73_test_entry(struct trap_frame *tf, int which) {
  switch (which) {
  case 0: sys_open(tf); break;
  case 1: sys_mkdir(tf); break;
  case 2: sys_chdir(tf); break;
  case 3: sys_unlink(tf); break;
  case 4: sys_rmdir(tf); break;
  case 5: sys_rename(tf); break;
  case 6: sys_spawn(tf); break;
  default: tf->regs[0] = (uint64_t)(int64_t)-1; break;
  }
}
#endif
