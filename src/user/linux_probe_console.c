/*
 * src/user/linux_probe_console.c - C0-console: is the serial console
 * line-atomic under load?  (docs/cp-console.md §5)
 *
 * ORDINARY musl Linux program.  For LPC_SECS (60) seconds:
 *   - 12 stress threads call the kernel's private console stress
 *     (syscall 0x4843, src/kernel/linux_compat_cons.c), which prints
 *     "[CSTR] ..." rows from whatever CPU the thread runs on, in five shapes
 *     (multi-call, char-at-a-time lock-free, one kline, char-at-a-time locked,
 *     multi-call across masked interrupts);
 *   - 4 writer threads write "[LPC] ..." rows to fd 1 (the console):
 *       w=0 a short row, one write()
 *       w=1 a long row (600..1800 bytes), one write()
 *       w=2 eight rows in ONE write()
 *       w=3 one row written in THREE write() calls (prefix, payload, end)
 * Every row's payload is a pure function of its numbers, so the host checker
 * (tools/gate/lp_check_console.py, run by tools/gate/t3_linux_probe.sh)
 * regenerates it and counts interleaved, torn and missing rows from the
 * console log.  This program cannot see the console; it prints its totals:
 *   [LP] console totals kernel=<rows> user=<rows> cpus=<mask> ...
 *   [LP] done rc=0    (rc=1: the stress call is missing or no row was printed)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NSTRESS 12
#define NWRITER 4
#define SYS_HB_CONS_STRESS 0x4843

static const char alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static volatile int stop;
static volatile unsigned long krows[NSTRESS];
static volatile unsigned long kcpus;
static volatile int kerr;
static volatile unsigned long urows[NWRITER];

static void out(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static void nap_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *stress(void *v) {
  int t = (int)(long)v;
  unsigned long seq = (unsigned long)t * 10000000UL;
  while (!stop) {
    long r = syscall(SYS_HB_CONS_STRESS, 3L, (long)seq, 0x1FL);
    if (r < 0) {
      kerr = (int)errno;
      break;
    }
    unsigned long done = (unsigned long)r & 0xFFFFFFFFUL;
    unsigned long cpu = ((unsigned long)r >> 32) & 63;
    __atomic_or_fetch(&kcpus, 1UL << cpu, __ATOMIC_RELAXED);
    krows[t] += done;
    seq += done;
    nap_ms(8);
  }
  return 0;
}

/* user row payload: w, seq -> length and characters (mirrored in the checker) */
static int ulen(int w, unsigned long s) {
  switch (w) {
  case 1: return 600 + (int)((s * 37UL) % 1200UL);
  case 2: return 60 + (int)((s * 11UL) % 80UL);
  default: return 30 + (int)((s * 13UL + (unsigned long)w) % 90UL);
  }
}
static char uch(int w, unsigned long s, int i) {
  return alpha[(s + (unsigned long)i * 17UL + (unsigned long)w * 5UL) % 62UL];
}

static int urow(char *b, int w, unsigned long s) {
  int n = sprintf(b, "[LPC] w=%d s=%lu p=", w, s);
  int len = ulen(w, s);
  for (int i = 0; i < len; i++) b[n++] = uch(w, s, i);
  n += sprintf(b + n, " e=%d\n", len);
  return n;
}

static void *writer(void *v) {
  int w = (int)(long)v;
  static char bufs[NWRITER][8 * 256 + 2048];
  char *b = bufs[w];
  unsigned long s = 0;
  while (!stop) {
    if (w == 2) {
      int n = 0;
      for (int k = 0; k < 8; k++) n += urow(b + n, w, s++);
      (void)!write(1, b, (size_t)n);
      urows[w] += 8;
      nap_ms(10);
    } else if (w == 3) {
      int n = urow(b, w, s);
      char *p = strstr(b, " p=");
      char *e = strstr(b, " e=");
      (void)!write(1, b, (size_t)(p - b));
      (void)!write(1, p, (size_t)(e - p));
      (void)!write(1, e, (size_t)(b + n - e));
      s++;
      urows[w]++;
      nap_ms(5);
    } else {
      int n = urow(b, w, s++);
      (void)!write(1, b, (size_t)n);
      urows[w]++;
      nap_ms(w == 1 ? 20 : 3);
    }
  }
  return 0;
}

int main(void) {
  int secs = 60;
  const char *e = getenv("LPC_SECS");
  if (e && atoi(e) > 0) secs = atoi(e);
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  out("[LP] console begin secs=%d stress=%d writers=%d ncpu=%ld\n", secs, NSTRESS, NWRITER, ncpu);
  pthread_t th[NSTRESS + NWRITER];
  for (int i = 0; i < NSTRESS; i++)
    pthread_create(&th[i], 0, stress, (void *)(long)i);
  for (int i = 0; i < NWRITER; i++)
    pthread_create(&th[NSTRESS + i], 0, writer, (void *)(long)i);
  double t0 = now_s();
  while (now_s() - t0 < secs && !kerr) nap_ms(200);
  stop = 1;
  for (int i = 0; i < NSTRESS + NWRITER; i++) pthread_join(th[i], 0);
  unsigned long kt = 0, ut = 0;
  for (int i = 0; i < NSTRESS; i++) kt += krows[i];
  for (int i = 0; i < NWRITER; i++) ut += urows[i];
  int ncpus = __builtin_popcountl(kcpus);
  out("[LP] console totals kernel=%lu user=%lu cpus=0x%lx ncpus=%d w0=%lu w1=%lu w2=%lu w3=%lu secs=%.1f\n",
      kt, ut, kcpus, ncpus, urows[0], urows[1], urows[2], urows[3], now_s() - t0);
  for (int i = 0; i < NSTRESS; i++) out("[LP] console stress t=%d rows=%lu\n", i, krows[i]);
  int rc = 0;
  if (kerr) {
    out("[LP] FAIL console stress syscall 0x%x errno=%d (kernel without linux_compat_cons.c?)\n",
        SYS_HB_CONS_STRESS, kerr);
    rc = 1;
  }
  if (kt == 0 || ut == 0) {
    out("[LP] FAIL console no rows printed\n");
    rc = 1;
  }
  out("[LP] done rc=%d\n", rc);
  return rc;
}
