/* tools/cp/audio2/aload.c -- a guest-side load generator for the audio
 * repro (lane H5-audio2, docs/cp-audio2.md).  Static musl; runs as a second
 * boot-wave member next to ATONE.ELF, inside the guest only (never on the
 * strix host).
 *
 *   ALOAD.ELF SECONDS [threads=8] [duty=100] [mode=cpu|mix]
 *
 * Each thread works `duty` % of every 20 ms window and sleeps the rest.
 * mode=cpu spins; mode=mix spends the work in page faults and munmap (TLB
 * shootdown IPIs to the other CPUs), futex ping-pong with a partner thread and
 * short spins: the shape of a game's loading phase more than a pure burner. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double secs = 60;
static int duty = 100, mix = 0;
static volatile int stop;

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pc = PTHREAD_COND_INITIALIZER;
static long tok;

static void work_mix(double until) {
  static __thread unsigned seed = 1;
  while (now_s() < until) {
    size_t sz = (size_t)(256 + (rand_r(&seed) % 1024)) * 1024;
    char *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != MAP_FAILED) {
      for (size_t o = 0; o < sz; o += 4096) m[o] = (char)o;
      munmap(m, sz);
    }
    pthread_mutex_lock(&pm);
    tok++;
    pthread_cond_broadcast(&pc);
    pthread_mutex_unlock(&pm);
    volatile unsigned x = 0;
    for (int i = 0; i < 20000; i++) x += i * 7u;
  }
}

static void *thr(void *a) {
  (void)a;
  double t0 = now_s();
  volatile unsigned long x = 0;
  while (!stop && now_s() - t0 < secs) {
    double w0 = now_s();
    double until = w0 + 0.020 * duty / 100.0;
    if (mix) work_mix(until);
    else
      while (now_s() < until) x += 1;
    if (duty < 100) {
      double rest = w0 + 0.020 - now_s();
      if (rest > 0) {
        struct timespec ts = {0, (long)(rest * 1e9)};
        nanosleep(&ts, 0);
      }
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  int n = 8;
  if (argc > 1) secs = atof(argv[1]);
  for (int i = 2; i < argc; i++) {
    if (!strncmp(argv[i], "threads=", 8)) n = atoi(argv[i] + 8);
    else if (!strncmp(argv[i], "duty=", 5)) duty = atoi(argv[i] + 5);
    else if (!strncmp(argv[i], "mode=", 5)) mix = argv[i][5] == 'm';
  }
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  setvbuf(stdout, 0, _IOLBF, 0);
  printf("[AL] aload secs=%.0f threads=%d duty=%d mode=%s\n", secs, n, duty, mix ? "mix" : "cpu");
  pthread_t t[64];
  for (int i = 0; i < n; i++) pthread_create(&t[i], 0, thr, 0);
  for (int i = 0; i < n; i++) pthread_join(t[i], 0);
  printf("[AL] done rc=0 tok=%ld\n", tok);
  return 0;
}
