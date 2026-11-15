/*
 * src/user/linux_probe_thr.c — C1-thr: the clone() flag sets, thread exit and
 * the thread-directed signal contract that glibc 2.31's NPTL and Wine 11 need.
 *
 * ORDINARY musl Linux program (no HobbyOS header): the same static-PIE ELF
 * runs on the reference kernel (strix, Linux 7.0) and in the HobbyOS guest
 * (tools/gate/t3_linux_probe.sh thr), so the two logs diff row by row.  Every
 * row is ONE write(2) ("[LP] ...\n"), so a kernel console line can never land
 * inside it.  The last row is "[LP] done rc=<fails>".
 *
 * Rows (G = graded, R = report-only: Linux accepts it, HobbyOS refuses it on
 * purpose, see docs/cp-thr.md):
 *
 *  K  thread clones through a RAW clone(2) (musl 1.2.5's clone() wrapper
 *     refuses CLONE_THREAD with EINVAL in user space, which is what
 *     linux_probe_join row E used to measure):
 *     K1 G glibc 2.31 pthread_create  0x3D0F00 (VM|FS|FILES|SIGHAND|THREAD|
 *          SYSVSEM|SETTLS|PARENT_SETTID|CHILD_CLEARTID, ptid == ctid)
 *     K2 G musl 1.2.5 pthread_create  0x7D0F00 (K1 + DETACHED)
 *     K3 G join row E's shape: VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|CHILD_SETTID
 *     K4 R VM|SIGHAND|THREAD (no FS/FILES: Linux gives the thread its own fd
 *          table and fs; HobbyOS keeps those per group and refuses)
 *  X  Linux's own refusals:
 *     X1 G VM|THREAD without SIGHAND            -> EINVAL
 *     X2 G SIGHAND without VM                   -> EINVAL
 *     X3 G FS|NEWNS                             -> EINVAL
 *     X4 G PIDFD|PARENT_SETTID (legacy clone)   -> EINVAL
 *     X5 G thread clone, SETTLS >= TASK_SIZE    -> EPERM
 *     X6 R VM|SIGHAND|SIGCHLD (LinuxThreads)    Linux accepts, HobbyOS EINVAL
 *     X7 R VM|SIGCHLD (shared-VM process, no VFORK) same
 *  F/V fork and vfork shapes:
 *     F1 G glibc fork() 0x1200011 (CHILD_SETTID|CHILD_CLEARTID|SIGCHLD)
 *     F2 G fork(57)
 *     V1 G vfork(58): child writes a shared word and exits; the parent sees it
 *     V2 G glibc posix_spawn's clone(VM|VFORK|SIGCHLD, stack): the child runs
 *          on its own stack in the SAME memory, the parent resumes after it
 *     V3 G posix_spawn("/NOSUCH") reports ENOENT
 *  R  robust futexes and exit:
 *     R1 G set_robust_list(len != 24) -> EINVAL; get_robust_list round-trips
 *     R2 G a thread dies holding a robust lock with a waiter parked:
 *          the kernel writes OWNER_DIED|WAITERS and wakes the waiter
 *     R3 G a process SIGKILLed while holding robust lists: new threads on the
 *          reused pids start with get_robust_list == NULL
 *     G1 G exit_group(42) from a non-leader thread ends the whole group
 *     G2 G the leader exits (exit(60)) first; a worker's exit_group(43) is the
 *          group's status
 *  L  latency from tgkill(SIGUSR1) to handler entry on a thread parked in
 *     futex / nanosleep / rt_sigsuspend / read(pipe), and on a spinning
 *     thread; plus what the interrupted call did afterwards (futex and read
 *     restart under SA_RESTART, nanosleep and sigsuspend answer EINTR)
 *  S  one-thread delivery:
 *     S1 G tgkill reaches exactly the chosen thread
 *     S2 G a tgkill'd signal the target blocks stays pending ON THAT THREAD
 *          (nobody else takes it) and runs when it unblocks
 *     S3 G a kill(2) is taken by the one thread that does not block it; with
 *          every thread blocking it, it waits for the first that unblocks
 *     S4 G Wine's suspend: SA_SIGINFO|SA_RESTART|SA_ONSTACK SIGUSR1 handler
 *          that blocks (futex, then read(pipe)) while a second SIGUSR1 is
 *          queued; resume, re-delivery, the interrupted futex restarts
 *     S5 G SIGQUIT to one thread whose handler exit(2)s it: only that thread
 *          dies, CLEARTID wakes its joiner, tgkill to it is then ESRCH
 *     S6 G a handler that blocks in rt_sigsuspend for a second signal
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define L_ARCH_GET_FS 0x1003

static int fails;

static void row(const char *fmt, ...) {
  char b[480];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
  char out[500];
  int m = snprintf(out, sizeof out, "[LP] %.*s\n", n, b);
  if (m > 0) (void)!write(1, out, (size_t)(m < (int)sizeof out ? m : (int)sizeof out - 1));
}

static void grade(int ok) {
  if (!ok) fails++;
}

/* ---- raw syscalls (no errno, no TLS) ----------------------------------- */
static long rsys(long n, long a, long b, long c, long d, long e, long f) {
  long r;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "=a"(r)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return r;
}

