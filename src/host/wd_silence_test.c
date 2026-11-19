/* wd_silence_test.c -- host test of the stall watchdog's silence test
 * (src/include/wd_silence.h; H4-kglitch, cp-hunt H1-14).  Build with
 * -DWD_SILENCE_OLD to run it against integ15's expression (it fails W2). */
#include <stdio.h>
#include "wd_silence.h"

static int fails;
static void t(const char *name, int got, int want) {
  printf("%s %s (got %d, want %d)\n", got == want ? "ok  " : "FAIL", name, got, want);
  fails += got != want;
}

int main(void) {
  t("W1 printed 5 s ago: not silent", wd_console_silent(100000, 95000, 20000), 0);
  t("W2 stamped 1 ms after now (the tick's own [NFSIO] row): not silent",
    wd_console_silent(100000, 100001, 20000), 0);
  t("W3 stamped at now: not silent", wd_console_silent(100000, 100000, 20000), 0);
  t("W4 quiet for 25 s: silent", wd_console_silent(100000, 75000, 20000), 1);
  t("W5 quiet for exactly 20 s: silent", wd_console_silent(100000, 80000, 20000), 1);
  printf("wd_silence_test: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
