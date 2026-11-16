/*
 * src/user/linux_probe_exitgroup.c - C1-k4: exit_group(2) and SIGKILL of a
 * group whose other threads are RUNNING in user mode on other CPUs
 * (docs/cp-k4.md §6).
 *
 * Linux stops every thread before the address space goes away.  The HobbyOS
 * exit_group path marked the members THREAD_DONE and waited for 32 wfi
 * wakeups for their CPUs to leave them -- without kicking those CPUs -- and
 * then tore the group down while a member could still be running in it
 * ("[THREADS] exit_group claim drain timeout"; C4-x2 P4 run 8: a winedevice
 * thread faulted "in=V1 live=0" in its own r-x text after its group was
 * gone, and other Wine processes then read garbage).  A thread that runs on
 * after its frames were freed writes into whoever gets them next.
 *
 * ORDINARY musl program; same ELF on strix (Linux) and in the guest.
 *
 *  X1  40 times: a child with 7 threads that spin WRITING a 4 MiB private
 *      buffer calls exit_group from main; the parent reaps status 0
 *  X2  40 times: the same child is SIGKILLed instead; the parent reaps 9
 *  X3  all the while a checker process keeps mapping fresh anonymous pages,
 *      filling them with its own pattern and verifying them: any foreign byte
 *      is a frame written after it was freed
 *  X4  a child whose threads sit in a SIGUSR1 handler loop (Wine's SIGQUIT
 *      handler shape) when main calls exit_group
 *  X5  20 times: the child's threads alternate a LONG syscall (an 8 MiB
 *      pwrite to a /tmp file) with bumping a MAP_SHARED counter; once the
 *      parent has reaped the child, the counter must never move again (Linux:
 *      every thread is dead before the exit status is reported)
 *  X6  free memory after X1..X5 is within 96 MiB of free memory before: a
 *      dying group's address space is released, not leaked
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

#define BUF (4u << 20)
static volatile uint8_t *buf;
static volatile int up;

static void *writer(void *v) {
  uint8_t tag = (uint8_t)(0xA0 + (int)(intptr_t)v);
  __sync_fetch_and_add(&up, 1);
  for (uint64_t i = (uint64_t)(intptr_t)v * 4096;; i = (i + 4099) % BUF) buf[i] = tag;
  return 0;
}

static volatile int in_h;
static void h(int s) {
  (void)s;
  in_h = 1;
  for (volatile unsigned long i = 0;; i++) buf[i % BUF] = 0xEE; /* never returns */
}
static void *sig_target(void *v) {
  (void)v;
  __sync_fetch_and_add(&up, 1);
  for (;;) buf[0] = 1;
  return 0;
}

/* mode 0: exit_group, 1: wait to be killed, 2: threads in a signal handler */
static void child(int mode) {
  buf = mmap(0, BUF, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  for (uint32_t i = 0; i < BUF; i += 4096) buf[i] = 1;
  pthread_t t[7];
  pid_t tids[7] = {0};
  (void)tids;
  if (mode == 2) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigaction(SIGUSR1, &sa, 0);
    for (int i = 0; i < 7; i++) pthread_create(&t[i], 0, sig_target, 0);
    while (up < 7) {}
    for (int i = 0; i < 7; i++) pthread_kill(t[i], SIGUSR1);
    struct timespec ts = {0, 5 * 1000000};
    nanosleep(&ts, 0);
  } else {
    for (int i = 0; i < 7; i++) pthread_create(&t[i], 0, writer, (void *)(intptr_t)i);
    while (up < 7) {}
  }
  if (mode == 1)
    for (;;) pause();
  syscall(SYS_exit_group, 0);
}

static volatile uint64_t *shared_ctr;
static void *long_sys(void *v) {
  int fd = (int)(intptr_t)v;
  static uint8_t big[8u << 20];
  __sync_fetch_and_add(&up, 1);
  for (;;) {
    (void)!pwrite(fd, big, sizeof big, 0);
    __sync_fetch_and_add(shared_ctr, 1);
  }
  return 0;
}
static void child_long(void) {
  int fd = open("/tmp/k4xg.bin", 0102 /* O_RDWR|O_CREAT */, 0644);
  pthread_t t;
  for (int i = 0; i < 6; i++) pthread_create(&t, 0, long_sys, (void *)(intptr_t)fd);
  while (up < 6) {}
  while (*shared_ctr < 12) {}
  syscall(SYS_exit_group, 0);
}

