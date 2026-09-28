/*
 * PROCCHLD.BIN — child binary for the Phase-3 process test
 * (src/user/proc_test.c). Exits with the code given as argv[1]
 * (default 42), so the parent can verify fork+exec+waitpid end to end:
 * if the exec'd image or its argv were wrong, the exit status differs
 * and the parent's check fails.
 */
#include <stdlib.h>

/* The old-trio libc has no atoi; every tool keeps its own tiny parser. */
static int p_atoi(const char *s) {
  int v = 0;
  if (*s == '-') s++;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

int main(int argc, char **argv) {
  extern void print_console(const char *s);
  print_console("[PROCCHLD] running\n");
  if (argc >= 2)
    return p_atoi(argv[1]);
  return 7;
}
