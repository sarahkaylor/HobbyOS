/*
 * HobbyOS selfhost W2-2 host test: setjmp/longjmp semantics.
 *
 * Compiles src/libc/src/setjmp.c with -DHOST_TEST so the real handlers land
 * as hb_setjmp/hb_longjmp/hb_sigsetjmp/hb_siglongjmp and operate on the
 * host's <setjmp.h> jmp_buf (the x86-64 footprint — 64 B — fits glibc's
 * 200 B buffer).  The host run exercises the x86-64 code path live; the
 * aarch64 path is covered structurally by the device build (disassembly +
 * every in-OS compile that links libc.a includes setjmp.o).
 *
 * Checks (POSIX semantics):
 *   - setjmp returns 0 on the initial pass;
 *   - longjmp(env, val) makes setjmp return val (val != 0);
 *   - longjmp(env, 0) makes setjmp return 1;
 *   - callee-saved registers survive a jump from a nested frame;
 *   - sigsetjmp/siglongjmp round-trip (savesigs is a no-op here).
 *
 * Exit 0 on full pass, non-zero with a FAIL count otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

/* The HobbyOS implementations (renamed by the HOST_TEST block).  The
 * returns_twice attribute matters: without it the compiler may hold an
 * automatic variable in a caller-saved register across the call and clobber
 * it on the second pass. */
extern int hb_setjmp(jmp_buf env) __attribute__((returns_twice));
extern void hb_longjmp(jmp_buf env, int val) __attribute__((noreturn));
extern int hb_sigsetjmp(sigjmp_buf env, int savesigs) __attribute__((returns_twice));
extern void hb_siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                   \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s (line %d)\n", what, __LINE__);  \
        }                                                   \
    } while (0)

/* Round trip: setjmp returns 0 first, longjmp(env,val) later. */
static int g_env_stage;

static void test_round_trip(void) {
  jmp_buf env;
  volatile int r = 0;
  volatile int local = 12345;

  g_env_stage = 0;
  r = hb_setjmp(env);
  if (g_env_stage == 0) {
    CHECK(r == 0, "setjmp initial return");
    g_env_stage = 1;
    hb_longjmp(env, 7);
    CHECK(0, "longjmp did not jump");
  } else {
    CHECK(r == 7, "longjmp(env,7) -> setjmp returns 7");
    CHECK(local == 12345, "automatic local survives the jump");
  }
}

/* longjmp(env, 0) must map to a return value of 1 (POSIX). */
static void test_zero_val(void) {
  jmp_buf env;
  volatile int r = 0;

  g_env_stage = 0;
  r = hb_setjmp(env);
  if (g_env_stage == 0) {
    g_env_stage = 1;
    hb_longjmp(env, 0);
    CHECK(0, "longjmp(env,0) did not jump");
  } else {
    CHECK(r == 1, "longjmp(env,0) -> setjmp returns 1");
  }
}

/* Jump from a nested frame back to the setjmp frame: callee-saved
 * registers (rbx/rbp/r12-r15) must be restored exactly. */
static void nested_clobber_and_jump(jmp_buf env);
static void test_nested_jump(void) {
  jmp_buf env;
  volatile int r = 0;

  g_env_stage = 0;
  r = hb_setjmp(env);
  if (g_env_stage == 0) {
    g_env_stage = 1;
    nested_clobber_and_jump(env);
    CHECK(0, "nested longjmp did not jump");
  } else {
    CHECK(r == 42, "nested longjmp -> setjmp returns 42");
  }
}

static void nested_clobber_and_jump(jmp_buf env) {
  /* Heavy use of locals forces callee-saved register saves, which we then
   * deliberately trash before the jump. */
  register long long a = 111, b = 222, c = 333, d = 444;
  register long long e = 555, f = 666;
  (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
  hb_longjmp(env, 42);
}

/* sigsetjmp/siglongjmp are the same machinery here. */
static void test_sig_variants(void) {
  sigjmp_buf env;
  volatile int r = 0;

  g_env_stage = 0;
  r = hb_sigsetjmp(env, 1);
  if (g_env_stage == 0) {
    CHECK(r == 0, "sigsetjmp initial return");
    g_env_stage = 1;
    hb_siglongjmp(env, 9);
    CHECK(0, "siglongjmp did not jump");
  } else {
    CHECK(r == 9, "siglongjmp -> sigsetjmp returns 9");
  }
}

/* The jump can be taken more than once (a second longjmp from the same
 * frame restarts at the same point). */
static void test_repeated_jump(void) {
  jmp_buf env;
  volatile int r = 0;
  volatile int count = 0;

  g_env_stage = 0;
  r = hb_setjmp(env);
  count++;
  if (g_env_stage == 0) {
    g_env_stage = 1;
    hb_longjmp(env, 5);
  } else if (g_env_stage == 1) {
    g_env_stage = 2;
    hb_longjmp(env, 6);
  } else {
    CHECK(r == 6, "second longjmp lands too");
    CHECK(count == 3, "setjmp point re-entered exactly 3 times");
  }
}

int main(void) {
  test_round_trip();
  test_zero_val();
  test_nested_jump();
  test_sig_variants();
  test_repeated_jump();

  printf("libc_setjmp_test: %d checks, %d failures\n", checks, failures);
  if (failures == 0) {
    printf("libc_setjmp_test: PASSED\n");
    return 0;
  }
  return 1;
}
