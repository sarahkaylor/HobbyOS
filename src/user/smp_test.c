/* SMPTEST.BIN — concurrency soak + scaling probe (kept in the wave on purpose).
 *
 * This is not a "does SMP work" demo any more.  It is the user-side half of the
 * lock-scope work (docs/lock-scope.md): the kernel now takes its filesystem and
 * pipe locks WITHOUT masking interrupts, and this program is what keeps that
 * honest under real concurrency, inside the same automated wave that already
 * runs fork/pipe/sql/signal tests.
 *
 * What it proves, per phase (workers = 1, 2, 4, 8):
 *   - every worker completes its full op count: a thread parked and never
 *     woken, a stranded filesystem lock, or a lost wakeup shows up as a
 *     missing worker instead of a quiet pass;
 *   - workers do not lose or duplicate ops (each touches only its own file, so
 *     a total mismatch points at the kernel, not at the test);
 *   - the two I/O paths that used to mask interrupts are exercised together:
 *     FAT16 (fat_lock / f->lock) and pipes (pipes_lock);
 *   - wall time per phase is printed, so a scaling regression is visible in
 *     the log rather than having to be argued about later.
 *
 * Failure signalling: the parent prints exactly one of
 *     [CSTEST] PASS ...
 *     [CSTEST] FAIL ...
 * and the T3 gate police treats a missing PASS / any FAIL as red.  While it
 * waits it prints "[CSTEST] wait phase=N workers=W", so a stall names its own
 * phase: that is the difference between "the wave hung" and "8 workers on the
 * FAT16 path deadlocked".
 */
#include "libc.h"
#include <fcntl.h>

#define ITERS 8
#define BUFLEN 48

static void put(const char *s) { print(s); }

static void putn(int v) {
  char b[12];
  int i = 11;
  b[i] = '\0';
  if (v < 0) {
    put("-");
    v = -v;
  }
  if (v == 0)
    b[--i] = '0';
  while (v > 0 && i > 0) {
    b[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  put(&b[i]);
}

static unsigned long now_ms(void) {
  struct sys_cpuinfo ci;
  if (sysinfo(1, &ci, (int)sizeof ci) != 0)
    return 0;
  return (unsigned long)ci.uptime_ms;
}

/* One unit of work: a private file round trip plus, every fourth iteration, a
 * round trip through a self-pipe (exercises pipes_lock without a cooperating
 * second process).  Returns the number of completed iterations; stops early on
 * an error so a partial count is visible instead of being counted as done. */
static int worker_body(int w) {
  char path[32];
  char buf[BUFLEN];
  int done = 0;

  for (int i = 0; i < BUFLEN; i++)
    buf[i] = (char)('a' + ((w + i) % 26));

  for (int it = 0; it < ITERS; it++) {
    int n = 0;
    const char *p = "/CSTEST";
    while (*p && n < 20)
      path[n++] = *p++;
    path[n++] = (char)('0' + (w % 10));
    const char *ext = ".TMP";
    while (*ext)
      path[n++] = *ext++;
    path[n] = '\0';

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0)
      break;
    if (write(fd, buf, BUFLEN) != BUFLEN) {
      close(fd);
      break;
    }
    close(fd);

    fd = open(path, O_RDONLY);
    if (fd < 0)
      break;
    char rb[BUFLEN];
    int got = read(fd, rb, BUFLEN);
    close(fd);
    if (got != BUFLEN)
      break;

    int bad = 0;
    for (int i = 0; i < BUFLEN; i++)
      if (rb[i] != buf[i])
        bad = 1;
    if (bad) {
      /* Silent corruption is worse than a short count: name it and stop. */
      put("[CSTEST] worker=");
      putn(w);
      put(" data mismatch\n");
      return done;
    }

    if (unlink(path) != 0)
      break;

    if ((it & 3) == 0) {
      int pfd[2];
      if (pipe(pfd) != 0)
        break;
      if (write(pfd[1], buf, 16) != 16) {
        close(pfd[0]);
        close(pfd[1]);
        break;
      }
      char pb[16];
      int pgot = read(pfd[0], pb, 16);
      close(pfd[0]);
      close(pfd[1]);
      if (pgot != 16)
        break;
    }
    done++;
  }
  return done;
}

__attribute__((section(".text._start")))
void _start(void) {
  static const int phases[] = {1, 2, 4, 8};
  const int nphases = (int)(sizeof phases / sizeof phases[0]);
  int failures = 0;

  put("--- HobbyOS concurrency soak (lock-scope) ---\n");

  for (int ph = 0; ph < nphases; ph++) {
    int workers = phases[ph];
    unsigned long t0 = now_ms();
    int spawned = 0;

    for (int w = 0; w < workers; w++) {
      int pid = fork();
      if (pid == 0) {
        int ops = worker_body(w);
        put("[CSTEST] worker=");
        putn(w);
        put(" ops=");
        putn(ops);
        put("\n");
        exit(ops == ITERS ? 0 : 1);
      }
      if (pid < 0)
        break;
      spawned++;
    }
    if (spawned != workers) {
      put("[CSTEST] FAIL phase=");
      putn(ph);
      put(" fork completed ");
      putn(spawned);
      put("/");
      putn(workers);
      put("\n");
      failures++;
      workers = spawned;
    }

    put("[CSTEST] wait phase=");
    putn(ph);
    put(" workers=");
    putn(workers);
    put("\n");

    int ok = 0;
    for (int w = 0; w < workers; w++) {
      int st = 0;
      int r = waitpid(-1, &st, 0);
      if (r < 0)
        break;
      /* waitpid status encoding (src/kernel/process.c): low 7 bits zero means
       * a normal exit, and the low byte of the high half is the code.  Spelled
       * out here because the freestanding user headers carry no WIFEXITED. */
      if ((st & 0x7F) == 0 && ((st >> 8) & 0xFF) == 0)
        ok++;
    }

    unsigned long ms = now_ms() - t0;
    put("[CSTEST] phase=");
    putn(ph);
    put(" workers=");
    putn(workers);
    put(" ok=");
    putn(ok);
    put(" iters=");
    putn(ITERS);
    put(" ms=");
    if (ms)
      putn((int)ms);
    else
      put("<1");
    put("\n");

    if (ok != workers) {
      put("[CSTEST] FAIL phase=");
      putn(ph);
      put(" completed ");
      putn(ok);
      put("/");
      putn(workers);
      put(" workers (a thread parked and never woken looks exactly like this)\n");
      failures++;
    }
  }

  if (failures == 0)
    put("[CSTEST] PASS all phases: every worker completed every op under "
        "1/2/4/8-way concurrency\n");
  else
    put("[CSTEST] FAIL concurrency soak\n");

  exit(failures == 0 ? 0 : 1);
}
