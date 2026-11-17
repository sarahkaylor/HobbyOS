/*
 * src/user/linux_probe_wfibound.c — a kernel wait that halts must wake by
 * itself, not only on CPU 0's tick (C1-k6, docs/cp-k6.md §14; C3-fat's P9-8).
 *
 * P9-8: blk_token_acquire waits in safe_wfi() (sti; hlt) and the release
 * wakes nobody; an AP's only periodic interrupt is CPU 0's broadcast 0x81,
 * and CPU 0 spinning on a ylock inside a syscall runs with IF=0 (SFMASK), so
 * the tick stops and the halted holder never looks again.
 *
 *   W1  200 halts from a syscall: the longest (us) -- bounded by this CPU's
 *       own one-shot (<= 2500 us), not by the 10 ms tick
 *   W2  the P9-8 shape: a thread halts in the kernel waiting for a flag that
 *       no wakeup announces; another thread, on CPU 0 with IF=0, sets the
 *       flag and spins until the waiter saw it.  The waiter must see it in
 *       <= 20 ms (before the bound it waits for an unrelated interrupt, up
 *       to the spinner's 500 ms give-up).  Up to 10 tries to get the spinner
 *       onto CPU 0 per round, 3 rounds; a round that cannot be arranged is
 *       an info row, and W2 needs at least one arranged round.
 *
 * Private syscall 0x4845 (test kernels only); -ENOSYS -> SKIP.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static long wt(long op, long a) { return syscall(0x4845, op, a); }

static volatile long w_ret;
static void *waiter(void *a) {
  (void)a;
  w_ret = wt(2, 0);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fails = 0;
  long r = wt(4, 0);
  if (r < 0 && errno == ENOSYS) {
    printf("[LP] SKIP W1 W2: no test syscall 0x4845 (not a HobbyOS test kernel)\n[LP] done rc=0\n");
    return 0;
  }
  long a0 = r;
  long worst = wt(1, 200);
  int ok = worst >= 0 && worst <= 2500;
  if (!ok) fails++;
  printf("[LP] %s W1 longest of 200 kernel halts: %ld us (bound 1000 us, limit 2500)\n",
         ok ? "PASS" : "FAIL", worst);
  int rounds_ok = 0, rounds_run = 0;
  for (int round = 0; round < 3; round++) {
    int done = 0;
    for (int tries = 0; tries < 10 && !done; tries++) {
      w_ret = 0;
      pthread_t t;
      pthread_create(&t, 0, waiter, 0);
      long spun = -EAGAIN;
      for (int k = 0; k < 20000; k++) {
        spun = wt(3, 0);
        if (spun >= 0) break;
        if ((k & 15) == 15) usleep(200); /* sleep, so a wake can move us */
        else sched_yield();
      }
      pthread_join(t, 0);
      if (spun < 0) continue; /* never got CPU 0 while the waiter waited */
      done = 1;
      rounds_run++;
      long wr = w_ret;
      long wcpu = wr > 0 ? (wr >> 32) - 1 : -1;
      long lat = wr > 0 ? (wr & 0xffffffffL) : -1;
      int rok = wr > 0 && lat <= 20000;
      if (rok) rounds_ok++;
      printf("[LP] %s W2 round %d: waiter on cpu %ld saw the flag %ld us after CPU 0 set it "
             "with IF=0 (spinner spun %ld us)\n",
             rok ? "PASS" : "FAIL", round + 1, wcpu, lat, spun);
    }
    if (!done)
      printf("[LP] info W2 round %d: the spinner never landed on CPU 0 while the waiter waited\n",
             round + 1);
  }
  if (rounds_run == 0) {
    printf("[LP] FAIL W2 no round could be arranged\n");
    fails++;
  } else if (rounds_ok != rounds_run) {
    fails++;
  }
  printf("[LP] info bounded-halt arms during the probe: %ld\n", wt(4, 0) - a0);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
