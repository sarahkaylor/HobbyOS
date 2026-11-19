/*
 * src/user/linux_probe_tlbstale.c -- no thread keeps a stale translation
 * after munmap / mprotect / MAP_FIXED by another thread (H7-fault round 2,
 * docs/cp-fault.md §4: the shootdown changes must stay correct).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * A worker thread spins on its own CPU, touching page P every iteration so
 * its TLB holds P's translation.  Each round the main thread changes P and
 * then asks the worker for one access:
 *   M1 mprotect(P, PROT_READ)          -> the worker's WRITE must fault
 *   M2 munmap(P)                       -> the worker's READ must fault
 *   M3 mmap(P, MAP_FIXED|MAP_ANONYMOUS)  -> the worker must READ zero, not the
 *      old page's marker (a stale translation reads the freed frame)
 * 1000 rounds of each.  A round whose access did not fault (M1/M2) or read
 * the old marker (M3) is a stale TLB entry: FAIL.  The worker never sleeps
 * between its last touch and the access, so it is a CPU that ran the AS
 * the whole time (a shootdown target that must be flushed).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PG 4096
#define ROUNDS 1000

static volatile uint8_t *P;
static volatile int req, ack, mode, stop, hot, touched, phase, quiet;
static volatile int faulted;
static volatile uint64_t seen;
static sigjmp_buf jb;

static void on_segv(int sig) {
  (void)sig;
  faulted = 1;
  siglongjmp(jb, 1);
}

/* worker: while `hot`, touch P (its TLB caches P); when not, say so (quiet)
   and wait; on a request, one access to P that must see the change */
static void *worker(void *a) {
  (void)a;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_segv;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  int done = 0;
  while (!stop) {
    if (__atomic_load_n(&req, __ATOMIC_ACQUIRE) != done) {
      faulted = 0;
      seen = 0;
      if (sigsetjmp(jb, 1) == 0) {
        if (mode == 1) P[8] = 1;                      /* M1: write to read-only */
        else seen = *(volatile uint64_t *)(P + 16);    /* M2 / M3: read */
      }
      done = req;
      __atomic_store_n(&ack, done, __ATOMIC_RELEASE);
      continue;
    }
    if (hot) {
      P[0]++;
      (void)*(volatile uint64_t *)(P + 16);
      __atomic_store_n(&touched, touched + 1, __ATOMIC_RELEASE);
    } else {
      /* hot was 0 when read, so no touch is in flight: acknowledge the
         phase the main thread published after clearing it */
      __atomic_store_n(&quiet, __atomic_load_n(&phase, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    }
  }
  return 0;
}

static uint8_t *fresh(void) {
  uint8_t *p = mmap((void *)P, PG, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | (P ? MAP_FIXED : 0), -1, 0);
  return p == MAP_FAILED ? 0 : p;
}

/* one round: arm P, let the worker cache it, stop it, change P, ask */
static int round_(int m, int *bad) {
  if (!fresh()) return -1;
  *(volatile uint64_t *)(P + 16) = 0x5354414C45ull; /* "STALE" marker */
  int r = req;
  int t0 = touched;
  hot = 1;
  while (touched - t0 < 64) ;                        /* the worker has P cached */
  __atomic_store_n(&hot, 0, __ATOMIC_SEQ_CST);
  int ph = __atomic_add_fetch(&phase, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&quiet, __ATOMIC_ACQUIRE) != ph) ; /* and stopped */
  if (m == 1) mprotect((void *)P, PG, PROT_READ);
  else if (m == 2) munmap((void *)P, PG);
  else if (m == 3 && mmap((void *)P, PG, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    return -1;
  mode = m;
  __atomic_store_n(&req, r + 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&ack, __ATOMIC_ACQUIRE) != r + 1) ;
  int stale = (m == 1 || m == 2) ? !faulted : (seen == 0x5354414C45ull);
  if (stale) (*bad)++;
  if (m == 1) mprotect((void *)P, PG, PROT_READ | PROT_WRITE);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  P = 0;
  P = fresh();
  if (!P) { printf("[LP] T0 mmap: FAIL\n[LP] done rc=1\n"); return 1; }
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  int fails = 0;
  static const char *name[4] = {"", "M1 mprotect(READ) then write", "M2 munmap then read",
                                "M3 MAP_FIXED replace then read"};
  for (int m = 1; m <= 3; m++) {
    int bad = 0, err = 0;
    for (int i = 0; i < ROUNDS; i++)
      if (round_(m, &bad) != 0) err++;
    printf("[LP] %s rounds=%d stale=%d err=%d -> %s\n", name[m], ROUNDS, bad, err,
           bad || err ? "FAIL" : "PASS");
    if (bad || err) fails++;
  }
  stop = 1;
  pthread_join(t, 0);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