static void checker(int wfd) {
  long bad = 0, pages = 0;
  for (int round = 0; round < 1000000; round++) {
    uint8_t *m = mmap(0, 1u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) continue;
    uint8_t pat = (uint8_t)(round * 7 + 1);
    if (pat >= 0xA0) pat = (uint8_t)(pat & 0x7F);
    memset(m, pat, 1u << 20);
    for (int spin = 0; spin < 3; spin++)
      for (uint32_t i = 0; i < (1u << 20); i += 64)
        if (m[i] != pat) { bad++; break; }
    pages += 256;
    munmap(m, 1u << 20);
    char c;
    if (read(wfd, &c, 1) == 1) break; /* parent says stop */
  }
  char msg[64];
  int n = snprintf(msg, sizeof msg, "%ld %ld", bad, pages);
  (void)!write(1, "", 0);
  _exit(bad ? 1 : 0);
  (void)msg; (void)n;
}

int main(void) {
  int fails = 0;
  int stop[2];
  if (pipe2(stop, 04000 /* O_NONBLOCK */)) return 1;
  pid_t ck = fork();
  if (ck == 0) { close(stop[1]); checker(stop[0]); }
  close(stop[0]);
  int bad1 = 0, bad2 = 0, bad4 = 0;
  struct sysinfo si0, si1;
  sysinfo(&si0);
  for (int it = 0; it < 40; it++) {
    pid_t k = fork();
    if (k == 0) child(0);
    int st = 0;
    if (waitpid(k, &st, 0) != k || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      if (!bad1) out("[LP] info X1 iter %d status=0x%x\n", it, st);
      bad1++;
    }
  }
  out("[LP] %s X1 exit_group with 7 writing threads x40 (%d bad)\n", bad1 ? "FAIL" : "PASS", bad1);
  for (int it = 0; it < 40; it++) {
    pid_t k = fork();
    if (k == 0) child(1);
    struct timespec ts = {0, (3 + it % 5) * 1000000L};
    nanosleep(&ts, 0);
    kill(k, SIGKILL);
    int st = 0;
    if (waitpid(k, &st, 0) != k || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
      if (!bad2) out("[LP] info X2 iter %d status=0x%x\n", it, st);
      bad2++;
    }
  }
  out("[LP] %s X2 SIGKILL with 7 writing threads x40 (%d bad)\n", bad2 ? "FAIL" : "PASS", bad2);
  for (int it = 0; it < 20; it++) {
    pid_t k = fork();
    if (k == 0) child(2);
    int st = 0;
    if (waitpid(k, &st, 0) != k || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      if (!bad4) out("[LP] info X4 iter %d status=0x%x\n", it, st);
      bad4++;
    }
  }
  out("[LP] %s X4 exit_group with 7 threads in a signal handler x20 (%d bad)\n", bad4 ? "FAIL" : "PASS", bad4);
  shared_ctr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  int bad5 = 0;
  for (int it = 0; it < 20 && shared_ctr != MAP_FAILED; it++) {
    *shared_ctr = 0;
    pid_t k = fork();
    if (k == 0) child_long();
    int st5 = 0;
    waitpid(k, &st5, 0);
    uint64_t c1 = *shared_ctr;
    struct timespec ts = {0, 300 * 1000000L};
    nanosleep(&ts, 0);
    uint64_t c2 = *shared_ctr;
    if (c2 != c1 || !WIFEXITED(st5)) {
      if (!bad5) out("[LP] info X5 iter %d: counter %llu at reap, %llu 300 ms later (status 0x%x)\n", it,
                     (unsigned long long)c1, (unsigned long long)c2, st5);
      bad5++;
    }
  }
  unlink("/tmp/k4xg.bin");
  out("[LP] %s X5 no thread runs after its group was reaped x20 (%d bad)\n", bad5 ? "FAIL" : "PASS", bad5);
  (void)!write(stop[1], "x", 1);
  int st = 0;
  waitpid(ck, &st, 0);
  int ok3 = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  out("[LP] %s X3 checker saw no foreign byte in its fresh pages (status 0x%x)\n", ok3 ? "PASS" : "FAIL", st);
  struct timespec tw = {1, 0};
  nanosleep(&tw, 0);
  sysinfo(&si1);
  long long f0 = (long long)si0.freeram * si0.mem_unit, f1 = (long long)si1.freeram * si1.mem_unit;
  int ok6 = f0 - f1 < (96LL << 20);
  out("[LP] %s X6 free memory before %lld MiB, after %lld MiB (drop %lld MiB)\n", ok6 ? "PASS" : "FAIL", f0 >> 20,
      f1 >> 20, (f0 - f1) >> 20);
  fails = (bad1 > 0) + (bad2 > 0) + (bad4 > 0) + !ok3 + (bad5 > 0) + !ok6;
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
