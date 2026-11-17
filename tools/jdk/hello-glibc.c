/* tools/jdk/hello-glibc.c -- the plumbing probe: a real glibc-2.31 x86_64 PIE.
 *
 * It exists to answer ONE question before the JDK is staged: does a dynamically
 * linked glibc program with PT_INTERP=/lib64/ld-linux-x86-64.so.2 start on this
 * kernel and reach main()?  The JDK's bin/java has exactly that shape (NEEDED
 * libjli.so + libc.so.6, max GLIBC_2.17), so if this runs, everything after it
 * is a JVM problem and not a loader problem.
 *
 * It deliberately touches the syscalls HotSpot also needs, so a failure names a
 * subsystem rather than "java died": stdio, malloc, clock_gettime, getpid,
 * uname, /proc/self/maps, and a pthread.
 */
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

static void *thread_main(void *arg) {
  int *n = (int *)arg;
  *n = 7 * 6;
  return NULL;
}

int main(void) {
  struct utsname u;
  struct timespec ts;
  int v = 0;

  printf("HELLO-GLIBC: start pid=%d\n", getpid());
  fflush(stdout);

  {
    char cb[4096];
    char *r;
    errno = 0;
    r = getcwd(cb, sizeof cb);
    printf("HELLO-GLIBC: getcwd -> %s (%s) errno=%d\n",
           r ? cb : "(null)", r ? "ok" : "FAIL", errno);
    errno = 0;
    if (chdir("/") != 0) printf("HELLO-GLIBC: chdir / errno=%d\n", errno);
    errno = 0;
    r = getcwd(cb, sizeof cb);
    printf("HELLO-GLIBC: getcwd after chdir -> %s (%s) errno=%d\n",
           r ? cb : "(null)", r ? "ok" : "FAIL", errno);
    /* Raw syscall: separates "the kernel answered wrong" from "glibc handled
       the answer wrong".  Linux getcwd returns the LENGTH incl. NUL; HobbyOS's
       compat shim returns the buffer pointer, which is also >= 0, so both look
       like success to a plain INLINE_SYSCALL. */
    memset(cb, 0xAA, sizeof cb);
    long raw = syscall(SYS_getcwd, cb, (long)sizeof cb);
    printf("HELLO-GLIBC: raw getcwd ret=%ld buf='%s'\n", raw, cb);
    fflush(stdout);
    fflush(stdout);
  }

  if (uname(&u) == 0)
    printf("HELLO-GLIBC: uname sysname=%s release=%s machine=%s\n",
           u.sysname, u.release, u.machine);
  else
    printf("HELLO-GLIBC: uname FAILED\n");

  clock_gettime(CLOCK_MONOTONIC, &ts);
  printf("HELLO-GLIBC: clock mono=%ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);

  char *p = malloc(1 << 20);
  if (p) { memset(p, 0x5a, 1 << 20); printf("HELLO-GLIBC: malloc 1M ok\n"); free(p); }
  else printf("HELLO-GLIBC: malloc 1M FAILED\n");

  FILE *f = fopen("/proc/self/maps", "r");
  if (f) {
    char line[256];
    int n = 0;
    while (n < 3 && fgets(line, sizeof line, f)) { printf("HELLO-GLIBC: map %s", line); n++; }
    fclose(f);
  } else {
    printf("HELLO-GLIBC: /proc/self/maps FAILED\n");
  }

  pthread_t t;
  if (pthread_create(&t, NULL, thread_main, &v) == 0) {
    pthread_join(t, NULL);
    printf("HELLO-GLIBC: thread ok v=%d\n", v);
  } else {
    printf("HELLO-GLIBC: pthread_create FAILED\n");
  }

  printf("HELLO-GLIBC: DONE\n");
  fflush(stdout);
  return 0;
}
