/*
 * src/user/linux_probe_manythr.c - C1-k4: more than 256 live threads
 * (MAX_PROCESSES 192 -> 512 on x86_64, docs/cp-k4.md §3).  The game runs
 * 173-190 threads next to ~18 Wine processes.
 *
 * ORDINARY musl Linux program; same static-PIE ELF on strix (Linux) and in
 * the HobbyOS guest.  Every row is ONE write(2); last row "[LP] done rc=N".
 *
 *  M1  300 threads in one process, all alive at once (each parks on a
 *      condition variable until the main thread has seen all of them), with
 *      300 distinct tids; all join
 *  M2  3 forked children x 100 threads alive at the same time as each other
 *      (each child reports "all up" on a pipe and holds its threads until the
 *      parent says go); every child exits 0
 *  M3  M1 again: the 300 slots of M1/M2 were all reclaimed
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NT 300
static int fails;
static void out(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_up = PTHREAD_COND_INITIALIZER, cv_go = PTHREAD_COND_INITIALIZER;
static int up, go;
static int tids[NT];

static void *worker(void *v) {
  int i = (int)(long)v;
  tids[i] = (int)syscall(SYS_gettid);
  pthread_mutex_lock(&mu);
  up++;
  pthread_cond_signal(&cv_up);
  while (!go) pthread_cond_wait(&cv_go, &mu);
  pthread_mutex_unlock(&mu);
  return 0;
}

/* Start n threads, wait until all are up, release them, join.  Returns the
   number created; *maxtid / *distinct describe the tids. */
static int crowd(int n, int *maxtid, int *distinct, int *err) {
  pthread_t th[NT];
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 64 * 1024);
  up = go = 0;
  memset(tids, 0, sizeof tids);
  int made = 0;
  *err = 0;
  for (int i = 0; i < n; i++) {
    int r = pthread_create(&th[i], &at, worker, (void *)(long)i);
    if (r) { *err = r; break; }
    made++;
  }
  pthread_mutex_lock(&mu);
  while (up < made) pthread_cond_wait(&cv_up, &mu);
  go = 1;
  pthread_cond_broadcast(&cv_go);
  pthread_mutex_unlock(&mu);
  for (int i = 0; i < made; i++) pthread_join(th[i], 0);
  *maxtid = 0;
  *distinct = 1;
  for (int i = 0; i < made; i++) {
    if (tids[i] > *maxtid) *maxtid = tids[i];
    for (int j = 0; j < i; j++)
      if (tids[j] == tids[i]) *distinct = 0;
  }
  return made;
}

static void m1(const char *name) {
  int maxtid, distinct, err;
  int made = crowd(NT, &maxtid, &distinct, &err);
  int ok = made == NT && distinct;
  out("[LP] %s %s: %d/%d threads alive at once, distinct=%d max_tid=%d err=%d\n", ok ? "PASS" : "FAIL",
      name, made, NT, distinct, maxtid, err);
  if (!ok) fails++;
}

int main(void) {
  m1("M1 300 threads");
  /* M2 */
  int up_p[2], go_p[2];
  if (pipe(up_p) || pipe(go_p)) { out("[LP] FAIL M2 pipe\n"); return 1; }
  pid_t kid[3];
  for (int k = 0; k < 3; k++) {
    kid[k] = fork();
    if (kid[k] == 0) {
      pthread_t th[100];
      pthread_attr_t at;
      pthread_attr_init(&at);
      pthread_attr_setstacksize(&at, 64 * 1024);
      up = go = 0;
      int made = 0;
      for (int i = 0; i < 100; i++)
        if (pthread_create(&th[i], &at, worker, (void *)(long)i) == 0) made++;
      pthread_mutex_lock(&mu);
      while (up < made) pthread_cond_wait(&cv_up, &mu);
      pthread_mutex_unlock(&mu);
      char c = (char)made;
      (void)!write(up_p[1], &c, 1);
      (void)!read(go_p[0], &c, 1);
      pthread_mutex_lock(&mu);
      go = 1;
      pthread_cond_broadcast(&cv_go);
      pthread_mutex_unlock(&mu);
      for (int i = 0; i < made; i++) pthread_join(th[i], 0);
      _exit(made == 100 ? 0 : 3);
    }
  }
  int total = 0;
  for (int k = 0; k < 3; k++) {
    unsigned char c = 0;
    if (read(up_p[0], &c, 1) == 1) total += c;
  }
  char g[3] = {1, 1, 1};
  (void)!write(go_p[1], g, 3);
  int okk = 1;
  for (int k = 0; k < 3; k++) {
    int st = 0;
    waitpid(kid[k], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) okk = 0;
  }
  int ok = total == 300 && okk;
  out("[LP] %s M2 3 children x 100 threads: %d alive at once, children ok=%d\n", ok ? "PASS" : "FAIL", total, okk);
  if (!ok) fails++;
  m1("M3 300 threads again");
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
