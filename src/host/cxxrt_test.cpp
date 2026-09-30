/*
 * HobbyOS F2.4 host test: the minimal C++ runtime (src/libc/src/cxxrt.cpp)
 * compiled for the host and driven through the exact Itanium-ABI entry
 * points clang emits calls to.  The object under test is the shipped
 * source (obj/host_cxxrt.o), not a re-implementation.
 *
 * Scope split with the device wave: CXXSMOKE.BIN proves the integration
 * (crt0 walks .init_array before main, exit() calls the finalizer, the
 * linker script collects the section) on both arches; this test pins the
 * runtime's own semantics under fork/glibc — guard state machine, the
 * alignment-correct new/delete over a foreign allocator, LIFO/dso-filtered
 * finalization, and the abort paths — including boundaries (atexit table
 * full, >16-byte-safe alignment) the device wave cannot force.
 *
 * The TU deliberately does NOT put the sysroot on the include path (see
 * the Makefile rule): <stdio.h>/<sys/wait.h>/<signal.h> must be glibc's
 * here, and cxxrt.h is reached by relative include.  cxxrt.cpp itself
 * keeps the sysroot on its path (its cxxrt.h + stdlib.h resolve exactly
 * as they do on-device), so both header modes are live across the pair.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../libc/include/cxxrt.h"

static int total_checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        total_checks++;                                                       \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s (line %d)\n", msg, __LINE__);                     \
        }                                                                     \
    } while (0)

/* Run fn() in a forked child; returns the waitpid status, or -1 on
 * fork/wait failure.  The child physically exits with fn()'s return
 * value; a child that aborts comes back as WIFSIGNALED. */
static int run_child(int (*fn)(void)) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
    _exit(fn());
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  return status;
}

/* --------------------------------------------------------- __cxa_guard_* */

static void test_guards(void) {
  /* Fresh guard (all zero bytes): the first acquire grants ownership. */
  uint64_t g = 0;
  CHECK(__cxa_guard_acquire(&g) == 1, "guard: first acquire owns init");
  __cxa_guard_release(&g);
  CHECK(__cxa_guard_acquire(&g) == 0, "guard: acquire after release skips");
  CHECK(__cxa_guard_acquire(&g) == 0, "guard: stays initialized");

  /* Abort resets the guard so a retry owns the initialization again —
   * this is the path the compiler's exception cleanup uses (with
   * -fno-exceptions it only fires for explicit use, but the state
   * machine must still be correct). */
  uint64_t h = 0;
  CHECK(__cxa_guard_acquire(&h) == 1, "guard: abort-path first acquire");
  __cxa_guard_abort(&h);
  CHECK(__cxa_guard_acquire(&h) == 1, "guard: abort then retry owns again");
  __cxa_guard_release(&h);
  CHECK(__cxa_guard_acquire(&h) == 0, "guard: released after retry");

  /* The pattern clang actually emits for a function-local static with a
   * dynamic initializer: run the initializer exactly once across calls.
   * (The mid-initialization spin in the acquire loop is deliberately not
   * exercised here: completing it requires a racer, and it would block
   * this single-threaded test forever.) */
  static int runs;
  uint64_t k = 0;
  for (int i = 0; i < 5; i++) {
    if (__cxa_guard_acquire(&k)) {
      runs++;
      __cxa_guard_release(&k);
    }
  }
  CHECK(runs == 1, "guard: initializer runs exactly once");
}

/* ---------------------------------------------------------- new/delete */

