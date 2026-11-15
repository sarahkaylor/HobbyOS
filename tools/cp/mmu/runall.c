/* tools/cp/mmu/runall.c -- lane C1-mmu smoke harness (NOT a gate of record).
 *
 * One Linux-ABI process that runs every staged probe in turn, so an
 * intermediate migration step can be smoke-tested with ONE guest boot instead
 * of one boot per probe (the shared linuxci box serialises QEMU boots).  The
 * gates of record remain tools/gate/t3_linux_probe.sh <name>, one boot each.
 *
 * Reads /RUNALL.TXT: one "<name> <path>" per line.  For each: fork, execve the
 * path, waitpid, print "[LPRUN] <name> status=<wstatus> exit=<code>".  Ends
 * with "[LPRUN] done ran=<n> nonzero=<k>".  Build: musl-gcc -static-pie. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  static char buf[4096];
  int fd = open("/RUNALL.TXT", O_RDONLY);
  if (fd < 0) {
    printf("[LPRUN] done ran=0 nonzero=1 (no /RUNALL.TXT)\n");
    return 1;
  }
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) n = 0;
  buf[n] = 0;
  int ran = 0, bad = 0;
  char *save = 0;
  for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(0, "\n", &save)) {
    char name[64], path[128];
    if (sscanf(ln, "%63s %127s", name, path) != 2) continue;
    printf("[LPRUN] start %s %s\n", name, path);
    fflush(stdout);
    pid_t c = fork();
    if (c == 0) {
      char *argv[] = {path, 0};
      char *envp[] = {0};
      execve(path, argv, envp);
      printf("[LPRUN] %s execve failed\n", name);
      _exit(127);
    }
    int st = 0;
    if (c < 0 || waitpid(c, &st, 0) != c) {
      printf("[LPRUN] %s fork/wait failed\n", name);
      bad++;
      continue;
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    printf("[LPRUN] %s status=0x%x exit=%d\n", name, st, code);
    fflush(stdout);
    ran++;
    if (code) bad++;
  }
  printf("[LPRUN] done ran=%d nonzero=%d\n", ran, bad);
  return bad ? 1 : 0;
}
