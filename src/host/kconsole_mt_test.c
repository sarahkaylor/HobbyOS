/*
 * kconsole_mt_test.c -- threaded host test for the console's wire lock
 * (src/kernel/kconsole.c; H4-kglitch, docs/cp-kglitch.md §1).
 *
 * One thread per "CPU", all printing at once through the real cons_kwrite /
 * cons_user_write; the wire is a shared byte buffer, so bytes from two writers
 * on the wire at once interleave exactly as they did on integ12's COM1.  The
 * holder is made to stall inside a byte write in the three ways a guest's
 * holder stalls under host load:
 *
 *   jump   the whole guest is stopped: the clock jumps 400 ms while every
 *          thread is frozen, then the holder resumes (SIGSTOP-style host stall);
 *   short  the holder alone is stuck for 0.5 x the bound (a vCPU blocked in
 *          QEMU's serial write, or descheduled), the clock running;
 *   long   the holder alone is stuck for 2.5 x the bound: a takeover is
 *          right here, but it must not shred the rows.
 *
 * Before the fix, one "jump" released every waiter at once and they wrote over
 * each other ("[[[[COCI[CCNCSTPSWONSO!", integ12's console probe).  The
 * checks: phases jump+short end with every row intact and no takeover; the
 * long phase has no line carrying bytes of two rows ("soup") and loses at most
 * the one usurped row (cut up, its tail on a "[CONS!] " line: at most three
 * fragments, its head, one byte in flight, and the tail).
 *
 * Built with -DCONS_WAIT_US=300000 so the long phase takes a second, not 5 s.
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "console.h"
#include "cons_host_shim.h"

#define NCPU 6
#define ROWS 1500

static char *wire;
static size_t wire_cap = 64u << 20;
static volatile size_t wn;
static volatile uint64_t clock_jump_us;
static _Thread_local uint32_t my_cpu;
static _Thread_local int my_irqon = 1;
static char ctxs[NCPU];
static char stacks[NCPU][4096];
_Thread_local const char *cons_host_sp;

/* the stall to inject on the next byte written (0 = none) */
enum { ST_NONE, ST_JUMP, ST_SHORT, ST_LONG };
static volatile int stall_kind;
static volatile int stall_arm; /* this phase's kind */

static uint64_t real_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint32_t get_cpuid(void) { return my_cpu; }
uint64_t timer_get_us(void) { return real_us() + __atomic_load_n(&clock_jump_us, __ATOMIC_ACQUIRE); }
int cpu_is_halted(uint32_t c) { (void)c; return 0; }

static void wire_put(const char *s, int n) {
  for (int i = 0; i < n; i++) {
    size_t at = __atomic_fetch_add(&wn, 1, __ATOMIC_ACQ_REL);
    if (at < wire_cap)
      wire[at] = s[i];
  }
}

/* The stall lands in the middle of the call: a guest's holder is stuck inside
   one byte's port write, with part of its row already on the wire. */
void cons_hw_tx(const char *s, int n) {
  int k = __atomic_exchange_n(&stall_kind, ST_NONE, __ATOMIC_ACQ_REL);
  int h = k ? (n + 1) / 2 : n;
  wire_put(s, h);
  if (k == ST_JUMP) {
    /* The guest stops: every clock reads 400 ms later; the holder is the last
       to resume (its waiters' next samples already see the jump).  The first
       sleep lets the waiters reach their timed state, as they do in a guest
       (a COM1 line takes ~300 us there, here ~1 us). */
    usleep(5000);
    __atomic_fetch_add(&clock_jump_us, 400000, __ATOMIC_ACQ_REL);
    usleep(5000);
  } else if (k == ST_SHORT) {
    usleep(CONS_WAIT_US / 2);
  } else if (k == ST_LONG) {
    usleep(CONS_WAIT_US * 5 / 2);
  }
  wire_put(s + h, n - h);
}
uint64_t cons_hw_irqsave(void) { int was = my_irqon; my_irqon = 0; return (uint64_t)was; }
void cons_hw_irqrestore(uint64_t f) { my_irqon = (int)f; }
int cons_hw_irqs_on(uint64_t f) { return f != 0; }
const void *cons_hw_ctx(uint32_t cpu) { return cpu < NCPU ? &ctxs[cpu] : 0; }
int cons_hw_in_panic(void) { return 0; }
int cons_hw_bootarg(char *b, int cap) { (void)b; (void)cap; return 0; }
void cons_hw_relax(void) { sched_yield(); }

static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static int payload(char *out, int cpu, int seq) {
  int n = 20 + (seq * 7 + cpu * 13) % 100;
  for (int i = 0; i < n; i++)
    out[i] = ALPHA[(seq + i * 7 + cpu) % 62];
  out[n] = 0;
  return n;
}

static volatile int phase; /* rows are numbered phase * 100000 + i */

static void *writer(void *arg) {
  my_cpu = (uint32_t)(uintptr_t)arg;
  cons_host_sp = stacks[my_cpu] + 2048;
  char p[160], b[64];
  int base = phase * 100000;
  for (int i = 0; i < ROWS; i++) {
    int seq = base + i;
    /* CPU 0 arms the phase's stall at fixed points of its own progress; the
       next byte any holder writes takes it. */
    if (my_cpu == 0 && stall_arm && i % (ROWS / 5) == ROWS / 10 &&
        (stall_arm != ST_LONG || i == ROWS / 10))
      __atomic_store_n(&stall_kind, stall_arm, __ATOMIC_RELEASE);
    int n = payload(p, (int)my_cpu, seq);
    if (my_cpu >= 4) {
      /* a user row in two write()s */
      char row[256];
      int k = snprintf(row, sizeof row, "[LPC] w=%u s=%d p=%s e=%d\n", my_cpu, seq, p, n);
      cons_user_write(my_cpu + 100, row, k / 2);
      cons_user_write(my_cpu + 100, row + k / 2, k - k / 2);
    } else {
      /* a kernel row in three calls */
      snprintf(b, sizeof b, "[CSTR] c=%u s=%d p=", my_cpu, seq);
      cons_kwrite(b, -1);
      cons_kwrite(p, n);
      snprintf(b, sizeof b, " e=%d\n", n);
      cons_kwrite(b, -1);
    }
  }
  return 0;
}