static void test_new_delete(void) {
  int *p = new int(42);
  CHECK(p != NULL && *p == 42, "new: scalar with initializer");
  delete p;

  int *arr = new int[16];
  for (int i = 0; i < 16; i++)
    arr[i] = i * i;
  CHECK(arr != NULL && arr[0] == 0 && arr[15] == 225, "new[]: array + fill");
  delete[] arr;

  char *z = new char[0];
  CHECK(z != NULL, "new[]: zero size still non-NULL");
  delete[] z;

  int *nil = NULL;
  delete nil; /* the compiler emits unconditional delete calls */
  CHECK(1, "delete: NULL is a safe no-op");

  /* Alignment: max_align_t is 16 on both targets.  The runtime must
   * over-align every block itself — the underlying allocator (glibc here,
   * HobbyOS malloc on-device, which returns 8-mod-16 pointers) is not
   * relied on for this. */
  void *a1 = operator new(1);
  void *a2 = operator new(7);
  void *a3 = operator new(4096);
  CHECK(((uintptr_t)a1 & 15) == 0 && ((uintptr_t)a2 & 15) == 0 &&
            ((uintptr_t)a3 & 15) == 0,
        "new: 16-byte aligned blocks");
  operator delete(a1);
  operator delete(a2);
  operator delete(a3);

  /* The sized-delete entry points (C++14) are what clang emits for
   * non-trivial classes; they must accept and free. */
  void *s1 = operator new(32);
  operator delete(s1, (size_t)32);
  CHECK(1, "delete: sized entry");
  void *s2 = operator new[](48);
  operator delete[](s2, (size_t)48);
  CHECK(1, "delete[]: sized entry");

  /* Churn: round trips stay consistent.  Sum over rounds 0..255 of
   * (round + round + 7) = 2*(255*256/2) + 256*7 = 67072. */
  long total = 0;
  for (int round = 0; round < 256; round++) {
    int *q = new int[8];
    q[0] = round;
    q[7] = round + 7;
    total += q[0] + q[7];
    delete[] q;
  }
  CHECK(total == 67072L, "new/delete: churn sum");

  /* Placement new: pure pointer pass-through, no allocation. */
  static unsigned char buf[64];
  long long *pl = new (buf) long long(1234567890LL);
  CHECK((unsigned char *)pl == buf && *pl == 1234567890LL,
        "placement new: in-place construction");
}

/* ------------------------------------------------------- atexit/finalize */

static int fin_log[80];
static int fin_n;

static void fin_rec(void *arg) {
  if (fin_n < (int)(sizeof(fin_log) / sizeof(fin_log[0])))
    fin_log[fin_n++] = (int)(intptr_t)arg;
}

/* Runs in a child that must see the table EMPTY: fills all 64 slots and
 * expects the 65th registration to be rejected (documented limitation in
 * cxxrt.h).  Kept in a child because a full table poisons every later
 * registration in the process. */
static int child_fill_atexit_table(void) {
  for (int i = 0; i < 64; i++) {
    if (__cxa_atexit(fin_rec, NULL, NULL) != 0)
      return 1;
  }
  if (__cxa_atexit(fin_rec, NULL, NULL) != -1)
    return 1;
  return 0;
}

static void test_atexit_finalize(void) {
  CHECK(__cxa_atexit(NULL, NULL, NULL) == -1,
        "__cxa_atexit: NULL func rejected");

  static int dso_a, dso_b; /* any addresses serve as dso tags */
  CHECK(__cxa_atexit(fin_rec, (void *)(intptr_t)1, NULL) == 0,
        "__cxa_atexit: register (null dso)");
  CHECK(__cxa_atexit(fin_rec, (void *)(intptr_t)2, &dso_a) == 0,
        "__cxa_atexit: register (dso A)");
  CHECK(__cxa_atexit(fin_rec, (void *)(intptr_t)3, &dso_b) == 0,
        "__cxa_atexit: register (dso B)");

  fin_n = 0;
  __cxa_finalize(&dso_a); /* filtered: runs only the dso-A entry */
  CHECK(fin_n == 1 && fin_log[0] == 2, "__cxa_finalize: dso filter");

  __cxa_finalize(NULL); /* runs everything left, in reverse order */
  CHECK(fin_n == 3 && fin_log[1] == 3 && fin_log[2] == 1,
        "__cxa_finalize: LIFO for the rest");

  int after = fin_n;
  __cxa_finalize(NULL); /* idempotent: consumed entries never rerun */
  CHECK(fin_n == after, "__cxa_finalize: entry runs at most once");
}

/* ------------------------------------------------------- pure virtual trap */

static int child_pure_virtual(void) {
  __cxa_pure_virtual(); /* noreturn: prints a diagnostic, then abort()s */
  return 0;             /* not reached */
}

/* ------------------------------------------------------------------- main */

int main(void) {
  printf("[cxxrt_test] F2.4 minimal C++ runtime (host)\n");

  /* First: this child needs the atexit table empty. */
  int st = run_child(child_fill_atexit_table);
  CHECK(st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
        "__cxa_atexit: 64 slots fill, 65th rejected (child)");

  test_guards();
  test_new_delete();
  test_atexit_finalize();

  st = run_child(child_pure_virtual);
  CHECK(st >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
        "__cxa_pure_virtual: aborts with SIGABRT (child)");

  printf("cxxrt_test: %d checks, %d failures\n", total_checks, failures);
  if (failures == 0) {
    printf("CXXRT TEST PASSED\n");
    return 0;
  }
  printf("CXXRT TEST FAILED\n");
  return 1;
}
