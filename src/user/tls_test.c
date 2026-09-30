/* TLS_T.BIN — per-thread TLS + errno isolation acceptance (P1).
 *
 * What it proves (docs/browser/p1-threads-design.md sections 4/6):
 *   1. crt0 installs TPIDR_EL0 / IA32_FS_BASE before main: __thread
 *      variables in .tdata keep their initialized values and are writable.
 *   2. Every thread gets its OWN TLS block copied from the linker image:
 *      a thread observes the template value on entry (123 / zeroed .tbss)
 *      and its writes are invisible to the creator and to later threads.
 *   3. errno is per-thread: two threads hammering different failing
 *      syscalls never observe each other's errno.
 *   4. Values survive preemption (writes interleaved with yields) and
 *      join is a full quiescence point.
 *   5. Threads created late (after another thread died) still get a FRESH
 *      copy of the template, not the dead thread's leftovers.
 *
 * Output convention: "  TLS_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "TLS_T FAILED: n".
 *
 * Shared source: the host build (-DHOST_TEST) runs the same logic with
 * glibc thread_local/errno semantics. */

#ifdef HOST_TEST
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Host shims from src/host/compat.c (the device build gets these from
 * libc.h + user/libc.c). */
void print_console(const char *s);
void print_dec(long v);
#else
#include "libc.h"
#include "pthread.h"
#endif

static int fails;

