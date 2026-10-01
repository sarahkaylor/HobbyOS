/*
 * PROCCHLD.BIN — child binary for the Phase-3 process test
 * (src/user/proc_test.c). Exits with the code given as argv[1]
 * (default 42), so the parent can verify fork+exec+waitpid end to end:
 * if the exec'd image or its argv were wrong, the exit status differs
 * and the parent's check fails.
 *
 * P5 (docs/browser/p5-exec-signals-design.md D3) adds two probe modes:
 *   "env"          exit with PROCTEST_ENV's value (99 when unset) -- the
 *                  parent passes envp to execve(), so 33 proves the env
 *                  blob reached getenv().
 *   "fds A B"      close(A) must fail EBADF (A had FD_CLOEXEC and was
 *                  swept at exec) and close(B) must succeed (kept);
 *                  exit 0 on that pair, 1 otherwise.
 */
#include <stdlib.h>
#include <unistd.h>

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

  if (argc >= 2 && argv[1] && argv[1][0] == 'e' && argv[1][1] == 'n') {
    char *v = getenv("PROCTEST_ENV");
    return v ? p_atoi(v) : 99;
  }
  if (argc >= 4 && argv[1] && argv[1][0] == 'f' && argv[1][1] == 'd') {
    int a = p_atoi(argv[2]);
    int b = p_atoi(argv[3]);
    int ra = close(a);
    int rb = close(b);
    return (ra == -1 && rb == 0) ? 0 : 1;
  }
  if (argc >= 2)
    return p_atoi(argv[1]);
  return 7;
}
