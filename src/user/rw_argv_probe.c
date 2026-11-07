/*
 * src/user/rw_argv_probe.c — M2 companion probe: does a compat process get the
 * argv the loader was asked for?
 *
 * The wave manifest carries "PROG.EXE one two" (the M2 harness split), and a
 * real program needs its arguments (Mono needs `hello.exe`, Unity needs
 * `-force-glcore`).  This prints exactly what it received, so a mismatch is
 * visible as data instead of inferred from a downstream loader error.
 */
#include <stdio.h>

int main(int argc, char **argv) {
  printf("[ARGV] argc=%d\n", argc);
  for (int i = 0; i < argc; i++)
    printf("[ARGV] argv[%d]=%s\n", i, argv[i]);
  printf("[ARGV] PASS\n");
  return 0;
}