static long long now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void msleep(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static int fut_wait_to(volatile uint32_t *w, uint32_t v, int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  return (int)rsys(SYS_futex, (long)w, FUTEX_WAIT, v, ms >= 0 ? (long)&ts : 0, 0, 0);
}
static int fut_wake(volatile uint32_t *w, int n) {
  return (int)rsys(SYS_futex, (long)w, FUTEX_WAKE, n, 0, 0, 0);
}

/* Wait (bounded) for *p to become nonzero / reach v. */
static int wait_ge(volatile int *p, int v, int ms) {
  long long dl = now_us() + (long long)ms * 1000;
  while (__atomic_load_n(p, __ATOMIC_ACQUIRE) < v) {
    if (now_us() > dl) return 0;
    struct timespec ts = {0, 200000};
    nanosleep(&ts, 0);
  }
  return 1;
}

static int gettid_(void) { return (int)rsys(SYS_gettid, 0, 0, 0, 0, 0, 0); }

/* ---- a raw clone: thr_raw_clone(flags, stack, ptid, ctid, tls, fn, arg) ---
 * The child pops fn/arg off its new stack, calls fn(arg) and exit(2)s with
 * its return value -- glibc's clone.S shape.  stack must be non-zero. */
long thr_raw_clone(unsigned long flags, void *stack, volatile uint32_t *ptid,
                   volatile uint32_t *ctid, unsigned long tls,
                   int (*fn)(void *), void *arg);
__asm__(".text\n"
        ".globl thr_raw_clone\n"
        ".type thr_raw_clone,@function\n"
        "thr_raw_clone:\n"
        "  mov 8(%rsp), %rax\n"
        "  and $-16, %rsi\n"
        "  sub $16, %rsi\n"
        "  mov %rax, 8(%rsi)\n"
        "  mov %r9, 0(%rsi)\n"
        "  mov %rcx, %r10\n"
        "  mov $56, %eax\n"
        "  syscall\n"
        "  test %rax, %rax\n"
        "  jnz 1f\n"
        "  xor %ebp, %ebp\n"
        "  pop %r9\n"
        "  pop %rdi\n"
        "  call *%r9\n"
        "  mov %eax, %edi\n"
        "  mov $60, %eax\n"
        "  syscall\n"
        "  hlt\n"
        "1: ret\n"
        ".size thr_raw_clone, .-thr_raw_clone\n");

/* vfork(58): the child stores 1 into vf_shared and exit_group(5)s without
   ever returning from this function (it shares the parent's stack). */
static volatile int vf_shared;
static long raw_vfork_child_exit5(void) {
  long r;
  __asm__ volatile("mov $58, %%eax\n"
                   "syscall\n"
                   "test %%rax, %%rax\n"
                   "jnz 1f\n"
                   "movl $1, %1\n"
                   "mov $5, %%edi\n"
                   "mov $231, %%eax\n"
                   "syscall\n"
                   "hlt\n"
                   "1:\n"
                   : "=&a"(r), "=m"(vf_shared)
                   :
                   : "rcx", "r11", "rdi", "memory");
  return r;
}

static void *new_stack(size_t sz) {
  void *p = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}

/* A fake x86-64 TCB: word 0 points at itself (what %fs:0 must hold); the
   canary slot at 0x28 is readable.  Only raw syscalls run on it. */
static uint64_t fake_tcb[4][64] __attribute__((aligned(64)));

struct kid {
  volatile int mark;
  volatile int tid_seen;
  volatile unsigned long fs_seen;
  volatile int done;
  int exit_code;
  int sleep_ms;
};

static int kid_fn(void *a) {
  struct kid *k = (struct kid *)a;
  k->tid_seen = gettid_();
  unsigned long fs = 0;
  rsys(SYS_arch_prctl, L_ARCH_GET_FS, (long)&fs, 0, 0, 0, 0);
  k->fs_seen = fs;
  __atomic_store_n(&k->mark, 1, __ATOMIC_RELEASE);
  if (k->sleep_ms) {
    struct timespec ts = {k->sleep_ms / 1000, (long)(k->sleep_ms % 1000) * 1000000L};
    rsys(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
  }
  __atomic_store_n(&k->done, 1, __ATOMIC_RELEASE);
  return k->exit_code;
}

#define THR_BASE                                                               \
  (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |          \
   CLONE_SYSVSEM)

/* K1/K2: the NPTL shape -- ptid == ctid == the descriptor's tid word, the
   joiner parks on it until the kernel's exit-time clear+wake. */
static void k_nptl(const char *name, unsigned long flags, int tcb) {
  static volatile uint32_t word;
  struct kid k = {0};
  k.sleep_ms = 80; /* the joiner must be parked when the clear arrives */
  size_t sz = 128 * 1024;
  char *st = new_stack(sz);
  fake_tcb[tcb][0] = (uint64_t)(uintptr_t)&fake_tcb[tcb][0];
  word = 0xFFFFFFFFu;
  long tid = thr_raw_clone(flags, st + sz, &word, &word,
                           (unsigned long)(uintptr_t)&fake_tcb[tcb][0], kid_fn, &k);
  if (tid <= 0) {
    row("%s flags=0x%lx clone=%ld FAIL", name, flags, tid);
    grade(0);
    return;
  }
  uint32_t ptid_seen = word;
  int parks = 0, woke0 = 0, again = 0, other = 0;
  long long t0 = now_us();
  for (int i = 0; i < 400; i++) {
    uint32_t v = __atomic_load_n(&word, __ATOMIC_ACQUIRE);
    if (v == 0) break;
    int r = fut_wait_to(&word, v, 2000);
    parks++;
    if (r == 0) woke0++;
    else if (r == -EAGAIN) again++;
    else other++;
  }
  long long dt = now_us() - t0;
  wait_ge(&k.mark, 1, 500);
  int ok = ptid_seen == (uint32_t)tid && k.mark && k.tid_seen == tid &&
           k.fs_seen == (unsigned long)(uintptr_t)&fake_tcb[tcb][0] &&
           word == 0 && woke0 >= 1 && other == 0;
  row("%s flags=0x%lx tid=%ld ptid=%u child_tid=%d fs_ok=%d ctid_after=%u "
      "parks=%d woke=%d eagain=%d other=%d join_us=%lld %s",
      name, flags, tid, ptid_seen, k.tid_seen,
      k.fs_seen == (unsigned long)(uintptr_t)&fake_tcb[tcb][0], word, parks,
      woke0, again, other, dt, ok ? "ok" : "FAIL");
  grade(ok);
  munmap(st, sz);
}

/* K3: CHILD_SETTID without CLEARTID: the word names the child after it
   exits (only CLEARTID clears it). */
static void k3_settid(void) {
  static volatile uint32_t ctid;
  struct kid k = {0};
  size_t sz = 128 * 1024;
  char *st = new_stack(sz);
  ctid = 0;
  unsigned long fl = THR_BASE | CLONE_CHILD_SETTID;
  long tid = thr_raw_clone(fl, st + sz, 0, &ctid, 0, kid_fn, &k);
  if (tid <= 0) {
    row("K3 joinE-settid flags=0x%lx clone=%ld FAIL", fl, tid);
    grade(0);
    return;
  }
  wait_ge(&k.done, 1, 1000);
  msleep(100); /* well past its exit(2) */
  int ok = k.mark && ctid == (uint32_t)tid && k.tid_seen == tid;
  row("K3 joinE-settid flags=0x%lx tid=%ld ctid_after_exit=%u mark=%d %s", fl,
      tid, ctid, k.mark, ok ? "ok" : "FAIL");
  grade(ok);
  /* stack leaked on purpose: no CLEARTID, so nothing says it is free */
}

static void k4_minimal(void) {
  struct kid k = {0};
  size_t sz = 64 * 1024;
  char *st = new_stack(sz);
  unsigned long fl = CLONE_VM | CLONE_SIGHAND | CLONE_THREAD;
  long tid = thr_raw_clone(fl, st + sz, 0, 0, 0, kid_fn, &k);
  if (tid > 0) wait_ge(&k.done, 1, 1000);
  row("K4 minimal flags=0x%lx clone=%ld ran=%d (report-only: Linux accepts; "
      "HobbyOS keeps fs/files per group and refuses)",
      fl, tid, k.mark);
}

/* X rows: the clone must fail with `want` and create nothing. */
static void x_reject(const char *name, unsigned long flags, unsigned long tls,
                     long want, int graded) {
  static volatile uint32_t pt;
  struct kid k = {0};
  size_t sz = 64 * 1024;
  char *st = new_stack(sz);
  pt = 0;
  long r = thr_raw_clone(flags, st + sz, &pt, 0, tls, kid_fn, &k);
  if (r > 0) {
    wait_ge(&k.done, 1, 500);
    if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD)) waitpid((int)r, 0, __WALL);
  }
  int ok = r == want;
  row("%s flags=0x%lx -> %ld want %ld %s", name, flags, r, want,
      graded ? (ok ? "ok" : "FAIL") : "(report-only)");
  if (graded) grade(ok);
}

