/* src/user/linux_probe_mmap_race.c — Linux-ABI probe: concurrent mmap
 * base selection.
 *
 * Why this probe exists (L56): RimWorld's def loader runs four
 * DirectXmlLoader threads, each mapping Data/**\/*.xml through
 * MemoryMappedFile.CreateViewAccessor, i.e. mmap(NULL, len) from four
 * threads at once.  Measured at the L55 tip: 8 of 1,209 defs died per run
 * as `System.IO.IOException: Could not map file` from MemoryMapImpl.Map
 * with `[LCOMPAT] mmap fail ... -> -12` (-ENOMEM), and the failing set was
 * DISJOINT from run to run — the signature of a race, not exhaustion.
 * The mechanism: vm_mmap() chose a base with the lock-free vm_hole_find()
 * and inserted the region much later, so two threads could pick the same
 * base; the loser's insert was refused and the syscall reported -ENOMEM
 * with the whole mmap arena free.
 *
 * Shape: N threads mmap the SAME size, holding every mapping (no munmap, so
 * a reused address is a real duplicate, not allocator behaviour), and the
 * probe asserts:
 *   (a) no mmap failed with ENOMEM — the arena is gigabytes,
 *   (b) every returned base is distinct,
 *   (c) every mapping is writable end to end (a base is not just a number).
 *
 * An ordinary musl/glibc program: the SAME source runs on the host
 * (reference) and inside HobbyOS (measured).  Rows are "[LP] ..."; the last
 * row is "[LP] done rc=<n>".  Pre-fix this fails in the guest and passes on
 * the host; post-fix both pass.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

#define PAGE 4096UL
#define LEN (2 * PAGE)
#define NTHREADS 8
#define ROUNDS 64
#define MAXMAPS (NTHREADS * ROUNDS)

static int fails;
static void *bases[MAXMAPS];
static int nmaps; /* written under the mutex below */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

static volatile int start_gate;
static int enomem;
static int other_err;

static void *worker(void *arg) {
  long id = (long)arg;
  (void)id;
  while (!__atomic_load_n(&start_gate, __ATOMIC_ACQUIRE))
    sched_yield();
  for (int r = 0; r < ROUNDS; r++) {
    void *p = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      int e = errno;
      pthread_mutex_lock(&mu);
      if (e == ENOMEM) {
        enomem++;
        if (enomem <= 8)
          row("[LP] FAIL thread=%ld round=%d mmap -> ENOMEM (arena must be "
              "empty: this is the base-select race)\n",
              id, r);
      } else {
        other_err++;
        if (other_err <= 8)
          row("[LP] FAIL thread=%ld round=%d mmap -> errno=%d\n", id, r, e);
      }
      pthread_mutex_unlock(&mu);
      continue;
    }
    /* Prove the base is a real, writable mapping: touch both pages. */
    volatile char *q = (volatile char *)p;
    q[0] = (char)r;
    q[LEN - 1] = (char)id;
    if (q[0] != (char)r || q[LEN - 1] != (char)id) {
      pthread_mutex_lock(&mu);
      fails++;
      row("[LP] FAIL thread=%ld round=%d base=%p does not hold its bytes\n",
          id, r, p);
      pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    if (nmaps < MAXMAPS)
      bases[nmaps++] = p;
    else
      fails++;
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

static int cmp_ptr(const void *a, const void *b) {
  uintptr_t x = (uintptr_t)*(void *const *)a;
  uintptr_t y = (uintptr_t)*(void *const *)b;
  return (x > y) - (x < y);
}

int main(void) {
  pthread_t th[NTHREADS];

  row("[LP] mmap-race: %d threads x %d rounds of %lu bytes\n", NTHREADS,
      ROUNDS, (unsigned long)LEN);

  for (long i = 0; i < NTHREADS; i++) {
    if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
      row("[LP] FAIL pthread_create %ld\n", i);
      row("[LP] done rc=1\n");
      return 1;
    }
  }
  __atomic_store_n(&start_gate, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < NTHREADS; i++)
    pthread_join(th[i], NULL);

  row("[LP] mapped=%d enomem=%d other_err=%d\n", nmaps, enomem, other_err);
  if (nmaps != MAXMAPS) {
    fails++;
    row("[LP] FAIL expected %d mappings, got %d\n", MAXMAPS, nmaps);
  }
  if (enomem != 0) {
    fails++;
    row("[LP] FAIL %d spurious ENOMEM (a free arena answered -ENOMEM)\n",
        enomem);
  }

  /* Bases must be distinct: every mapping is still held, so a repeat is the
     very collision vm_mmap used to hand out. */
  qsort(bases, (size_t)nmaps, sizeof bases[0], cmp_ptr);
  int dup = 0;
  for (int i = 1; i < nmaps; i++) {
    if (bases[i] == bases[i - 1]) {
      dup++;
      if (dup <= 8)
        row("[LP] FAIL duplicate base %p (maps %d and %d)\n", bases[i], i - 1,
            i);
    }
  }
  if (dup) {
    fails++;
    row("[LP] FAIL %d duplicate bases\n", dup);
  } else {
    row("[LP] distinct-bases OK (%d/%d)\n", nmaps, nmaps);
  }
  if (nmaps > 0)
    row("[LP] span %p..%p\n", bases[0], bases[nmaps - 1]);

  row("[LP] done rc=%d\n", fails ? 1 : 0);
  return fails ? 1 : 0;
}
