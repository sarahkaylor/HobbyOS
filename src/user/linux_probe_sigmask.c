/*
 * src/user/linux_probe_sigmask.c — a process the kernel creates from scratch
 * (spawn_ex, the wave loader) starts with an EMPTY signal mask, whatever the
 * pid slot's previous occupant left (H4-kglitch round 2, docs/cp-kglitch.md
 * §9; H2-F2: `wineserver -k`'s SIGINT ignored for 10.5 s).
 *
 * ORDINARY musl Linux program, run by the native driver
 * tools/cp/kglitch/kgmask.c (tools/cp/kglitch/sigmask-gate.sh), never as a
 * wave member of its own:
 *   PROBE.ELF fill   forks FILL children; each blocks SIGINT (as every Wine
 *                    client does: ntdll's server_block_set) and _exits, so
 *                    every pid slot's last occupant died with SIGINT blocked
 *   PROBE.ELF check  prints its own mask at entry and exits 1 if SIGINT is in
 *                    it.  p8launch spawn_ex()es `wine`, which forks the
 *                    session's wineserver: with a stale mask the server ran
 *                    with SIGINT blocked and never saw `wineserver -k`.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILL 600

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc > 1 && !strcmp(argv[1], "fill")) {
    int made = 0;
    for (int i = 0; i < FILL; i++) {
      pid_t p = fork();
      if (p == 0) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGINT);
        sigaddset(&s, SIGCHLD);
        sigprocmask(SIG_BLOCK, &s, 0);
        _exit(0);
      }
      if (p > 0) {
        made++;
        waitpid(p, 0, 0);
      }
    }
    printf("[LP] fill: %d children exited with SIGINT blocked\n", made);
    return made == FILL ? 0 : 1;
  }
  unsigned long m = 0;
  syscall(SYS_rt_sigprocmask, SIG_BLOCK, 0, &m, 8);
  int bad = (m >> (SIGINT - 1)) & 1;
  printf("[LP] M1 %-44s pid=%d mask=0x%lx sigint_blocked=%d -> %s\n",
         "a spawned process starts with an empty mask", (int)getpid(), m, bad, m ? "FAIL" : "PASS");
  if (argc < 2) /* a plain gate-2 run (the wave loader's fresh slot): grade it alone */
    printf("[LP] done rc=%d\n", m ? 1 : 0);
  return m ? 1 : 0;
}
