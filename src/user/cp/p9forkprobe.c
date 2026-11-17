/* src/user/cp/p9forkprobe.c - lane C9-play (docs/cp-p9.md §4, stop P9-1):
 * how long a BIG process takes to start a child with fork() + execve().
 *
 * Why: in the game boots Cyberpunk spawns REDEngineErrorReporter.exe with
 * CreateProcess and waits for it.  Wine's NtCreateUserProcess forks the game
 * process on the unix side and execs the preloader.  Measured with Wine
 * +timestamp (gateA4): the reporter's first instruction runs 16.7 s after
 * the call; when it takes a little longer the game gives up and exits with
 * status 0 at launch + 20 s (integ7's "clean early exit", gateA3).  At that
 * point the game process has ~1300 regions, a ~140 GiB anonymous reservation
 * ([THRS] anon_mib=141058) and ~45 threads.
 *
 * The probe times fork -> child's first instruction after execve (the child
 * writes CLOCK_MONOTONIC to a pipe) and fork -> reaped, for a parent that is
 *   F1 small: nothing extra;
 *   F2a the 64 GiB PROT_NONE reservation alone; F2b the 1300 separate
 *      256 KiB anonymous mappings alone, each touched (325 MiB resident);
 *   F2 big VA: both;
 *   F3 big VA + 40 threads blocked in read() (the game's worker pool);
 *   F4 like F3 but vfork() + execve() (what posix_spawn does), for contrast.
 * Each case runs 5 times; rows print min/median/max in ms.  The row is a
 * FAIL when the median of F2/F3 is above 1000 ms (a Linux host: < 30 ms);
 * that is far inside the game's ~16 s budget, so a FAIL here means the
 * kernel's fork/exec cost scales with the parent's size.
 *
 *   p9forkprobe          the parent
 *   p9forkprobe child FD the exec'd child: write the time to FD, exit 0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static char self[256];
static int blockfd[2];

static void *blocker(void *arg) {
  (void)arg;
  char c;
  for (;;)
    if (read(blockfd[0], &c, 1) <= 0 && errno != EINTR) break;
  return 0;
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* one fork(or vfork)+exec; returns 0 and the two latencies */
static int one(int use_vfork, double *to_run, double *to_reap) {
  int p[2];
  if (pipe(p) != 0) return -1;
  char fdarg[16];
  snprintf(fdarg, sizeof fdarg, "%d", p[1]);
  char *argv[] = { self, (char *)"child", fdarg, 0 };
  double t0 = now_ms();
  pid_t pid = use_vfork ? vfork() : fork();
  if (pid == 0) {
    execv(self, argv);
    _exit(127);
  }
  if (pid < 0) { close(p[0]); close(p[1]); return -1; }
  close(p[1]);
  double t_child = 0;
  ssize_t n = read(p[0], &t_child, sizeof t_child);
  close(p[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  double t2 = now_ms();
  if (n != (ssize_t)sizeof t_child || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
  *to_run = t_child - t0;
  *to_reap = t2 - t0;
  return 0;
}

static int row(const char *tag, const char *what, int use_vfork, double fail_ms) {
  double run[5], reap[5];
  int ok = 0;
  for (int i = 0; i < 5; i++)
    if (one(use_vfork, &run[ok], &reap[ok]) == 0) ok++;
  if (ok == 0) {
    printf("[P9FORK] %s %s: every %s+exec failed -> FAIL\n", tag, what, use_vfork ? "vfork" : "fork");
    return 1;
  }
  qsort(run, ok, sizeof run[0], cmpd);
  qsort(reap, ok, sizeof reap[0], cmpd);
  int bad = fail_ms > 0 && run[ok / 2] > fail_ms;
  printf("[P9FORK] %s %s: %s+exec -> child running min %.1f med %.1f max %.1f ms; reaped med %.1f ms (n=%d) -> %s\n",
         tag, what, use_vfork ? "vfork" : "fork", run[0], run[ok / 2], run[ok - 1], reap[ok / 2], ok,
         fail_ms <= 0 ? "INFO" : bad ? "FAIL" : "PASS");
  fflush(stdout);
  return bad;
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "child") == 0) {
    double t = now_ms();
    ssize_t w = write(atoi(argv[2]), &t, sizeof t);
    return w == (ssize_t)sizeof t ? 0 : 1;
  }
  ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
  if (sl <= 0) snprintf(self, sizeof self, "%s", argv[0]);
  else self[sl] = 0;
  setvbuf(stdout, 0, _IOLBF, 0);
  printf("[P9FORK] start (C9-play): self=%s\n", self);
  int fails = 0;
  fails += row("F1", "small parent", 0, 0);

  /* which part costs: the reservation alone, then the mappings alone */
  void *res = mmap(0, 64UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  printf("[P9FORK] 64 GiB PROT_NONE reservation: %s\n", res == MAP_FAILED ? "FAILED" : "ok");
  row("F2a", "64 GiB PROT_NONE only", 0, 0);
  if (res != MAP_FAILED) munmap(res, 64UL << 30);
  int regions = 0;
  for (int i = 0; i < 1300; i++) {
    /* alternate protections so neighbours cannot merge into one region */
    char *m = mmap(0, 256 << 10, (i & 1) ? PROT_READ | PROT_WRITE : PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) break;
    for (int k = 0; k < (256 << 10); k += 4096) m[k] = (char)k;
    regions++;
  }
  printf("[P9FORK] %d x 256 KiB touched mappings\n", regions);
  row("F2b", "1300 mappings only", 0, 0);
  res = mmap(0, 64UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  printf("[P9FORK] big parent: reservation %s + the %d mappings\n", res == MAP_FAILED ? "FAILED" : "64 GiB",
         regions);
  fails += row("F2", "big VA", 0, 1000);

  if (pipe(blockfd) == 0) {
    pthread_t th;
    int nt = 0;
    for (int i = 0; i < 40; i++)
      if (pthread_create(&th, 0, blocker, 0) == 0) nt++;
    printf("[P9FORK] + %d threads blocked in read()\n", nt);
  }
  fails += row("F3", "big VA + 40 threads", 0, 1000);
  row("F4", "big VA + 40 threads", 1, 0);
  printf("[P9FORK] done: %s\n", fails ? "FAIL" : "PASS");
  return 0;
}
