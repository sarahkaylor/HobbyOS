/*
 * console_test.c - Host unit tests for src/user/console.c (CONSOLE.BIN).
 *
 * The app source is included directly so both of its roles are driven:
 *   - console_run_smoke(): the test-mode boot smoke — runs, returns 0.
 *   - console_run_shell(): the interactive handoff — prints a banner and
 *     spawns SH.BIN with the console's own stdin/stdout/stderr inherited
 *     (the spawn is recorded by the compat mock, never executed on the
 *     host), plus its failure path when the spawn fails.
 *
 * main()'s build-time selection (KERNEL_MODE_TEST -> smoke, everything
 * else -> shell) is a two-line #ifdef; it is exercised end-to-end by the
 * mode-specific QEMU runs (run_console_test.py for the desktop window,
 * the MODE=test smoke log for the boot smoke).
 */

#include <stdio.h>
#include <string.h>

#include "../user_include/libc.h"

/* Rename the app entry point so the test can define its own main(). */
#define main console_app_main
#include "../user/console.c"
#undef main

/* compat spawn2 test hook */
extern int mock_spawn2_intercept;
extern int mock_spawn2_result;
extern char mock_spawn2_last_file[64];
extern int mock_spawn2_last_stdin;
extern int mock_spawn2_last_stdout;
extern int mock_spawn2_last_stderr;

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *what) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", what);
  } else {
    printf("  FAIL: %s\n", what);
    checks_failed++;
  }
}

int main(void) {
  printf("[TEST] console app host tests\n");

  /* --- smoke role (what MODE=test compiles into main) --- */
  check(console_run_smoke() == 0, "smoke path runs and returns 0");

  /* --- interactive role: banner + SH.BIN handoff --- */
  mock_spawn2_intercept = 1;
  mock_spawn2_result = 99;
  mock_spawn2_last_file[0] = '\0';
  int r = console_run_shell();
  check(r == 1, "shell path reports success when the spawn succeeds");
  check(strcmp(mock_spawn2_last_file, "SH.BIN") == 0,
        "spawns exactly SH.BIN");
  check(mock_spawn2_last_stdin == 0 && mock_spawn2_last_stdout == 1 &&
            mock_spawn2_last_stderr == 1,
        "shell inherits the console's stdin/stdout/stderr");

  /* --- spawn failure is reported, not a crash --- */
  mock_spawn2_result = -1;
  r = console_run_shell();
  check(r == 0, "shell path reports failure when the spawn fails");

  mock_spawn2_intercept = 0;

  printf("[TEST] console app: %d checks, %d failures\n", checks_run,
         checks_failed);
  return checks_failed ? 1 : 0;
}