/* ---- F/V: fork and vfork shapes ---------------------------------------- */
static int reap(int pid, int ms) {
  long long dl = now_us() + (long long)ms * 1000;
  int st = 0;
  for (;;) {
    int r = waitpid(pid, &st, WNOHANG);
    if (r == pid) return st;
    if (r < 0) return -2;
    if (now_us() > dl) return -1;
    msleep(2);
  }
}

static void f1_glibc_fork(void) {
  static volatile uint32_t ctid;
  ctid = 0;
  unsigned long fl = CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD;
  long pid = rsys(SYS_clone, (long)fl, 0, 0, (long)&ctid, 0, 0);
  if (pid == 0) {
    long me = rsys(SYS_gettid, 0, 0, 0, 0, 0, 0);
    rsys(SYS_exit_group, ctid == (uint32_t)me ? 7 : 8, 0, 0, 0, 0, 0);
  }
  int st = pid > 0 ? reap((int)pid, 3000) : -3;
  int ok = pid > 0 && st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7 &&
           ctid == 0;
  row("F1 glibc-fork flags=0x%lx pid=%ld status=0x%x parent_ctid=%u %s", fl,
      pid, st, ctid, ok ? "ok" : "FAIL");
  grade(ok);
}

static void f2_fork57(void) {
  long pid = rsys(57, 0, 0, 0, 0, 0, 0);
  if (pid == 0) rsys(SYS_exit_group, 9, 0, 0, 0, 0, 0);
  int st = pid > 0 ? reap((int)pid, 3000) : -3;
  int ok = pid > 0 && st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 9;
  row("F2 fork57 pid=%ld status=0x%x %s", pid, st, ok ? "ok" : "FAIL");
  grade(ok);
}

static void v1_vfork58(void) {
  vf_shared = 0;
  long pid = raw_vfork_child_exit5();
  int seen = vf_shared;
  int st = pid > 0 ? reap((int)pid, 3000) : -3;
  int ok = pid > 0 && seen == 1 && st >= 0 && WIFEXITED(st) &&
           WEXITSTATUS(st) == 5;
  row("V1 vfork58 pid=%ld shared_seen=%d status=0x%x %s", pid, seen, st,
      ok ? "ok" : "FAIL");
  grade(ok);
}

static volatile int v2_err, v2_child_pid;
static int v2_child(void *a) {
  (void)a;
  v2_child_pid = gettid_();
  /* glibc's __spawni_child writes args->err on failure; this is that write */
  v2_err = 42;
  return 6; /* -> exit(60) of the only thread: the process exits 6 */
}

static void v2_spawn_shape(void) {
  size_t sz = 64 * 1024;
  char *st = new_stack(sz);
  v2_err = 0;
  v2_child_pid = 0;
  unsigned long fl = CLONE_VM | CLONE_VFORK | SIGCHLD;
  long pid = thr_raw_clone(fl, st + sz, 0, 0, 0, v2_child, 0);
  int err_seen = v2_err; /* the parent reads it right after clone returns */
  int cp = v2_child_pid;
  int s = pid > 0 ? reap((int)pid, 3000) : -3;
  int ok = pid > 0 && err_seen == 42 && cp == pid && s >= 0 && WIFEXITED(s) &&
           WEXITSTATUS(s) == 6;
  row("V2 spawn-shape flags=0x%lx pid=%ld err_seen=%d child_pid=%d "
      "status=0x%x %s",
      fl, pid, err_seen, cp, s, ok ? "ok" : "FAIL");
  grade(ok);
  munmap(st, sz);
}

static void v3_posix_spawn_enoent(void) {
  pid_t pid = -1;
  char *argv[] = {"/NOSUCH.ELF", 0};
  char *envp[] = {0};
  int r = posix_spawn(&pid, "/NOSUCH.ELF", 0, 0, argv, envp);
  int ok = r == ENOENT;
  row("V3 posix_spawn-enoent r=%d pid=%d %s", r, (int)pid, ok ? "ok" : "FAIL");
  grade(ok);
  if (r == 0 && pid > 0) reap(pid, 2000);
}

/* ---- R: robust futexes -------------------------------------------------- */
struct robust_head {
  void *next;
  long futex_offset;
  void *pending;
};
struct robust_lock {
  volatile uint32_t word;
  uint32_t pad;
  void *next; /* the list entry: lock word is at entry - 8 */
};

static void r1_robust_abi(void) {
  static struct robust_head h;
  h.next = &h;
  h.futex_offset = 0;
  h.pending = 0;
  long bad = rsys(SYS_set_robust_list, (long)&h, 23, 0, 0, 0, 0);
  long good = rsys(SYS_set_robust_list, (long)&h, sizeof h, 0, 0, 0, 0);
  void *got = 0;
  size_t len = 0;
  long g = rsys(SYS_get_robust_list, 0, (long)&got, (long)&len, 0, 0, 0);
  int ok = bad == -EINVAL && good == 0 && g == 0 && got == &h && len == 24;
  row("R1 robust-abi set(23)=%ld set(24)=%ld get=%ld head_ok=%d len=%zu %s",
      bad, good, g, got == &h, len, ok ? "ok" : "FAIL");
  grade(ok);
}

