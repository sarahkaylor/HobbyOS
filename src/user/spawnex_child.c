/*
 * SPAWNEX.BIN — child probe for the P5 S5 spawn_ex acceptance test
 * (src/user/sig_test.c).  Proves the D4 fd semantics from the CHILD's
 * side: the parent copies its fd table, applies the fd map and sweeps
 * FD_CLOEXEC before this image runs.
 *
 * Modes (argv[1]):
 *   "probe"  (default): check argv[2] == "hello" (argv round-trip), that
 *            getenv("P5EX") is visible (envp blob), that the fd named by
 *            getenv("P5K") is OPEN (parent table copy) and the fd named
 *            by getenv("P5C") is CLOSED (CLOEXEC sweep).  Report the four
 *            findings as "ARG=... EX=... K=... C=..." to fd 7 — the
 *            spawn_ex-mapped pipe write end — then exit 7.
 *   "empty": exit 9 when getenv("P5EX") is absent (envp == NULL means an
 *            empty environment, D3.1), 8 when it is set.  No fd use.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* The old-trio libc has no atoi; keep the tiny parser (proc_child rule). */
static int p_atoi(const char *s) {
  int v = 0;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

/* fd-liveness probe (skill rule: fcntl(F_GETFD) is NOT a probe; a real
 * close() must fail EBADF for a closed slot).  Runs at the very end of
 * the child, so destroying an open probe fd is harmless. */
static int fd_is_open(int fd) {
  if (fd < 0)
    return 0;
  return close(fd) == 0;
}

int main(int argc, char **argv) {
  const char *mode = (argc > 1 && argv[1]) ? argv[1] : "probe";

  if (mode[0] == 'e' && mode[1] == 'm') {
    /* envp == NULL spawn: the environment must be empty. */
    return getenv("P5EX") ? 8 : 9;
  }

  char rep[200];
  rep[0] = '\0';
  strcpy(rep, "ARG=");
  strcat(rep, (argc > 2 && argv[2] && strcmp(argv[2], "hello") == 0) ? "hello"
                                                                    : "bad");
  strcat(rep, " EX=");
  {
    char *ev = getenv("P5EX");
    strcat(rep, ev ? ev : "none");
  }
  {
    char *ke = getenv("P5K");
    char *ce = getenv("P5C");
    strcat(rep, " K=");
    strcat(rep, (ke && fd_is_open(p_atoi(ke))) ? "open" : "closed");
    strcat(rep, " C=");
    strcat(rep, (ce && fd_is_open(p_atoi(ce))) ? "open" : "closed");
  }

  /* fd 7 is the spawn_ex fdmap destination (the pipe write end). */
  size_t len = strlen(rep);
  ssize_t w = write(7, rep, len);
  return (w == (ssize_t)len) ? 7 : 6;
}