static void check(const char *name, int ok) {
  print_console("  TLS_T ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

static int create_retry(pthread_t *th, void *(*fn)(void *), void *arg) {
  for (int i = 0; i < 400; i++) {
    int rc = pthread_create(th, 0, fn, arg);
    if (rc == 0)
      return 0;
    if (rc != 11) /* EAGAIN */
      return rc;
    sched_yield();
  }
  return 11;
}

/* ---- TLS variables under test (the template lives in .tdata/.tbss) ------- */

__thread int t_int = 123;          /* .tdata: nonzero template */
__thread char t_buf[64];           /* .tbss: zeroed template */
__thread long t_long = -77;
__thread double t_dbl = 2.5;

/* ---- 1. main thread sees the template ----------------------------------- */

static void test_main_template(void) {
  int ok = (t_int == 123) && (t_long == -77) && (t_dbl == 2.5) &&
           (t_buf[0] == 0 && t_buf[63] == 0);
  t_int = 5;
  ok = ok && t_int == 5;
  t_int = 123;
  check("main-template", ok);
}

/* ---- 2. thread sees the template, writes are isolated -------------------- */

static volatile int iso_bad;

static void *iso_fn(void *arg) {
  long id = (long)arg;
  /* Fresh block: the template value, not the creator's writes. */
  if (t_int != 123 || t_long != -77 || t_dbl != 2.5 || t_buf[0] != 0)
    __atomic_store_n(&iso_bad, 1, __ATOMIC_RELAXED);
  t_int = (int)(1000 + id);
  t_long = -1000 - id;
  t_dbl = 10.0 + (double)id;
  t_buf[0] = (char)id;
  for (int r = 0; r < 40; r++) {
    sched_yield(); /* preemption must not leak another thread's block */
    if (t_int != (int)(1000 + id) || t_long != -1000 - id ||
        t_dbl != 10.0 + (double)id || t_buf[0] != (char)id) {
      __atomic_store_n(&iso_bad, 1, __ATOMIC_RELAXED);
      break;
    }
  }
  return (void *)(long)t_int;
}

static void test_thread_isolation(void) {
  pthread_t th[3];
  int ok = 1;
  iso_bad = 0;
  /* the creator writes its own values first: threads must NOT inherit them */
  t_int = 9;
  t_long = -9;
  for (int i = 0; i < 3; i++)
    if (create_retry(&th[i], iso_fn, (void *)(long)(i + 1)) != 0)
      ok = 0;
  for (int i = 0; i < 3 && ok; i++) {
    void *r = 0;
    if (pthread_join(th[i], &r) != 0 || (long)r != 1000 + i + 1)
      ok = 0;
  }
  /* the creator's block is untouched by everything the threads did */
  check("thread-isolation", ok && iso_bad == 0 && t_int == 9 && t_long == -9);
  t_int = 123;
  t_long = -77;
}

/* ---- 3. errno is per-thread --------------------------------------------- */

static volatile int errno_bad;
static volatile int errno_go;

static void *errno_fn(void *arg) {
  (void)arg;
  /* Hammer a deterministic failure: read() on an invalid fd -> EBADF (9). */
  for (int i = 0; i < 300; i++) {
    errno = 0;
    char c;
    long r = read(-1, &c, 1);
    if (r != -1 || errno != 9) {
      __atomic_store_n(&errno_bad, 1, __ATOMIC_RELAXED);
      break;
    }
    if ((i & 31) == 0)
      sched_yield();
  }
  __atomic_store_n(&errno_go, 1, __ATOMIC_RELEASE);
  return 0;
}

static void test_errno_isolation(void) {
  pthread_t t;
  int ok = create_retry(&t, errno_fn, 0) == 0;
  errno_bad = 0;
  errno_go = 0;
  /* main hammers a different failure: open() of a missing file -> ENOENT */
  for (int i = 0; i < 300; i++) {
    errno = 0;
    int fd = open("__tls_test_no_such_file__", 0);
    if (fd >= 0) {
      close(fd);
      ok = 0;
      break;
    }
    if (errno != 2 /* ENOENT */) {
      ok = 0; /* another thread's EBADF reached OUR errno: shared errno! */
      break;
    }
    if ((i & 31) == 0)
      sched_yield();
  }
  void *r = (void *)1;
  ok = ok && pthread_join(t, &r) == 0 && r == 0;
  check("errno-isolation", ok && errno_bad == 0 && errno_go == 1);
}

/* ---- 4. a later thread gets a fresh template ----------------------------- */

static volatile int late_bad;

static void *late_fn(void *arg) {
  long id = (long)arg;
  if (id == 1) {
    /* the "first" thread scribbles all over its block ... */
    t_int = 4242;
    t_long = 4242;
    t_dbl = 4242.0;
    t_buf[0] = 42;
  } else {
    /* ... the second (created after the first joined) must see the
       template, not the leftovers. */
    if (t_int != 123 || t_long != -77 || t_dbl != 2.5 || t_buf[0] != 0)
      __atomic_store_n(&late_bad, 1, __ATOMIC_RELAXED);
  }
  return 0;
}

static void test_fresh_after_death(void) {
  pthread_t t;
  int ok = 1;
  late_bad = 0;
  if (create_retry(&t, late_fn, (void *)1) != 0)
    ok = 0;
  void *r;
  ok = ok && pthread_join(t, &r) == 0;
  if (create_retry(&t, late_fn, (void *)2) != 0)
    ok = 0;
  ok = ok && pthread_join(t, &r) == 0;
  check("fresh-template-reuse", ok && late_bad == 0);
}

/* ---- 5. TLS pointers stay inside each thread's own region ---------------- */

static volatile uintptr_t self_ptrs[3];

static void *addr_fn(void *arg) {
  long id = (long)arg;
  self_ptrs[id - 1] = (uintptr_t)&t_int;
  for (int i = 0; i < 20; i++)
    sched_yield();
  if (self_ptrs[id - 1] != (uintptr_t)&t_int)
    __atomic_store_n(&errno_bad, 1, __ATOMIC_RELAXED);
  return 0;
}

static void test_address_stability(void) {
  pthread_t th[3];
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    self_ptrs[i] = 0;
    if (create_retry(&th[i], addr_fn, (void *)(long)(i + 1)) != 0)
      ok = 0;
  }
  for (int i = 0; i < 3 && ok; i++) {
    void *r;
    if (pthread_join(th[i], &r) != 0)
      ok = 0;
  }
  /* one TLS address per thread, all distinct: the image is private */
  ok = ok && self_ptrs[0] && self_ptrs[1] && self_ptrs[2] &&
       self_ptrs[0] != self_ptrs[1] && self_ptrs[1] != self_ptrs[2] &&
       self_ptrs[0] != self_ptrs[2];
  check("tls-private-blocks", ok);
}

/* ---- 6. main TLS unchanged across thread lifetimes ----------------------- */

static void test_main_survives_threads(void) {
  pthread_t th[2];
  int ok = 1;
  t_int = 314;
  for (int i = 0; i < 2; i++)
    if (create_retry(&th[i], iso_fn, (void *)(long)(i + 1)) != 0)
      ok = 0;
  for (int i = 0; i < 2 && ok; i++) {
    void *r;
    if (pthread_join(th[i], &r) != 0)
      ok = 0;
  }
  ok = ok && t_int == 314 && t_long == -77 && t_buf[0] == 0;
  t_int = 123;
  check("main-tls-across-threads", ok);
}

int main(void) {
  print_console("[TLS_T] per-thread TLS + errno isolation acceptance\n");

  test_main_template();
  test_thread_isolation();
  test_errno_isolation();
  test_fresh_after_death();
  test_address_stability();
  test_main_survives_threads();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
  }
  print_console("TLS_T FAILED: ");
  print_dec(fails);
  print_console("\n");
  return 1;
}
