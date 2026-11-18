/*
 * src/user/linux_probe_badelr.c — the scheduler's [BADELR] plausibility test
 * must not flag a thread resumed into a legal low mapping (H4-kglitch,
 * docs/cp-kglitch.md §6).  Every busy PE thread of the game (image at
 * 0x140000000, below USER_VA_BASE) printed "[BADELR] picking a user slot with
 * an implausible frame.elr=0x1400..." until the 20-row cap, so the instrument
 * was blind for a real bad frame.
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 *   B1  a child spins in `jmp .` at 0x140000000 (MAP_FIXED RWX) for 1.5 s on
 *       two threads, so the tick preempts and resumes it there many times;
 *       then SIGKILL.  tools/gate/lp_check_badelr.py fails the run if the
 *       console carries any [BADELR] row.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPIN_VA 0x140000000UL

static void *spin(void *code) {
  ((void (*)(void))code)();
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  pid_t pid = fork();
  if (pid == 0) {
    unsigned char *p = mmap((void *)SPIN_VA, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
      _exit(4);
    p[0] = 0xEB; /* jmp . */
    p[1] = 0xFE;
    pthread_t t;
    pthread_create(&t, 0, spin, p);
    spin(p);
    _exit(0);
  }
  usleep(1500000);
  kill(pid, SIGKILL);
  int st = 0;
  waitpid(pid, &st, 0);
  int ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
  printf("[LP] B1 %-44s status=0x%x -> %s (the console check decides)\n",
         "two threads spin at 0x140000000 for 1.5 s", st, ok ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