static struct robust_head r2_head;
static struct robust_lock r2_lock;
static volatile uint32_t r2_ctid;
static int r2_kid(void *a) {
  struct kid *k = (struct kid *)a;
  r2_head.next = &r2_lock.next;
  r2_head.futex_offset = -(long)offsetof(struct robust_lock, next);
  r2_head.pending = 0;
  r2_lock.next = &r2_head; /* circular: back to the head */
  rsys(SYS_set_robust_list, (long)&r2_head, sizeof r2_head, 0, 0, 0, 0);
  r2_lock.word = (uint32_t)gettid_(); /* "acquired" */
  __atomic_store_n(&k->mark, 1, __ATOMIC_RELEASE);
  struct timespec ts = {0, 80 * 1000000L};
  rsys(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
  return 0; /* dies holding it */
}

static void r2_robust_death(void) {
  struct kid k = {0};
  size_t sz = 64 * 1024;
  char *st = new_stack(sz);
  r2_lock.word = 0;
  r2_ctid = 1;
  fake_tcb[2][0] = (uint64_t)(uintptr_t)&fake_tcb[2][0];
  long tid = thr_raw_clone(THR_BASE | CLONE_SETTLS | CLONE_CHILD_CLEARTID,
                           st + sz, 0, &r2_ctid,
                           (unsigned long)(uintptr_t)&fake_tcb[2][0], r2_kid, &k);
  if (tid <= 0) {
    row("R2 robust-death clone=%ld FAIL", tid);
    grade(0);
    return;
  }
  wait_ge(&k.mark, 1, 1000);
  uint32_t owner = r2_lock.word;
  __atomic_or_fetch(&r2_lock.word, 0x80000000u, __ATOMIC_SEQ_CST); /* WAITERS */
  uint32_t v = r2_lock.word;
  int r = fut_wait_to(&r2_lock.word, v, 2000);
  uint32_t after = r2_lock.word;
  for (int i = 0; i < 100 && r2_ctid; i++) fut_wait_to(&r2_ctid, r2_ctid, 50);
  int ok = owner == (uint32_t)tid && r == 0 && after == 0xC0000000u;
  row("R2 robust-death owner=%u wait=%d word_after=0x%x (want 0xC0000000) %s",
      owner, r, after, ok ? "ok" : "FAIL");
  grade(ok);
}

/* R3: a robust list must not outlive its thread's pid.  A process killed
   with SIGKILL never runs the thread-exit path that drops its robust head;
   HobbyOS reuses pids at once, so a NEW thread on that pid must still start
   with no robust list (Linux: every clone child's list is NULL). */
static struct robust_head r3_head_a, r3_head_b;
static volatile uint32_t r3_park;
static int r3_child_thread(void *a) {
  (void)a;
  rsys(SYS_set_robust_list, (long)&r3_head_b, sizeof r3_head_b, 0, 0, 0, 0);
  for (;;) rsys(SYS_futex, (long)&r3_park, FUTEX_WAIT, 0, 0, 0, 0);
  return 0;
}

#define R3_N 8
static volatile int r3_tid[R3_N];
static volatile long r3_head_seen[R3_N];
static volatile int r3_done[R3_N];
static volatile uint32_t r3_go;
static volatile uint32_t r3_ctid[R3_N];
static int r3_probe_thread(void *a) {
  int i = (int)(intptr_t)a;
  void *h = (void *)1;
  size_t len = 0;
  r3_tid[i] = gettid_();
  long r = rsys(SYS_get_robust_list, 0, (long)&h, (long)&len, 0, 0, 0);
  r3_head_seen[i] = r == 0 ? (long)(uintptr_t)h : -1;
  __atomic_store_n(&r3_done[i], 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&r3_go, __ATOMIC_ACQUIRE))
    rsys(SYS_futex, (long)&r3_go, FUTEX_WAIT, 0, 0, 0, 0);
  return 0;
}

static void r3_robust_pid_reuse(void) {
  int sync[2];
  pipe(sync);
  pid_t pid = fork();
  if (pid == 0) {
    rsys(SYS_set_robust_list, (long)&r3_head_a, sizeof r3_head_a, 0, 0, 0, 0);
    size_t sz = 64 * 1024;
    char *st = new_stack(sz);
    long t = thr_raw_clone(THR_BASE, st + sz, 0, 0, 0, r3_child_thread, 0);
    int dead[2] = {(int)rsys(SYS_getpid, 0, 0, 0, 0, 0, 0), (int)t};
    msleep(30); /* the thread has set its list */
    (void)!write(sync[1], dead, sizeof dead);
    for (;;) rsys(SYS_futex, (long)&r3_park, FUTEX_WAIT, 0, 0, 0, 0);
  }
  int dead[2] = {0, 0};
  (void)!read(sync[0], dead, sizeof dead);
  kill(pid, SIGKILL);
  int st = reap(pid, 3000);
  msleep(50);
  /* R3_N live threads take the lowest free pids, the dead ones among them. */
  r3_go = 0;
  char *stk[R3_N];
  for (int i = 0; i < R3_N; i++) {
    r3_done[i] = 0;
    r3_tid[i] = 0;
    r3_ctid[i] = 1;
    stk[i] = new_stack(64 * 1024);
    thr_raw_clone(THR_BASE | CLONE_CHILD_CLEARTID, stk[i] + 64 * 1024, 0,
                  &r3_ctid[i], 0, r3_probe_thread, (void *)(intptr_t)i);
  }
  int bad = 0, reused = 0, seen = 0;
  for (int i = 0; i < R3_N; i++) {
    if (!wait_ge(&r3_done[i], 1, 2000)) continue;
    seen++;
    if (r3_head_seen[i] != 0) bad++;
    if (r3_tid[i] == dead[0] || r3_tid[i] == dead[1]) reused++;
  }
  __atomic_store_n(&r3_go, 1, __ATOMIC_RELEASE);
  fut_wake(&r3_go, R3_N);
  for (int i = 0; i < R3_N; i++)
    for (int k = 0; k < 100 && r3_ctid[i]; k++)
      fut_wait_to(&r3_ctid[i], r3_ctid[i], 20);
  for (int i = 0; i < R3_N; i++) munmap(stk[i], 64 * 1024);
  close(sync[0]);
  close(sync[1]);
  int ok = st >= 0 && seen == R3_N && bad == 0;
  row("R3 robust-pid-reuse killed=%d,%d status=0x%x threads=%d reused_dead_pid=%d "
      "stale_heads=%d %s",
      dead[0], dead[1], st, seen, reused, bad, ok ? "ok" : "FAIL");
  grade(ok);
}

