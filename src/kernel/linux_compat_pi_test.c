/*
 * linux_compat_pi_test.c — PI futex word state machine (unit tier).
 *
 * Drives the SAME functions the futex(202) handler uses: linux_compat_thr.c
 * exports them under KERNEL_MODE_UNIT_TEST, so this is not a model of the
 * algorithm — a regression in the handler fails here.
 *
 * The states that matter for the M6 park (libpthread 2.31 leaves a
 * PRIO_INHERIT mutex uninitialised when the kernel has no PI futexes, so
 * __lock holds whatever the allocator left behind):
 *
 *   uncontended   0                 -> tid
 *   self-lock     tid               -> EDEADLK, word untouched
 *   contended     other tid         -> park, FUTEX_WAITERS set
 *   trylock       other tid         -> EAGAIN, word untouched
 *   owner died    tid|OWNER_DIED    -> recovered, OWNER_DIED preserved so
 *                                      glibc reports EOWNERDEAD (robust) or
 *                                      clears it (non-robust)
 *   release       own               -> 0 and word 0
 *   release       foreign tid       -> EPERM, word untouched
 */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

#define PI_TID_MASK 0x3fffffffu
#define PI_WAITERS 0x80000000u
#define PI_OWNER_DIED 0x40000000u

#define PI_ACQUIRED 0
#define PI_NEED_WAIT 2
#define PI_EAGAIN (-11)
#define PI_EPERM (-1)
#define PI_EDEADLK (-35)

extern int linux_compat_pi_test_acquire(uint32_t *word, uint32_t pid,
                                        int trylock);
extern int linux_compat_pi_test_release(uint32_t *word, uint32_t pid);
extern uint32_t linux_compat_pi_test_cas(uint32_t *word, uint32_t expect,
                                         uint32_t desire);

static void test_pi_uncontended(void) {
  uint32_t w = 0;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 0), PI_ACQUIRED);
  EXPECT_EQ((int)(w & PI_TID_MASK), 7);
  tests_run++;
}

static void test_pi_self_lock(void) {
  uint32_t w = 7;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 0), PI_EDEADLK);
  EXPECT_EQ((int)w, 7); /* untouched */
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 1), PI_EDEADLK);
  tests_run++;
}

static void test_pi_contended(void) {
  uint32_t w = 9;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 0), PI_NEED_WAIT);
  /* the waiter bit must be published, or the owner's unlock never takes the
     kernel path and the sleeper is never handed the lock */
  EXPECT_EQ((int)(w & PI_WAITERS), (int)PI_WAITERS);
  EXPECT_EQ((int)(w & PI_TID_MASK), 9); /* owner unchanged */
  tests_run++;
}

static void test_pi_trylock_contended(void) {
  uint32_t w = 9;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 1), PI_EAGAIN);
  EXPECT_EQ((int)w, 9); /* trylock must not even set WAITERS */
  tests_run++;
}

static void test_pi_owner_died(void) {
  uint32_t w = 9 | PI_OWNER_DIED;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 0), PI_ACQUIRED);
  EXPECT_EQ((int)(w & PI_TID_MASK), 7);
  /* OWNER_DIED stays: glibc reads it back after a successful lock and turns
     it into EOWNERDEAD (pthread_mutex_lock.c:441) */
  EXPECT_EQ((int)(w & PI_OWNER_DIED), (int)PI_OWNER_DIED);
  tests_run++;
}

static void test_pi_owner_died_waiters(void) {
  uint32_t w = 9 | PI_OWNER_DIED | PI_WAITERS;
  EXPECT_EQ(linux_compat_pi_test_acquire(&w, 7, 0), PI_ACQUIRED);
  EXPECT_EQ((int)(w & PI_TID_MASK), 7);
  EXPECT_EQ((int)(w & PI_OWNER_DIED), (int)PI_OWNER_DIED);
  EXPECT_EQ((int)(w & PI_WAITERS), (int)PI_WAITERS);
  tests_run++;
}

static void test_pi_release(void) {
  uint32_t w = 7;
  EXPECT_EQ(linux_compat_pi_test_release(&w, 7), 0);
  EXPECT_EQ((int)w, 0);
  /* a second release is not ours any more */
  EXPECT_EQ(linux_compat_pi_test_release(&w, 7), PI_EPERM);
  tests_run++;
}

static void test_pi_release_foreign(void) {
  uint32_t w = 7;
  EXPECT_EQ(linux_compat_pi_test_release(&w, 9), PI_EPERM);
  EXPECT_EQ((int)w, 7); /* the owner's word survives a stranger's unlock */
  tests_run++;
}

static void test_pi_cas(void) {
  uint32_t w = 9;
  /* A failed compare must not store: this is what keeps a userspace
     fast-path acquisition from being overwritten by a handoff. */
  EXPECT_EQ((int)linux_compat_pi_test_cas(&w, 7, 11), 9);
  EXPECT_EQ((int)w, 9);
  EXPECT_EQ((int)linux_compat_pi_test_cas(&w, 9, 11), 9);
  EXPECT_EQ((int)w, 11);
  tests_run++;
}

void linux_compat_pi_test_suite(void) {
  uart_puts("[PI] word state machine (LOCK_PI/TRYLOCK_PI/UNLOCK_PI)\n");
  test_pi_uncontended();
  test_pi_self_lock();
  test_pi_contended();
  test_pi_trylock_contended();
  test_pi_owner_died();
  test_pi_owner_died_waiters();
  test_pi_release();
  test_pi_release_foreign();
  test_pi_cas();
}

#endif /* KERNEL_MODE_UNIT_TEST */
