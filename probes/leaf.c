/* probes/leaf.c -- the native binary a parent is supposed to be able to exec.
 *
 * It is the C twin of probes/Leaf.java and exists so that "exec a native
 * binary" can be judged without a JVM in the way.  It is a real glibc-2.31
 * PIE with PT_INTERP, i.e. exactly the shape /JDK/bin/java has, so a failure
 * here is the loader/exec path and not HotSpot.
 *
 *   leaf [--exit=N] [--echo] [--sleep=MS] [--stderr] [--sig=default|handle]
 *
 * Line-buffered and flushed after every write: under TCG the console is the
 * only witness, and a half-flushed buffer looks identical to a wedge.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int got_sig;

static void on_sig(int s) {
  got_sig = s;
  char b[64];
  int n = snprintf(b, sizeof b, "LEAF-NATIVE: signal %d delivered\n", s);
  if (write(1, b, n) < 0) { }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);

  int code = 0, echo = 0, sleep_ms = 0, to_err = 0, sig_default = 0;
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "--exit=", 7)) code = atoi(argv[i] + 7);
    else if (!strcmp(argv[i], "--echo")) echo = 1;
    else if (!strncmp(argv[i], "--sleep=", 8)) sleep_ms = atoi(argv[i] + 8);
    else if (!strcmp(argv[i], "--stderr")) to_err = 1;
    else if (!strcmp(argv[i], "--sig=default")) sig_default = 1;
    else if (!strcmp(argv[i], "--sig=handle")) sig_default = 0;
  }

  printf("LEAF-NATIVE: start pid=%d ppid=%d argc=%d\n", (int)getpid(),
         (int)getppid(), argc);
  for (int i = 0; i < argc; i++) printf("LEAF-NATIVE: argv[%d]=%s\n", i, argv[i]);
  { char cwd[256]; printf("LEAF-NATIVE: cwd=%s\n", getcwd(cwd, sizeof cwd) ? cwd : "(fail)"); }
  printf("LEAF-NATIVE: env.PROBE=%s\n", getenv("PROBE") ? getenv("PROBE") : "(unset)");
  if (to_err) fprintf(stderr, "LEAF-NATIVE: this line went to stderr\n");

  if (!sig_default) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0) printf("LEAF-NATIVE: sigaction TERM errno=%d\n", errno);
    if (sigaction(SIGINT, &sa, NULL) != 0) printf("LEAF-NATIVE: sigaction INT errno=%d\n", errno);
    printf("LEAF-NATIVE: handlers installed\n");
  } else {
    signal(SIGTERM, SIG_DFL);
    printf("LEAF-NATIVE: handlers default\n");
  }

  if (echo) {
    char buf[512];
    ssize_t n;
    int lines = 0;
    printf("LEAF-NATIVE: echo start\n");
    while ((n = read(0, buf, sizeof buf)) > 0) {
      for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
      printf("LEAF-NATIVE: echo got %d bytes\n", (int)n);
    }
    printf("LEAF-NATIVE: echo eof n=%d errno=%d lines=%d\n", (int)n, errno, lines);
  }

  if (sleep_ms > 0) {
    printf("LEAF-NATIVE: sleeping %dms\n", sleep_ms);
    usleep((useconds_t)sleep_ms * 1000);
    if (got_sig) printf("LEAF-NATIVE: woke early after signal %d\n", got_sig);
  }

  printf("LEAF-NATIVE: exiting code=%d\n", code);
  fflush(stdout);
  return code;
}
