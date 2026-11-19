/*
 * src/user/linux_probe_tcpconn.c — a BLOCKING TCP connect() to a non-loopback
 * address blocks until the handshake ends; it never answers EINPROGRESS
 * (H4-kglitch round 2, K10).  The guest's user-mode network puts the host at
 * 10.0.2.2; the NFS server the gates mount listens on its port 2049.
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 *   C1  blocking connect(10.0.2.2:2049): 0 (or a real refusal), not EINPROGRESS
 *   C2  O_NONBLOCK connect to the same address: 0 or EINPROGRESS (both legal),
 *       and an EINPROGRESS socket becomes writable with SO_ERROR 0 within 5 s
 *   C3  blocking connect to a loopback listener of our own: 0
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;

static struct sockaddr_in addr(const char *ip, int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  inet_pton(AF_INET, ip, &a.sin_addr);
  return a;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  struct sockaddr_in h = addr("10.0.2.2", 2049);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int r = connect(s, (struct sockaddr *)&h, sizeof h);
  int e = r ? errno : 0;
  int ok = r == 0 || (e != EINPROGRESS && e != EALREADY && e != EAGAIN);
  printf("[LP] C1 %-44s rc=%d errno=%d -> %s\n", "blocking TCP connect(10.0.2.2:2049)", r, e, ok ? "PASS" : "FAIL");
  fails += !ok;
  close(s);

  s = socket(AF_INET, SOCK_STREAM, 0);
  fcntl(s, F_SETFL, O_NONBLOCK);
  r = connect(s, (struct sockaddr *)&h, sizeof h);
  e = r ? errno : 0;
  int soerr = -1;
  if (r == 0) {
    soerr = 0;
  } else if (e == EINPROGRESS) {
    struct pollfd pf = {s, POLLOUT, 0};
    if (poll(&pf, 1, 5000) == 1) {
      socklen_t l = sizeof soerr;
      getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &l);
    }
  }
  ok = soerr == 0;
  printf("[LP] C2 %-44s rc=%d errno=%d so_error=%d -> %s\n", "O_NONBLOCK connect completes", r, e, soerr, ok ? "PASS" : "FAIL");
  fails += !ok;
  close(s);

  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in lo = addr("127.0.0.1", 47123);
  int one = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  r = bind(l, (struct sockaddr *)&lo, sizeof lo) || listen(l, 4);
  s = socket(AF_INET, SOCK_STREAM, 0);
  int c = r ? -1 : connect(s, (struct sockaddr *)&lo, sizeof lo);
  e = c ? errno : 0;
  ok = c == 0;
  printf("[LP] C3 %-44s rc=%d errno=%d -> %s\n", "blocking loopback connect", c, e, ok ? "PASS" : "FAIL");
  fails += !ok;
  close(s);
  close(l);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