struct grade { int ok, torn, soup, frag, takeovers; };

static int valid_row(const char *ln, int len, int *cpu, int *seq) {
  char p[160], tmp[400];
  int c, s, e, n;
  if (len <= 0 || len >= (int)sizeof tmp)
    return 0;
  memcpy(tmp, ln, (size_t)len);
  tmp[len] = 0;
  char pay[200];
  if (sscanf(tmp, "[CSTR] c=%d s=%d p=%199s e=%d%n", &c, &s, pay, &e, &n) == 4 ||
      sscanf(tmp, "[LPC] w=%d s=%d p=%199s e=%d%n", &c, &s, pay, &e, &n) == 4) {
    if (n != len || c < 0 || c >= NCPU)
      return 0;
    int k = payload(p, c, s);
    if (k != e || strcmp(p, pay) != 0)
      return 0;
    *cpu = c;
    *seq = s;
    return 1;
  }
  return 0;
}

static int count_tags(const char *ln, int len) {
  int t = 0;
  for (int i = 0; i + 6 <= len; i++)
    if (!memcmp(ln + i, "[CSTR]", 6) || (i + 5 <= len && !memcmp(ln + i, "[LPC]", 5)))
      t++;
  return t;
}

static struct grade grade(size_t from, size_t to, int ph, int show) {
  struct grade g = {0, 0, 0, 0, 0};
  static unsigned char seen[NCPU][ROWS];
  memset(seen, 0, sizeof seen);
  size_t i = from;
  while (i < to) {
    size_t j = i;
    while (j < to && wire[j] != '\n')
      j++;
    const char *ln = wire + i;
    int len = (int)(j - i);
    i = j + 1;
    if (len == 0)
      continue;
    if (len >= 16 && !memcmp(ln, "[CONS] takeover", 15)) {
      g.takeovers++;
      continue;
    }
    if (len >= 7 && !memcmp(ln, "[CONS] ", 7))
      continue;
    int tagged = 0;
    while (len >= 8 && !memcmp(ln, "[CONS!] ", 8)) {
      ln += 8;
      len -= 8;
      tagged = 1;
    }
    int c, s, tags = count_tags(ln, len);
    if (tags > 1 || (tags == 1 && ln[0] != '[')) {
      g.soup++;
      if (show-- > 0)
        printf("  soup: %.*s\n", len > 150 ? 150 : len, ln);
      continue;
    }
    if (valid_row(ln, len, &c, &s) && s / 100000 == ph && s % 100000 < ROWS) {
      if (!seen[c][s % 100000]++)
        g.ok++;
      continue;
    }
    if (tagged || tags == 0)
      g.frag++;
    else
      g.torn++;
    if (show-- > 0)
      printf("  %s: %.*s\n", tagged || tags == 0 ? "frag" : "torn", len > 150 ? 150 : len, ln);
  }
  return g;
}

static int fails;

static void run_phase(const char *name, int kind, int must_be_clean) {
  size_t from = wn;
  uint64_t forced0 = cons_stats.forced, usurped0 = cons_stats.usurped;
  pthread_t th[NCPU];
  phase++;
  stall_arm = kind;
  for (int c = 0; c < NCPU; c++)
    pthread_create(&th[c], 0, writer, (void *)(uintptr_t)c);
  for (int c = 0; c < NCPU; c++)
    pthread_join(th[c], 0);
  stall_arm = ST_NONE;
  stall_kind = ST_NONE;
  /* print any pending "[CONS] takeover" row and flush, from "CPU 0" */
  my_cpu = 0;
  cons_host_sp = stacks[0] + 2048;
  cons_tick(0);
  size_t to = wn < wire_cap ? wn : wire_cap;
  struct grade g = grade(from, to, phase, 4);
  int want = NCPU * ROWS;
  uint64_t forced = cons_stats.forced - forced0, usurped = cons_stats.usurped - usurped0;
  int bad;
  if (must_be_clean)
    bad = g.ok != want || g.torn || g.soup || g.frag || forced;
  else
    bad = g.soup || g.torn || g.ok < want - 1 || g.frag > 3;
  printf("%s %-6s rows intact %d/%d, torn %d, soup %d, fragments %d, forced %llu, usurped %llu\n",
         bad ? "FAIL" : "ok  ", name, g.ok, want, g.torn, g.soup, g.frag, (unsigned long long)forced,
         (unsigned long long)usurped);
  fails += bad;
}

int main(void) {
  wire = malloc(wire_cap);
  if (!wire)
    return 2;
  cons_diag_mask = CONS_D_ALL;
  my_cpu = 0;
  cons_host_sp = stacks[0] + 2048;
  run_phase("none", ST_NONE, 1);
  run_phase("jump", ST_JUMP, 1);
  run_phase("short", ST_SHORT, 1);
  run_phase("long", ST_LONG, 0);
  printf("kconsole_mt_test: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
