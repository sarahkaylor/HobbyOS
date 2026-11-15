/*
 * src/user/linux_probe_join.c — L42: the thread-exit / joiner wake contract.
 *
 * ORDINARY musl Linux program: no HobbyOS header, so the same static-PIE
 * binary runs on the workstation (reference kernel) and inside the intel
 * guest (HobbyOS's compat layer).  Every row is "[LP] ..." and the last is
 * "[LP] done rc=<fails>"; tools/gate/t3_linux_probe.sh join only accepts rc=0.
 *
 * The orderings that decide whether a joiner can be stranded:
 *
 *   A  waiter parks FIRST, then the signaller stores its flag and wakes:
 *      futex(WAIT) must return 0 and the wake must report one waiter.
 *   B  the signaller stores and broadcasts BEFORE the "joiner" looks:
 *      the reader must see the flag and not park at all (a wake that finds
 *      nobody must not be required for correctness).
 *   C  CLONE_CHILD_CLEARTID: the kernel itself must zero the tid word and
 *      wake a parent already parked on it (pthread_join's contract).
 *   D  pthread_join called AFTER the thread has already exited: the tid word
 *      is already 0, so join must reap and return, never park.
 *   E  clone WITHOUT CLONE_CHILD_CLEARTID: the kernel must NOT clear the
 *      word after the child exits (only the explicit wake signals), which
 *      proves C's clear came from the flag and not from every exit.  Issued
 *      as a RAW clone(2): musl's clone() wrapper refuses CLONE_THREAD.
 *
 * A failure in C/D is the "lost wakeup on thread exit" shape: the joiner
 * sleeps forever because the exit path's clear+broadcast never reached it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int fails;

/* ONE write(2) per row.  The old fputs/vprintf/fputc triple was three
   writes on an unbuffered stdout, and a kernel console line emitted between
   them landed inside the row ("[LP] [IPI] ..." then "A park-first ..."), so
   the gate's '^\[LP\]' grep lost the row. */
static void row(const char *fmt, ...) {
  char b[400], out[420];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  int m = snprintf(out, sizeof out, "[LP] %s\n", b);
  if (m > 0) (void)!write(1, out, (size_t)(m < (int)sizeof out ? m : (int)sizeof out - 1));
}

static void msleep(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static int fut_wait(volatile uint32_t *p, uint32_t val) {
  return (int)syscall(SYS_futex, (void *)p, FUTEX_WAIT, val, NULL, NULL, 0);
}
static int fut_wake(volatile uint32_t *p, int n) {
  return (int)syscall(SYS_futex, (void *)p, FUTEX_WAKE, n, NULL, NULL, 0);
}

/* ---- A/B/D: a pthread, the shape a real signaller/joiner has ---------- */
static volatile uint32_t flag_;
static volatile int wake_count_;

static void *setter(void *arg) {
  (void)arg;
  msleep(50);
  __atomic_store_n(&flag_, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&wake_count_, fut_wake(&flag_, 1), __ATOMIC_RELEASE);
  return NULL;
}

static void case_a(void) {
  pthread_t t;
  flag_ = 0;
  wake_count_ = -1;
  pthread_create(&t, NULL, setter, NULL);
  int r = fut_wait(&flag_, 0); /* park FIRST */
  uint32_t f = __atomic_load_n(&flag_, __ATOMIC_ACQUIRE);
  /* The setter stores wake_count_ AFTER its futex(WAKE) returns, and the
     woken waiter can run before that store: reading it here was the row's
     own race (measured: wait=0 flag=1 wake=-1, with the kernel's WAKE record
     saying woke=1).  The join orders the read after the store. */
  pthread_join(t, NULL);
  int w = __atomic_load_n(&wake_count_, __ATOMIC_ACQUIRE);
  row("A park-first wait=%d wake=%d flag=%u", r, w, f);
  if (r != 0 || w != 1 || f != 1)
    fails++;
}

static void case_b(void) {
  pthread_t t;
  flag_ = 0;
  wake_count_ = -1;
  pthread_create(&t, NULL, setter, NULL);
  msleep(150); /* the store+broadcast happens before we look */
  int r = 0;
  if (__atomic_load_n(&flag_, __ATOMIC_ACQUIRE) == 0)
    r = fut_wait(&flag_, 0); /* must NOT park: the flag is already set */
  row("B wake-before-look wait=%d flag=%u", r, flag_);
  if (flag_ != 1 || r != 0)
    fails++;
  pthread_join(t, NULL);
}

/* ---- C/E: raw clone so the CLEARTID flag is under our control -------- */
static void raw_sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  syscall(SYS_nanosleep, &ts, NULL);
}

