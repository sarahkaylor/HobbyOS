/*
 * src/user/linux_probe_execthr.c - C1-k4: execve(2) from a secondary thread
 * (Linux de_thread: the exec'ing thread becomes the leader, tid == pid,
 * docs/cp-k4.md §4).
 *
 * ORDINARY musl Linux program; same static-PIE ELF on strix (Linux) and in
 * the HobbyOS guest.  Every row is ONE write(2); last row "[LP] done rc=N".
 *
 *  E1  a worker thread (tid != pid) execs this program again while the main
 *      thread and a third thread are busy: the new image sees getpid() equal
 *      to the old pid and gettid() == getpid()
 *  E2  the new image is single-threaded (/proc/self/status Threads: 1 when
 *      readable; otherwise a second exec-from-thread round must also work)
 *  E3  the new image's parent is unchanged (getppid() == the parent's pid),
 *      and the parent reaps it under the ORIGINAL pid with exit status 0
 *  E4  a signal mask set by the exec'ing thread survives the exec (Linux
 *      keeps the thread's blocked set)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static char self[512];
static char arg_pid[32], arg_ppid[32], arg_round[8];

static void find_self(const char *argv0) {
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n > 0) {
    self[n] = 0;
    return;
  }
  snprintf(self, sizeof self, "%s", argv0[0] == '/' ? argv0 : "/PROBE.ELF");
}

static void *spinner(void *v) {
  (void)v;
  for (volatile unsigned long i = 0;; i++) {}
  return 0;
}
static void *execer(void *v) {
  (void)v;
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &s, 0);
  char *av[] = {self, "child", arg_pid, arg_ppid, arg_round, 0};
  char *ev[] = {0};
  execve(self, av, ev);
  out("[LP] FAIL E1 execve from thread errno=%d\n", errno);
  _exit(9);
}

static int threads_now(void) {
  int fd = open("/proc/self/status", O_RDONLY);
  if (fd < 0) return -1;
  char b[2048];
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return -1;
  b[n] = 0;
  char *t = strstr(b, "Threads:");
  return t ? atoi(t + 8) : -1;
}

/* The exec'd image: report, and for round 1 exec again from a thread. */
static int child_main(int argc, char **argv) {
  if (argc < 5) return 8;
  int want_pid = atoi(argv[2]), want_ppid = atoi(argv[3]), round = atoi(argv[4]);
  int pid = getpid(), tid = (int)syscall(SYS_gettid), ppid = getppid();
  int fails = 0;
  int ok = pid == want_pid && tid == pid;
  out("[LP] %s E1 round %d: after exec-from-thread pid=%d (was %d) tid=%d\n", ok ? "PASS" : "FAIL", round, pid,
      want_pid, tid);
  fails += !ok;
  int th = threads_now();
  ok = th == 1 || th == -1;
  out("[LP] %s E2 round %d: Threads=%d (-1 = no /proc/self/status)\n", ok ? "PASS" : "FAIL", round, th);
  fails += !ok;
  ok = ppid == want_ppid;
  out("[LP] %s E3 round %d: getppid=%d (want %d)\n", ok ? "PASS" : "FAIL", round, ppid, want_ppid);
  fails += !ok;
  sigset_t cur;
  sigprocmask(SIG_BLOCK, 0, &cur);
  ok = sigismember(&cur, SIGUSR2) == 1;
  out("[LP] %s E4 round %d: SIGUSR2 still blocked=%d\n", ok ? "PASS" : "FAIL", round, sigismember(&cur, SIGUSR2));
  fails += !ok;
  if (fails) return 1;
  if (round == 1) {
    /* round 2: exec from a thread of the exec'd image */
    snprintf(arg_round, sizeof arg_round, "2");
    snprintf(arg_pid, sizeof arg_pid, "%s", argv[2]);
    snprintf(arg_ppid, sizeof arg_ppid, "%s", argv[3]);
    pthread_t a, b;
    pthread_create(&a, 0, spinner, 0);
    pthread_create(&b, 0, execer, 0);
    pthread_join(b, 0);
    return 7; /* not reached */
  }
  return 0;
}

int main(int argc, char **argv) {
  find_self(argv[0]);
  if (argc > 1 && strcmp(argv[1], "child") == 0) return child_main(argc, argv);
  int me = getpid();
  pid_t k = fork();
  if (k == 0) {
    snprintf(arg_pid, sizeof arg_pid, "%d", getpid());
    snprintf(arg_ppid, sizeof arg_ppid, "%d", me);
    snprintf(arg_round, sizeof arg_round, "1");
    pthread_t a, b;
    pthread_create(&a, 0, spinner, 0);
    pthread_create(&b, 0, execer, 0);
    for (;;) pause(); /* the main thread is busy elsewhere when the worker execs */
  }
  int st = 0;
  pid_t w = waitpid(k, &st, 0);
  int ok = w == k && WIFEXITED(st) && WEXITSTATUS(st) == 0;
  out("[LP] %s E3 parent reaped pid %d (forked %d) status=0x%x\n", ok ? "PASS" : "FAIL", (int)w, (int)k, st);
  out("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
