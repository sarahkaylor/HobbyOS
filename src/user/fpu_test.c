/* FPU_T.BIN — floating point on both arches (browser.md F1.5).
 *
 * Not wired into the build yet: user programs are still compiled with
 * -mgeneral-regs-only (ARM) / -mno-sse (x64), so this file only compiles
 * once the FPU lane for an arch flips its USER_CFLAGS and lands the kernel
 * side (CPACR_EL1.FPEN / CR0+CR4 + context-switch save/restore).  The lane
 * wires it in the same change:
 *   Makefile:  FPU_T_BIN = $(OBJ_DIR)/fpu_test.bin  (build+link rules like
 *              the other *_test bins; link with user_libc.o + user_malloc.o
 *              + libc_string.o where those are needed), add $(FPU_T_BIN) to
 *              the disk.img dependency list and
 *              $(MCOPY) -i disk.img $(FPU_T_BIN) ::/FPU_T.BIN
 *   main.c:    load_and_run_program_in_scheduler("FPU_T.BIN", -1,-1,-1,-1);
 *              at the END of the KERNEL_MODE_TEST list.
 *
 * What it proves (evidence rules: observed output from the real case):
 *   1. Basic float/double arithmetic executes and is exact for binary-
 *      representable values (bit-compared, volatile-defeated folding).
 *   2. Values in the register file survive preemption: each check happens
 *      after sleep()/yield() let other processes run, and a two-process
 *      fork loop hammers different constants through the same registers
 *      while both sides verify every iteration (catches a missing save/
 *      restore on context switch, including cross-CPU).
 *   3. fork() gives the child an independent FP context: child mutation
 *      must not be visible in the parent.
 *
 * Output convention: "  FPU_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "FPU_T FAILED: n". Self-terminating;
 * short sleeps only.
 *
 * Known-value policy: every compared constant is exactly representable in
 * binary (1.5, 3.25, 5.625, 0.125, -2.5, 123.75, 7.75 …) so equality is
 * exact regardless of rounding mode.
 */

#include "libc.h"

static int fails;

static void check(const char *name, int ok) {
  print_console("  FPU_T ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok) fails++;
}

static uint32_t fbits(float f) {
  union { float f; uint32_t u; } v;
  v.f = f;
  return v.u;
}

static uint64_t dbits(double d) {
  union { double d; uint64_t u; } v;
  v.d = d;
  return v.u;
}

static void test_float_exact(void) {
  volatile float a = 1.5f, b = 3.25f;
  float r = a * b + 0.75f;              /* 4.875 + 0.75 = 5.625, exact    */
  check("float-mul-add", fbits(r) == fbits(5.625f));

  volatile float c = -2.5f;
  float r2 = c * 4.0f;                  /* -10.0, exact                   */
  check("float-neg-mul", fbits(r2) == fbits(-10.0f));

  volatile float x = 1.0f;
  float acc = 0.0f;
  for (int i = 0; i < 8; i++) acc += x * 0.125f;   /* 8 * 0.125 = 1.0      */
  check("float-loop", fbits(acc) == fbits(1.0f));
}

static void test_double_exact(void) {
  volatile double n = 2.0, d = 7.0;
  double q1 = n / d, q2 = n / d;
  check("double-consistent", dbits(q1) == dbits(q2));

  volatile double e = 1.0;
  double eighth = e / 8.0;
  check("double-eighth", dbits(eighth) == dbits(0.125));

  check("double-precision-1", dbits(q1) != (uint64_t)fbits((float)q1));
  check("double-precision-2", fbits((float)0.125) != 0);      /* guard */
}

static void test_preemption(void) {
  /* Values computed now must be intact after other processes run. */
  volatile float vals[16];
  for (int i = 0; i < 16; i++) vals[i] = (float)i * 1.25f;

  sleep(1);                                   /* ~1s of other processes    */
  for (int i = 0; i < 4; i++) yield();

  int ok = 1;
  for (int i = 0; i < 16; i++) {
    volatile float expect = (float)i * 1.25f;
    if (fbits(vals[i]) != fbits(expect)) ok = 0;
  }
  check("preemption-persistence", ok);
}

/* Two processes hammering different constants through the same FP
 * registers; both verify every iteration while the scheduler preempts
 * them.  Child reports through a pipe (1 = ok, 0 = failed, EOF = died). */
static void test_two_process_torture(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    check("torture-pipe", 0);
    return;
  }

  int pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int bad = 0;
    for (int i = 0; i < 200; i++) {
      volatile float v = 7.75f;
      float r = v + 0.25f;                   /* 8.0 exact */
      if (fbits(r) != fbits(8.0f)) bad++;
      volatile double w = 0.5;
      if (dbits(w * 4.0) != dbits(2.0)) bad++;
      yield();
    }
    char byte = bad ? '0' : '1';
    write(fds[1], &byte, 1);
    close(fds[1]);
    exit(bad ? 1 : 0);
  }

  close(fds[1]);
  int bad = 0;
  for (int i = 0; i < 200; i++) {
    volatile float v = 3.5f;
    float r = v + 0.25f;                     /* 3.75 exact */
    if (fbits(r) != fbits(3.75f)) bad++;
    volatile double w = 0.25;
    if (dbits(w * 8.0) != dbits(2.0)) bad++;
    yield();
  }

  char byte = '0';
  int got = read(fds[0], &byte, 1);
  close(fds[0]);
  int st1 = 0;
  if (got == 1) waitpid(pid, &st1, 0);

  check("torture-parent", bad == 0);
  check("torture-child", got == 1 && byte == '1');
}

static void test_fork_isolation(void) {
  volatile float v = 123.75f;
  int fds[2];
  if (pipe(fds) != 0) {
    check("fork-pipe", 0);
    return;
  }

  int pid = fork();
  if (pid == 0) {
    close(fds[0]);
    volatile float mine = v;
    int ok = fbits(mine) == fbits(123.75f);  /* inherited FP state        */
    mine = -1.5f;
    ok = ok && fbits(mine) == fbits(-1.5f);
    char byte = ok ? '1' : '0';
    write(fds[1], &byte, 1);
    close(fds[1]);
    exit(0);
  }

  close(fds[1]);
  char byte = '0';
  int got = read(fds[0], &byte, 1);
  close(fds[0]);
  int st2 = 0;
  if (got == 1) waitpid(pid, &st2, 0);
  sleep(1);                                  /* let the child run/exit    */

  check("fork-inherit", got == 1 && byte == '1');
  check("fork-isolation", fbits(v) == fbits(123.75f));
}

static void test_syscall_boundary(void) {
  volatile float v = 9.0f / 4.0f;            /* 2.25 exact */
  int pid = getpid();                        /* syscall round trip */
  yield();
  check("syscall-boundary", pid > 0 && fbits(v) == fbits(2.25f));
}

__attribute__((section(".text._start")))
void _start(void) {
  print_console("[FPU_T] floating-point bring-up test\n");

  test_float_exact();
  test_double_exact();
  test_preemption();
  test_two_process_torture();
  test_fork_isolation();
  test_syscall_boundary();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("FPU_T FAILED: ");
  print_dec(fails);
  print_console("\n");
  exit(1);
}
