/*
 * PROCCHLD.BIN — child binary for the Phase-3 process test
 * (src/user/proc_test.c). Exits with the code given as argv[1]
 * (default 42), so the parent can verify fork+exec+waitpid end to end:
 * if the exec'd image or its argv were wrong, the exit status differs
 * and the parent's check fails.
 */
#include <stdlib.h>

int main(int argc, char **argv) {
  extern void print_console(const char *s);
  print_console("[PROCCHLD] running\n");
  if (argc >= 2)
    return atoi(argv[1]);
  return 7;
}