/* ---- G: exit_group from a non-leader ------------------------------------ */
static void *g1_worker(void *a) {
  msleep(30);
  syscall(SYS_exit_group, (int)(intptr_t)a);
  return 0;
}

static void g1_exit_group_worker(void) {
  long long t0 = now_us();
  pid_t pid = fork();
  if (pid == 0) {
    pthread_t t;
    int pr = pthread_create(&t, 0, g1_worker, (void *)(intptr_t)42);
    if (pr) syscall(SYS_exit_group, 100 + pr); /* status names the errno */
    static volatile uint32_t never;
    fut_wait_to(&never, 0, 5000); /* the leader parks; the worker ends us */
    syscall(SYS_exit_group, 99);
  }
  int st = pid > 0 ? reap(pid, 4000) : -3;
  long long ms = (now_us() - t0) / 1000;
  int ok = st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 42 && ms < 3000;
  row("G1 exit_group-from-worker status=0x%x ms=%lld %s", st, ms,
      ok ? "ok" : "FAIL");
  grade(ok);
}

static void g2_leader_first(void) {
  pid_t pid = fork();
  if (pid == 0) {
    pthread_t t;
    int pr = pthread_create(&t, 0, g1_worker, (void *)(intptr_t)43);
    if (pr) syscall(SYS_exit_group, 100 + pr);
    syscall(SYS_exit, 0); /* the LEADER thread leaves; the group lives on */
  }
  int st = pid > 0 ? reap(pid, 4000) : -3;
  int ok = st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 43;
  row("G2 leader-exits-first status=0x%x %s", st, ok ? "ok" : "FAIL");
  grade(ok);
}

/* ---- signals ------------------------------------------------------------ */
#define NSLOT 8
static volatile int slot_tid[NSLOT];
static volatile int hits[NSLOT][65];
static volatile long long hit_us[NSLOT][65];

static int slot_of(int tid) {
  for (int i = 0; i < NSLOT; i++)
    if (slot_tid[i] == tid) return i;
  return -1;
}

static void on_sig(int sig, siginfo_t *si, void *uc) {
  (void)si;
  (void)uc;
  int s = slot_of(gettid_());
  if (s < 0) s = NSLOT - 1;
  hit_us[s][sig] = now_us();
  __atomic_add_fetch(&hits[s][sig], 1, __ATOMIC_SEQ_CST);
}

static void set_handler(int sig, void (*fn)(int, siginfo_t *, void *),
                        int flags, const sigset_t *mask) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = fn;
  sa.sa_flags = SA_SIGINFO | flags;
  if (mask) sa.sa_mask = *mask;
  else sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, 0);
}

static int tgkill_(int tid, int sig) {
  return (int)rsys(SYS_tgkill, getpid(), tid, sig, 0, 0, 0);
}

static void clear_hits(void) {
  for (int i = 0; i < NSLOT; i++)
    for (int s = 0; s < 65; s++) hits[i][s] = 0;
}

enum { PK_FUTEX, PK_SLEEP, PK_SUSPEND, PK_READ, PK_SPIN };
static const char *pk_name[] = {"futex", "nanosleep", "sigsuspend", "read", "spin"};

struct worker {
  pthread_t th;
  int slot;
  volatile int tid;
  volatile uint32_t cmd; /* futex word */
  volatile int kind;
  volatile int quit;
  volatile int parked; /* set just before entering the park */
  volatile int rets;   /* how many times the park call returned */
  volatile long last_ret;
  volatile int last_err;
  volatile int mask_req, mask_done; /* apply `mask` when req != done */
  sigset_t mask;
  int pfd[2];
};

static void *worker_main(void *a) {
  struct worker *w = (struct worker *)a;
  w->tid = gettid_();
  /* HobbyOS reuses tids at once (Linux does not): a dead worker's slot may
     still name this tid, and slot_of() takes the first match. */
  for (int i = 0; i < NSLOT; i++)
    if (i != w->slot && slot_tid[i] == w->tid) slot_tid[i] = 0;
  slot_tid[w->slot] = w->tid;
  while (!w->quit) {
    if (w->mask_req != w->mask_done) {
      pthread_sigmask(SIG_SETMASK, &w->mask, 0);
      __atomic_store_n(&w->mask_done, w->mask_req, __ATOMIC_RELEASE);
    }
    uint32_t seq = w->cmd;
    long r = 0;
    int e = 0;
    __atomic_store_n(&w->parked, 1, __ATOMIC_RELEASE);
    switch (w->kind) {
    case PK_FUTEX:
      r = rsys(SYS_futex, (long)&w->cmd, FUTEX_WAIT, seq, 0, 0, 0);
      if (r < 0) e = (int)-r;
      break;
    case PK_SLEEP: {
      struct timespec ts = {5, 0};
      r = nanosleep(&ts, 0);
      e = r ? errno : 0;
      break;
    }
    case PK_SUSPEND: {
      sigset_t m;
      sigemptyset(&m);
      r = sigsuspend(&m);
      e = errno;
      break;
    }
    case PK_READ: {
      char c;
      r = read(w->pfd[0], &c, 1);
      e = r < 0 ? errno : 0;
      break;
    }
    case PK_SPIN:
      while (w->cmd == seq && !w->quit) {
      }
      break;
    }
    w->parked = 0;
    w->last_ret = r;
    w->last_err = e;
    __atomic_add_fetch(&w->rets, 1, __ATOMIC_SEQ_CST);
  }
  return 0;
}

static void worker_start(struct worker *w, int slot, int kind) {
  memset(w, 0, sizeof *w);
  w->slot = slot;
  w->kind = kind;
  pipe(w->pfd);
  slot_tid[slot] = -1;
  pthread_create(&w->th, 0, worker_main, w);
  long long dl = now_us() + 2000000;
  while ((!w->tid || !w->parked) && now_us() < dl) msleep(1);
  msleep(30); /* inside the park, not just about to enter it */
}