struct child_arg {
  volatile uint32_t *go;
  volatile int *mark;
};

static int child_fn(void *a) {
  struct child_arg *c = (struct child_arg *)a;
  if (c->mark)
    __atomic_store_n(c->mark, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < 2000; i++) {
    if (!c->go || __atomic_load_n(c->go, __ATOMIC_ACQUIRE))
      break;
    raw_sleep_ms(1);
  }
  syscall(SYS_exit, 0);
  return 0;
}

/* join_raw_clone(flags, stack, ptid, ctid, fn, arg): glibc clone.S's shape.
   The child pops fn/arg off its new stack, calls fn(arg), exit(2)s. */
long join_raw_clone(unsigned long flags, void *stack, volatile uint32_t *ptid,
                    volatile uint32_t *ctid, int (*fn)(void *), void *arg);
__asm__(".text\n"
        ".globl join_raw_clone\n"
        ".type join_raw_clone,@function\n"
        "join_raw_clone:\n"
        "  and $-16, %rsi\n"
        "  sub $16, %rsi\n"
        "  mov %r9, 8(%rsi)\n"
        "  mov %r8, 0(%rsi)\n"
        "  mov %rcx, %r10\n"
        "  xor %r8d, %r8d\n"
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
        ".size join_raw_clone, .-join_raw_clone\n");

static void *new_stack(size_t *sz) {
  *sz = 256 * 1024;
  return malloc(*sz);
}

static volatile int c_ran;
static void *slow_setter(void *arg) {
  (void)arg;
  msleep(120); /* the joiner is already parked in pthread_join */
  c_ran = 1;
  return NULL;
}

static void case_c(void) {
  struct timespec t0, t1;
  pthread_t t;
  c_ran = 0;
  pthread_create(&t, NULL, slow_setter, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int r = pthread_join(t, NULL); /* park BEFORE the signaller exits */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L +
            (t1.tv_nsec - t0.tv_nsec) / 1000L;
  row("C join-park-first rc=%d us=%ld ran=%d", r, us, c_ran);
  if (r != 0 || !c_ran || us < 50000)
    fails++;
}

static void case_d(void) {
  struct timespec t0, t1;
  pthread_t t;
  flag_ = 0;
  pthread_create(&t, NULL, setter, NULL);
  msleep(200); /* the thread has stored, woken and exited */
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int r = pthread_join(t, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L +
            (t1.tv_nsec - t0.tv_nsec) / 1000L;
  row("D join-after-exit rc=%d us=%ld", r, us);
  if (r != 0 || us > 200000)
    fails++;
}

static void case_e(void) {
  /* SETTID without CLEARTID: after the child exits the word must still name
     it.  No kernel clear means no kernel wake either; the explicit flag is
     the only signal, and it must arrive. */
  static volatile uint32_t ctid;
  ctid = 0;
  volatile uint32_t go = 0;
  volatile int mark = 0;
  struct child_arg a = {&go, &mark};
  size_t sz;
  char *st = new_stack(&sz);
  /* RAW clone(2): musl 1.2.5's clone() wrapper refuses CLONE_THREAD in
     user space with EINVAL (measured on the reference kernel too: "E clone
     failed errno=22" on Linux 7.0), so the wrapper never reached any kernel. */
  long pid = join_raw_clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                                CLONE_THREAD | CLONE_SYSVSEM | CLONE_CHILD_SETTID,
                            st + sz, NULL, &ctid, child_fn, &a);
  if (pid < 0) {
    row("E clone failed errno=%ld", -pid);
    fails++;
    free(st);
    return;
  }
  msleep(50);
  __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
  msleep(250);
  int ok = mark == 1 && ctid == (uint32_t)pid;
  row("E nocleartid pid=%ld ctid=%u mark=%d %s", pid, ctid, mark,
      ok ? "ok" : "FAIL");
  if (!ok)
    fails++;
  /* stack leaked on purpose: without CLEARTID nothing says it is free */
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  row("join probe start");
  case_a();
  case_b();
  case_c();
  case_d();
  case_e();
  row("done rc=%d", fails);
  return fails ? 1 : 0;
}
