/*
 * src/user/linux_probe_loaccept.c — a blocking accept() on a loopback TCP
 * listener sleeps until a connection comes (H4-kglitch round 2, K11).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 *   A1  the main thread blocks in accept(); a second thread connects 500 ms
 *       later.  accept returns that connection, and the process used under
 *       100 ms of CPU meanwhile (a busy-yield wait burns ~500 ms)
 *   A2  a byte sent on the connection arrives
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 47321

static long cpu_us(void) {
  struct rusage r;
  getrusage(RUSAGE_SELF, &r);
  return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000L + r.ru_utime.tv_usec + r.ru_stime.tv_usec;
}

static void *connector(void *a) {
  (void)a;
  usleep(500000);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in lo;
  memset(&lo, 0, sizeof lo);
  lo.sin_family = AF_INET;
  lo.sin_port = htons(PORT);
  inet_pton(AF_INET, "127.0.0.1", &lo.sin_addr);
  if (connect(s, (struct sockaddr *)&lo, sizeof lo) == 0)
    (void)!write(s, "k", 1);
  usleep(300000);
  close(s);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fails = 0;
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in lo;
  memset(&lo, 0, sizeof lo);
  lo.sin_family = AF_INET;
  lo.sin_port = htons(PORT);
  inet_pton(AF_INET, "127.0.0.1", &lo.sin_addr);
  int one = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(l, (struct sockaddr *)&lo, sizeof lo) || listen(l, 4)) {
    printf("[LP] A1 bind/listen failed errno=%d -> FAIL\n[LP] done rc=1\n", errno);
    return 1;
  }
  pthread_t t;
  pthread_create(&t, 0, connector, 0);
  long c0 = cpu_us();
  struct timespec w0, w1;
  clock_gettime(CLOCK_MONOTONIC, &w0);
  int a = accept(l, 0, 0);
  clock_gettime(CLOCK_MONOTONIC, &w1);
  long cpu = cpu_us() - c0;
  long wall = (w1.tv_sec - w0.tv_sec) * 1000L + (w1.tv_nsec - w0.tv_nsec) / 1000000L;
  int ok = a >= 0 && cpu < 100000 && wall >= 400;
  printf("[LP] A1 %-44s fd=%d wall_ms=%ld cpu_us=%ld -> %s\n", "blocking loopback accept sleeps", a, wall, cpu,
         ok ? "PASS" : "FAIL");
  fails += !ok;
  char c = 0;
  int n = a >= 0 ? (int)read(a, &c, 1) : -1;
  ok = n == 1 && c == 'k';
  printf("[LP] A2 %-44s n=%d -> %s\n", "the byte sent arrives", n, ok ? "PASS" : "FAIL");
  fails += !ok;
  pthread_join(t, 0);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