static void worker_poke(struct worker *w) {
  __atomic_add_fetch(&w->cmd, 1, __ATOMIC_SEQ_CST);
  fut_wake(&w->cmd, 1);
  if (w->kind == PK_READ) (void)!write(w->pfd[1], "x", 1);
}

static void worker_set_mask(struct worker *w, const sigset_t *m) {
  w->mask = *m;
  int want = w->mask_req + 1;
  __atomic_store_n(&w->mask_req, want, __ATOMIC_RELEASE);
  int r0 = w->rets;
  worker_poke(w);
  wait_ge(&w->mask_done, want, 2000);
  wait_ge(&w->rets, r0 + 1, 2000);
  long long dl = now_us() + 2000000;
  while (!w->parked && now_us() < dl) msleep(1);
  msleep(20);
}

static void worker_stop(struct worker *w) {
  w->quit = 1;
  worker_poke(w);
  if (w->kind == PK_SLEEP || w->kind == PK_SUSPEND) tgkill_(w->tid, SIGUSR2);
  pthread_join(w->th, 0);
  close(w->pfd[0]);
  close(w->pfd[1]);
  slot_tid[w->slot] = 0;
}

/* L: tgkill -> handler-entry latency, per park kind. */
static void l_latency(int kind) {
  struct worker w;
  clear_hits();
  worker_start(&w, 1, kind);
  int rets0 = w.rets;
  long long t0 = now_us();
  int k = tgkill_(w.tid, SIGUSR1);
  int got = wait_ge(&hits[1][SIGUSR1], 1, 2000);
  long long lat = got ? hit_us[1][SIGUSR1] - t0 : -1;
  msleep(40); /* what did the interrupted call do afterwards? */
  int rets = w.rets - rets0;
  long ret = w.last_ret;
  int err = w.last_err;
  /* Linux: futex (no timeout) and read restart under SA_RESTART; nanosleep
     and sigsuspend return -1/EINTR after the handler. */
  int want_rets = (kind == PK_FUTEX || kind == PK_READ || kind == PK_SPIN) ? 0 : 1;
  int sem_ok = rets == want_rets &&
               (want_rets == 0 || (ret == -1 && err == EINTR));
  int ok = k == 0 && got && lat >= 0 && lat < 20000 && sem_ok;
  row("L-%s tgkill=%d latency_us=%lld handler_on=%d after: returned=%d ret=%ld "
      "errno=%d (want %s) %s",
      pk_name[kind], k, lat, got, rets, rets ? ret : 0, rets ? err : 0,
      want_rets ? "EINTR" : "restart", ok ? "ok" : "FAIL");
  grade(ok);
  worker_stop(&w);
}

/* S1/S2: thread-directed delivery and the per-thread mask. */
static void s1_s2_directed(void) {
  struct worker w[3];
  clear_hits();
  for (int i = 0; i < 3; i++) worker_start(&w[i], 1 + i, PK_FUTEX);
  /* S1 */
  tgkill_(w[1].tid, SIGUSR1);
  wait_ge(&hits[2][SIGUSR1], 1, 2000);
  tgkill_(w[2].tid, SIGQUIT);
  wait_ge(&hits[3][SIGQUIT], 1, 2000);
  msleep(50);
  int ok1 = hits[2][SIGUSR1] == 1 && hits[1][SIGUSR1] == 0 &&
            hits[3][SIGUSR1] == 0 && hits[0][SIGUSR1] == 0 &&
            hits[3][SIGQUIT] == 1 && hits[1][SIGQUIT] == 0 &&
            hits[2][SIGQUIT] == 0 && hits[0][SIGQUIT] == 0;
  row("S1 tgkill-one usr1:w0=%d w1=%d w2=%d main=%d quit:w0=%d w1=%d w2=%d "
      "main=%d %s",
      hits[1][SIGUSR1], hits[2][SIGUSR1], hits[3][SIGUSR1], hits[0][SIGUSR1],
      hits[1][SIGQUIT], hits[2][SIGQUIT], hits[3][SIGQUIT], hits[0][SIGQUIT],
      ok1 ? "ok" : "FAIL");
  grade(ok1);

  /* S2: w0 blocks SIGUSR1; a tgkill to it stays pending on w0 alone. */
  clear_hits();
  sigset_t m;
  sigemptyset(&m);
  sigaddset(&m, SIGUSR1);
  worker_set_mask(&w[0], &m);
  tgkill_(w[0].tid, SIGUSR1);
  msleep(100);
  int held = hits[1][SIGUSR1] == 0 && hits[2][SIGUSR1] == 0 &&
             hits[3][SIGUSR1] == 0 && hits[0][SIGUSR1] == 0;
  sigset_t pend;
  sigemptyset(&pend);
  sigpending(&pend); /* main's view: a thread-directed bit is not ours */
  int main_sees = sigismember(&pend, SIGUSR1);
  sigemptyset(&m);
  long long t0 = now_us();
  worker_set_mask(&w[0], &m);
  int got = wait_ge(&hits[1][SIGUSR1], 1, 2000);
  long long lat = got ? hit_us[1][SIGUSR1] - t0 : -1;
  msleep(30);
  int ok2 = held && !main_sees && got && hits[2][SIGUSR1] == 0 &&
            hits[3][SIGUSR1] == 0 && hits[0][SIGUSR1] == 0;
  row("S2 blocked-stays-on-thread held=%d main_sigpending=%d delivered_on_unblock=%d "
      "unblock_to_handler_us=%lld others=%d,%d,%d %s",
      held, main_sees, got, lat, hits[2][SIGUSR1], hits[3][SIGUSR1],
      hits[0][SIGUSR1], ok2 ? "ok" : "FAIL");
  grade(ok2);

  /* S3: process-directed.  main, w0 and w2 block SIGUSR2; w1 does not. */
  clear_hits();
  sigset_t b2, old;
  sigemptyset(&b2);
  sigaddset(&b2, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &b2, &old);
  worker_set_mask(&w[0], &b2);
  worker_set_mask(&w[2], &b2);
  kill(getpid(), SIGUSR2);
  int a1 = wait_ge(&hits[2][SIGUSR2], 1, 2000);
  msleep(30);
  int first_ok = a1 && hits[1][SIGUSR2] == 0 && hits[3][SIGUSR2] == 0 &&
                 hits[0][SIGUSR2] == 0;
  /* now every thread blocks it: the next kill waits for an unblocker */
  worker_set_mask(&w[1], &b2);
  clear_hits();
  kill(getpid(), SIGUSR2);
  msleep(100);
  int none = hits[1][SIGUSR2] == 0 && hits[2][SIGUSR2] == 0 &&
             hits[3][SIGUSR2] == 0 && hits[0][SIGUSR2] == 0;
  sigemptyset(&pend);
  sigpending(&pend);
  int proc_pending = sigismember(&pend, SIGUSR2);
  sigemptyset(&m);
  worker_set_mask(&w[2], &m);
  int a2 = wait_ge(&hits[3][SIGUSR2], 1, 2000);
  msleep(30);
  int ok3 = first_ok && none && proc_pending && a2 && hits[1][SIGUSR2] == 0 &&
            hits[2][SIGUSR2] == 0 && hits[0][SIGUSR2] == 0;
  row("S3 kill-process first_to_unblocked=%d all_blocked_held=%d "
      "sigpending_shows=%d taken_by_first_unblocker=%d %s",
      first_ok, none, proc_pending, a2, ok3 ? "ok" : "FAIL");
  grade(ok3);
  pthread_sigmask(SIG_SETMASK, &old, 0);
  worker_set_mask(&w[0], &m);
  worker_set_mask(&w[1], &m);
  for (int i = 0; i < 3; i++) worker_stop(&w[i]);
}

