/* src/user/cp/p8killprobe.c - lane C8-game (docs/cp-p8.md, stop 7): SIGKILL
 * to a thread group whose LEADER thread has already exited.
 *
 * This is the P8 game at the end of every boot: after the wineserver died,
 * the game's main thread was gone (THREAD_DONE, st=9) while its other
 * threads sat blocked on the dead server's pipes, and the launcher's
 * kill(pid, SIGKILL) + waitpid() never returned (run6: waited > 5 min).
 * Linux kills every thread of the group and the parent reaps it.
 *
 *   child:  pthread_create(a thread that blocks in read() on a pipe nobody
 *           writes), then pthread_exit() in main -- the leader is gone, the
 *           group lives on
 *   parent: sleep 1 s, kill(child, SIGKILL), poll waitpid(WNOHANG) for 10 s
 *
 * Rows: K1 the child is reaped within 10 s, K2 it reports SIGKILL.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pfd[2];

static void *blocker(void *arg) {
  (void)arg;
  char c;
  for (;;)
    if (read(pfd[0], &c, 1) < 0 && errno != EINTR) break;
  return 0;
}

static void sleep_ms(long ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, 0);
}

int main(void) {
  if (pipe(pfd) != 0) { perror("[P8KILL] pipe"); return 2; }
  pid_t pid = fork();
  if (pid < 0) { perror("[P8KILL] fork"); printf("[P8KILL] K1 fork failed -> FAIL\n"); return 2; }
  if (pid == 0) {
    pthread_t t;
    if (pthread_create(&t, 0, blocker, 0) != 0) _exit(3);
    pthread_exit(0);           /* the leader goes; the blocker keeps the group alive */
  }
  sleep_ms(1000);
  int k = kill(pid, SIGKILL);
  int st = 0, got = 0, waited = 0;
  for (; waited < 10000; waited += 100) {
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) { got = 1; break; }
    if (w < 0) break;
    sleep_ms(100);
  }
  printf("[P8KILL] kill rc=%d; K1 reaped=%d after %d ms -> %s\n", k, got, waited, got ? "PASS" : "FAIL");
  printf("[P8KILL] K2 status=0x%x signaled=%d sig=%d -> %s\n", st, got && WIFSIGNALED(st),
         got && WIFSIGNALED(st) ? WTERMSIG(st) : 0, got && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL ? "PASS" : "FAIL");
  fflush(stdout);
  return 0;
}
