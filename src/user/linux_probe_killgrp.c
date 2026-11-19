/*
 * src/user/linux_probe_killgrp.c — signals to a thread group whose LEADER
 * thread has exited (C1-k6, docs/cp-k6.md §3; C8-game's P8 stop 7 and
 * src/user/cp/p8killprobe.c K1/K2).
 *
 * ORDINARY musl Linux program (runs on the strix host and in the guest).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 * Each case forks a child whose main thread starts worker threads and then
 * pthread_exit()s: the group lives on without its leader thread (Linux keeps
 * the leader as a zombie task; the pid still names the process).
 *
 *   K1/K2  kill(child, SIGKILL): reaped within 10 s, status = killed by 9
 *          (the workers block in read() on a pipe nobody writes)
 *   K3     kill(child, 0) answers 0 while the group lives
 *   K4     kill(child, SIGTERM) with SIG_DFL: terminate, status = 15
 *   K5     kill(child, SIGUSR1) with a handler: a worker runs it (the worker
 *          then exit_group(42)s), status = exited 42
 *   K6     a worker calls exit_group(7) by itself: status = exited 7
 *   K7     the last worker returns (pthread_exit): status = exited 0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[LP] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}
static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
}

static int pfd[2];
static int mode;
static volatile int got_usr1;

static void *blocker(void *arg) {
  (void)arg;
  char c;
  for (;;) {
    if (mode == 5 && got_usr1) syscall(SYS_exit_group, 42);
    ssize_t r = read(pfd[0], &c, 1);
    if (r < 0 && errno != EINTR) break;
    if (r == 0) break;
  }
  return 0;
}
static void *exiter(void *arg) {
  (void)arg;
  sleep_ms(300);
  syscall(SYS_exit_group, 7);
  return 0;
}
static void *returner(void *arg) {
  (void)arg;
  sleep_ms(300);
  return 0;
}
static void on_usr1(int s) {
  (void)s;
  got_usr1 = 1;
}

/* fork a leader-exited group in `m`; returns the child pid */
static pid_t spawn(int m) {
  if (pipe(pfd) != 0) return -1;
  pid_t pid = fork();
  if (pid != 0) return pid;
  mode = m;
  pthread_t t;
  if (m == 5) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1; /* no SA_RESTART: the read returns EINTR */
    sigaction(SIGUSR1, &sa, 0);
  }
  void *(*fn)(void *) = m == 6 ? exiter : m == 7 ? returner : blocker;
  for (int i = 0; i < 2; i++)
    if (pthread_create(&t, 0, fn, 0) != 0) _exit(3);
  pthread_exit(0); /* the leader goes; the workers keep the group alive */
}

static int reap(pid_t pid, int *st, int limit_ms) {
  for (int waited = 0; waited < limit_ms; waited += 50) {
    pid_t w = waitpid(pid, st, WNOHANG);
    if (w == pid) return waited;
    if (w < 0) return -1;
    sleep_ms(50);
  }
  return -2;
}

static void verdict(const char *id, const char *what, int ok, const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (!ok) fails++;
  row("%s %-40s %s -> %s", id, what, b, ok ? "PASS" : "FAIL");
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int st = 0, w;
  pid_t pid;

  /* K1/K2/K3: SIGKILL */
  pid = spawn(1);
  sleep_ms(500);
  int k0 = kill(pid, 0);
  verdict("K3", "kill(pid,0) on a leader-exited group", k0 == 0, "rc=%d errno=%d", k0,
          k0 ? errno : 0);
  int k = kill(pid, SIGKILL);
  w = reap(pid, &st, 10000);
  verdict("K1", "SIGKILL: the group is reaped", k == 0 && w >= 0, "kill rc=%d waited=%dms", k, w);
  verdict("K2", "SIGKILL: status is killed-by-9", w >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "status=0x%x", st);
  close(pfd[0]);
  close(pfd[1]);

  /* K4: SIGTERM, default action */
  pid = spawn(4);
  sleep_ms(500);
  k = kill(pid, SIGTERM);
  w = reap(pid, &st, 10000);
  verdict("K4", "SIGTERM (SIG_DFL) terminates it", k == 0 && w >= 0 && WIFSIGNALED(st) &&
          WTERMSIG(st) == SIGTERM, "kill rc=%d waited=%dms status=0x%x", k, w, st);
  close(pfd[0]);
  close(pfd[1]);

  /* K5: a handled signal goes to a live worker */
  pid = spawn(5);
  sleep_ms(500);
  k = kill(pid, SIGUSR1);
  w = reap(pid, &st, 10000);
  if (w < 0) { kill(pid, SIGKILL); reap(pid, &st, 5000); }
  verdict("K5", "SIGUSR1 (handler) runs on a worker", k == 0 && w >= 0 && WIFEXITED(st) &&
          WEXITSTATUS(st) == 42, "kill rc=%d waited=%dms status=0x%x", k, w, st);
  close(pfd[0]);
  close(pfd[1]);

  /* K6: exit_group from a worker */
  pid = spawn(6);
  w = reap(pid, &st, 10000);
  if (w < 0) { kill(pid, SIGKILL); reap(pid, &st, 5000); }
  verdict("K6", "a worker's exit_group(7)", w >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7,
          "waited=%dms status=0x%x", w, st);
  close(pfd[0]);
  close(pfd[1]);

  /* K7: the last worker returns */
  pid = spawn(7);
  w = reap(pid, &st, 10000);
  if (w < 0) { kill(pid, SIGKILL); reap(pid, &st, 5000); }
  verdict("K7", "the last thread returns", w >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "waited=%dms status=0x%x", w, st);
  close(pfd[0]);
  close(pfd[1]);

  row("done rc=%d", fails);
  return fails;
}