/* S4: Wine's suspend/resume (dlls/ntdll/unix/signal_x86_64.c usr1_handler +
   wait_suspend): SIGUSR1 with SA_SIGINFO|SA_RESTART|SA_ONSTACK and the
   server block set as sa_mask; the handler waits for the server. */
static volatile int s4_count, s4_in, s4_on_alt;
static volatile uint32_t s4_resume, s4_inword;
static int s4_pipe[2];
static char *s4_alt;
static size_t s4_alt_sz = 64 * 1024;
static volatile int s4_alt_tid;

static void s4_handler(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)si;
  (void)uc;
  char here;
  int n = __atomic_add_fetch(&s4_count, 1, __ATOMIC_SEQ_CST);
  if (&here >= s4_alt && &here < s4_alt + s4_alt_sz) s4_on_alt++;
  s4_in = 1;
  __atomic_add_fetch(&s4_inword, 1, __ATOMIC_SEQ_CST);
  fut_wake(&s4_inword, 1);
  if (n == 1) {
    while (__atomic_load_n(&s4_resume, __ATOMIC_ACQUIRE) == 0)
      fut_wait_to(&s4_resume, 0, 3000);
    s4_resume = 0;
  } else {
    char c;
    (void)!read(s4_pipe[0], &c, 1);
  }
  s4_in = 0;
}

static void *s4_thread(void *a) {
  struct worker *w = (struct worker *)a;
  stack_t ss;
  ss.ss_sp = s4_alt;
  ss.ss_size = s4_alt_sz;
  ss.ss_flags = 0;
  sigaltstack(&ss, 0);
  return worker_main(w);
}

static void s4_wine_suspend(void) {
  struct worker w;
  memset(&w, 0, sizeof w);
  w.slot = 1;
  w.kind = PK_FUTEX;
  pipe(w.pfd);
  pipe(s4_pipe);
  s4_alt = new_stack(s4_alt_sz);
  s4_count = s4_in = s4_on_alt = 0;
  s4_resume = 0;
  sigset_t server_block;
  sigemptyset(&server_block);
  int blk[] = {SIGALRM, SIGIO, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGCHLD, SIGQUIT};
  for (unsigned i = 0; i < sizeof blk / sizeof blk[0]; i++)
    sigaddset(&server_block, blk[i]);
  set_handler(SIGUSR1, s4_handler, SA_RESTART | SA_ONSTACK, &server_block);
  pthread_create(&w.th, 0, s4_thread, &w);
  long long dl = now_us() + 2000000;
  while ((!w.tid || !w.parked) && now_us() < dl) msleep(1);
  msleep(30);

  long long t0 = now_us();
  tgkill_(w.tid, SIGUSR1);
  int in1 = wait_ge(&s4_count, 1, 2000);
  long long lat = now_us() - t0;
  tgkill_(w.tid, SIGUSR1); /* blocked by sa_mask: must queue, not nest */
  msleep(60);
  int nested = s4_count != 1;
  int still_in = s4_in;
  __atomic_store_n(&s4_resume, 1, __ATOMIC_RELEASE);
  fut_wake(&s4_resume, 1);
  int in2 = wait_ge(&s4_count, 2, 2000); /* the queued one, at sigreturn */
  msleep(30);
  (void)!write(s4_pipe[1], "r", 1);
  dl = now_us() + 2000000;
  while (s4_in && now_us() < dl) msleep(1);
  msleep(40);
  int rets = w.rets; /* SA_RESTART: the futex the signal broke restarts */
  int ok = in1 && !nested && still_in && in2 && s4_count == 2 &&
           s4_on_alt == 2 && !s4_in && rets == 0 && lat < 20000;
  row("S4 wine-suspend first_us=%lld nested=%d held_in_handler=%d redelivered=%d "
      "count=%d on_altstack=%d futex_returned=%d %s",
      lat, nested, still_in, in2, s4_count, s4_on_alt, rets, ok ? "ok" : "FAIL");
  grade(ok);
  set_handler(SIGUSR1, on_sig, SA_RESTART, 0);
  w.quit = 1;
  worker_poke(&w);
  pthread_join(w.th, 0);
  slot_tid[w.slot] = 0;
  close(w.pfd[0]);
  close(w.pfd[1]);
  close(s4_pipe[0]);
  close(s4_pipe[1]);
}

/* S5: SIGQUIT kills ONE thread (Wine's quit_handler -> abort_thread). */
static volatile uint32_t s5_ctid, s5_park;
static void s5_quit(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)si;
  (void)uc;
  rsys(SYS_exit, 0, 0, 0, 0, 0, 0);
}
static int s5_kid(void *a) {
  struct kid *k = (struct kid *)a;
  k->tid_seen = gettid_();
  __atomic_store_n(&k->mark, 1, __ATOMIC_RELEASE);
  for (;;) rsys(SYS_futex, (long)&s5_park, FUTEX_WAIT, 0, 0, 0, 0);
  return 0;
}

