/* tools/cp/exec/exec_child.c — the glibc 2.31 child linux_probe_exec execve()s
 * (cp/exec, docs/cp-exec.md).  Built by tools/cp/exec/exec_gate.sh twice against
 * the pinned sysroot: -static-pie (IRELATIVE: glibc's own startup relocates it)
 * and dynamic (PT_INTERP /lib64/ld-linux-x86-64.so.2, libc.so.6 from /LIB64).
 * argv[1] = tag, argv[2] = expected exit code; prints one [LP] row and checks
 * that argv and the MARK environment variable survived execve. */
#include <gnu/libc-version.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *tag = argc > 1 ? argv[1] : "?";
  int code = argc > 2 ? atoi(argv[2]) : 0;
  const char *mark = getenv("MARK");
  char exe[256];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  exe[n > 0 ? n : 0] = 0;
  printf("[LP] child glibc %s libc=%s pid=%d MARK=%s exe=%s\n", tag,
         gnu_get_libc_version(), (int)getpid(), mark ? mark : "(unset)", exe);
  fflush(stdout);
  return (mark && !strcmp(mark, "glibc")) ? code : 99;
}