static void s5_quit_one(void) {
  struct worker other;
  clear_hits();
  worker_start(&other, 2, PK_FUTEX);
  set_handler(SIGQUIT, s5_quit, 0, 0);
  struct kid k = {0};
  size_t sz = 64 * 1024;
  char *st = new_stack(sz);
  fake_tcb[3][0] = (uint64_t)(uintptr_t)&fake_tcb[3][0];
  s5_ctid = 0;
  s5_park = 0;
  long tid = thr_raw_clone(THR_BASE | CLONE_SETTLS | CLONE_PARENT_SETTID |
                               CLONE_CHILD_CLEARTID,
                           st + sz, &s5_ctid, &s5_ctid,
                           (unsigned long)(uintptr_t)&fake_tcb[3][0], s5_kid, &k);
  wait_ge(&k.mark, 1, 1000);
  msleep(30);
  int r = tid > 0 ? tgkill_((int)tid, SIGQUIT) : -1;
  int woke = 0;
  for (int i = 0; i < 100 && s5_ctid; i++)
    if (fut_wait_to(&s5_ctid, s5_ctid, 50) == 0) woke++;
  int cleared = s5_ctid == 0;
  msleep(50);
  int esrch = tid > 0 ? tgkill_((int)tid, SIGUSR1) : 0;
  set_handler(SIGQUIT, on_sig, SA_RESTART, 0);
  tgkill_(other.tid, SIGUSR1); /* the rest of the group still runs */
  int alive = wait_ge(&hits[2][SIGUSR1], 1, 2000);
  int ok = tid > 0 && r == 0 && cleared && esrch == -ESRCH && alive;
  row("S5 sigquit-one tid=%ld tgkill=%d ctid_cleared=%d joiner_woken=%d "
      "tgkill_after=%d (want %d) sibling_alive=%d %s",
      tid, r, cleared, woke, esrch, -ESRCH, alive, ok ? "ok" : "FAIL");
  grade(ok);
  worker_stop(&other);
}

/* S6: a handler that waits in rt_sigsuspend for a second signal. */
static volatile int s6_in, s6_ret, s6_err, s6_mask_same, s6_usr1_blocked;
static void s6_usr1(int sig, siginfo_t *si, void *uc) {
  (void)sig;
  (void)si;
  (void)uc;
  sigset_t before, after, wait;
  /* the kernel writes 8 bytes of musl's 128-byte sigset_t: clear the rest */
  sigemptyset(&before);
  sigemptyset(&after);
  pthread_sigmask(SIG_SETMASK, 0, &before);
  s6_usr1_blocked = sigismember(&before, SIGUSR1);
  s6_in = 1;
  sigfillset(&wait);
  sigdelset(&wait, SIGUSR2);
  s6_ret = sigsuspend(&wait);
  s6_err = errno;
  pthread_sigmask(SIG_SETMASK, 0, &after);
  s6_mask_same = memcmp(&before, &after, 8) == 0;
  s6_in = 2;
}

static void s6_handler_sigsuspend(void) {
  struct worker w;
  clear_hits();
  s6_in = 0;
  set_handler(SIGUSR1, s6_usr1, SA_RESTART, 0);
  worker_start(&w, 1, PK_FUTEX);
  tgkill_(w.tid, SIGUSR1);
  int in = wait_ge(&s6_in, 1, 2000);
  msleep(30);
  long long t0 = now_us();
  tgkill_(w.tid, SIGUSR2);
  int out = wait_ge(&s6_in, 2, 2000);
  long long lat = now_us() - t0;
  msleep(20);
  int ok = in && out && hits[1][SIGUSR2] == 1 && s6_ret == -1 &&
           s6_err == EINTR && s6_mask_same && s6_usr1_blocked && w.rets == 0;
  row("S6 handler-sigsuspend entered=%d resumed=%d usr2_on_thread=%d ret=%d "
      "errno=%d mask_restored=%d usr1_blocked_in_handler=%d resume_us=%lld %s",
      in, out, hits[1][SIGUSR2], s6_ret, s6_err, s6_mask_same, s6_usr1_blocked,
      lat, ok ? "ok" : "FAIL");
  grade(ok);
  set_handler(SIGUSR1, on_sig, SA_RESTART, 0);
  worker_stop(&w);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  row("thr probe start pid=%d", getpid());
  slot_tid[0] = gettid_();
  set_handler(SIGUSR1, on_sig, SA_RESTART, 0);
  set_handler(SIGUSR2, on_sig, SA_RESTART, 0);
  set_handler(SIGQUIT, on_sig, SA_RESTART, 0);

  k_nptl("K1 glibc-pthread", THR_BASE | CLONE_SETTLS | CLONE_PARENT_SETTID |
                                 CLONE_CHILD_CLEARTID, 0);
  k_nptl("K2 musl-pthread", THR_BASE | CLONE_SETTLS | CLONE_PARENT_SETTID |
                                CLONE_CHILD_CLEARTID | CLONE_DETACHED, 1);
  k3_settid();
  k4_minimal();

  x_reject("X1 thread-no-sighand", CLONE_VM | CLONE_THREAD, 0, -EINVAL, 1);
  x_reject("X2 sighand-no-vm", CLONE_SIGHAND | SIGCHLD, 0, -EINVAL, 1);
  x_reject("X3 fs-newns", CLONE_FS | CLONE_NEWNS | SIGCHLD, 0, -EINVAL, 1);
  x_reject("X4 pidfd-parent-settid", CLONE_PIDFD | CLONE_PARENT_SETTID | SIGCHLD,
           0, -EINVAL, 1);
  x_reject("X5 settls-noncanonical", THR_BASE | CLONE_SETTLS,
           0x0000800000000000UL, -EPERM, 1);
  x_reject("X6 linuxthreads-vm-sighand", CLONE_VM | CLONE_SIGHAND | SIGCHLD, 0,
           -EINVAL, 0);
  x_reject("X7 vm-process", CLONE_VM | SIGCHLD, 0, -EINVAL, 0);

  f1_glibc_fork();
  f2_fork57();
  v1_vfork58();
  v2_spawn_shape();
  v3_posix_spawn_enoent();

  r1_robust_abi();
  r2_robust_death();
  r3_robust_pid_reuse();
  g1_exit_group_worker();
  g2_leader_first();

  for (int k = PK_FUTEX; k <= PK_SPIN; k++) l_latency(k);
  s1_s2_directed();
  s4_wine_suspend();
  s5_quit_one();
  s6_handler_sigsuspend();

  row("done rc=%d", fails);
  return fails ? 1 : 0;
}
