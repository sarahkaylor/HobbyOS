/* SOCK2TST.BIN — Phase F1 (browser.md F1.1–F1.3): user-level acceptance for
 * the frozen socket/select surface (syscalls 65–71, libc.h).
 *
 * Two sections:
 *   hard — offline checks that must pass on any machine: fd-table error
 *          paths (EBADF/ENOTSOCK/EAFNOSUPPORT/EPROTONOSUPPORT/ENOPROTOOPT),
 *          fcntl(F_GETFL/F_SETFL) round-trip, SO_TYPE/SO_ERROR/SO_REUSEADDR,
 *          FD_SET/FD_ISSET macro sanity, select() argument validation.
 *          Any failure fails the program.
 *   net  — live checks against 1.1.1.1:80 (the target NETTEST.BIN already
 *          uses): non-blocking connect completion via select() writability
 *          + SO_ERROR, then an HTTP GET round-trip.  If the target is
 *          unreachable the section prints SKIP lines and does not fail the
 *          exit code; the hard section still decides it.
 *
 * Output convention: "  SOCK2TST <name>: PASS/FAIL/SKIP" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "SOCK2TST FAILED: n".  Self-
 * terminating (exit 0/1), no long sleeps.
 */

#include "libc.h"
#include <stdint.h>

/* 1.1.1.1 in network byte order. */
#define NET2_IP_1_1_1_1 0x01010101u
#define NET2_PORT_80 80

static int fails;
static int skips;

static void check(const char *name, int ok) {
  print_console("  SOCK2TST ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok) fails++;
}

static void skip(const char *name) {
  print_console("  SOCK2TST ");
  print_console(name);
  print_console(": SKIP (network target unreachable)\n");
  skips++;
}

/* Host -> network byte order for a 16-bit port (no htons in the kernel
 * libc; the value is what the frozen connect_fd ABI expects). */
static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static uint64_t now_ms(void) {
  struct sys_cpuinfo ci;
  ci.uptime_ms = 0;
  ci.total_idle_ms = 0;
  ci.num_cpus = 0;
  sysinfo(1, &ci, sizeof(ci));
  return ci.uptime_ms;
}

/* ---- hard section (offline) ------------------------------------------- */

static void hard_socket_error_paths(void) {
  errno = 0;
  int r = socket(AF_UNIX, SOCK_STREAM, 0);
  check("socket(AF_UNIX) -> -1/EAFNOSUPPORT", r == -1 && errno == EAFNOSUPPORT);

  errno = 0;
  r = socket(AF_INET, SOCK_STREAM, IPPROTO_UDP);
  check("socket(STREAM,UDP) -> -1/EPROTONOSUPPORT",
        r == -1 && errno == EPROTONOSUPPORT);

  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  check("socket(AF_INET,SOCK_STREAM,TCP) >= 0", fd >= 0);
  if (fd < 0) return;

  int type = 0;
  int len = (int)sizeof(type);
  r = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len);
  check("getsockopt(SO_TYPE) == SOCK_STREAM after socket()",
        r == 0 && type == SOCK_STREAM && len == (int)sizeof(int));

  int soerr = -1;
  len = (int)sizeof(soerr);
  r = getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
  check("getsockopt(SO_ERROR) == 0 on a fresh socket", r == 0 && soerr == 0);

  errno = 0;
  len = (int)sizeof(type);
  r = getsockopt(fd, SOL_SOCKET, 1234 /* unknown option */, &type, &len);
  check("getsockopt(unknown option) -> -1/ENOPROTOOPT",
        r == -1 && errno == ENOPROTOOPT);

  errno = 0;
  r = getsockopt(fd, 7 /* bad level */, SO_TYPE, &type, &len);
  check("getsockopt(bad level) -> -1/ENOPROTOOPT", r == -1 && errno == ENOPROTOOPT);

  int one = 1;
  r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
  check("setsockopt(SO_REUSEADDR) accepted as a no-op", r == 0);

  r = fcntl(fd, F_GETFL, 0);
  check("fcntl(F_GETFL) == 0 on a fresh socket", r == 0);
  r = fcntl(fd, F_SETFL, O_NONBLOCK);
  check("fcntl(F_SETFL, O_NONBLOCK) == 0", r == 0);
  r = fcntl(fd, F_GETFL, 0);
  check("fcntl(F_GETFL) == O_NONBLOCK after the set", r == O_NONBLOCK);
  r = fcntl(fd, F_SETFL, 0);
  check("fcntl(F_SETFL, 0) clears O_NONBLOCK",
        r == 0 && fcntl(fd, F_GETFL, 0) == 0);
  errno = 0;
  r = fcntl(fd, 77 /* unknown cmd */, 0);
  check("fcntl(unknown cmd) -> -1/EINVAL", r == -1 && errno == EINVAL);

  errno = 0;
  r = connect_fd(999, NET2_IP_1_1_1_1, be16(NET2_PORT_80));
  check("connect_fd(bad fd) -> -1/EBADF", r == -1 && errno == EBADF);
  errno = 0;
  r = fcntl(999, F_GETFL, 0);
  check("fcntl(bad fd) -> -1/EBADF", r == -1 && errno == EBADF);
  errno = 0;
  len = (int)sizeof(type);
  r = getsockopt(999, SOL_SOCKET, SO_TYPE, &type, &len);
  check("getsockopt(bad fd) -> -1/EBADF", r == -1 && errno == EBADF);

  /* connect_fd on a non-socket fd (pipe write end) -> ENOTSOCK. */
  int pfd[2] = {-1, -1};
  if (pipe(pfd) == 0) {
    errno = 0;
    r = connect_fd(pfd[1], NET2_IP_1_1_1_1, be16(NET2_PORT_80));
    check("connect_fd(non-socket) -> -1/ENOTSOCK", r == -1 && errno == ENOTSOCK);
    close(pfd[0]);
    close(pfd[1]);
  } else {
    check("pipe() available for the non-socket check", 0);
  }

  close(fd);
}

static void hard_fd_set_macros(void) {
  fd_set s;
  FD_ZERO(&s);
  FD_SET(3, &s);
  FD_SET(255, &s);
  check("FD_SET/FD_ISSET round-trip (3 and 255)",
        FD_ISSET(3, &s) && FD_ISSET(255, &s));
  check("unset bits stay clear (4, 200)",
        !FD_ISSET(4, &s) && !FD_ISSET(200, &s));
  FD_CLR(3, &s);
  check("FD_CLR clears only its bit", !FD_ISSET(3, &s) && FD_ISSET(255, &s));
  check("fd_set is FD_SETSIZE/32 32-bit words",
        (int)sizeof(fd_set) == (FD_SETSIZE / 32) * 4);
}

static void hard_select_args(void) {
  fd_set rd;
  FD_ZERO(&rd);

  errno = 0;
  int r = select(FD_SETSIZE + 1, &rd, 0, 0, 0);
  check("select(nfds > FD_SETSIZE) -> -1/EINVAL", r == -1 && errno == EINVAL);

  FD_SET(42, &rd); /* >= MAX_OPEN_FDS (32): not open */
  errno = 0;
  r = select(64, &rd, 0, 0, 0);
  check("select(unopened fd in the mask) -> -1/EBADF", r == -1 && errno == EBADF);

  r = select(0, 0, 0, 0, 0);
  check("select(0, NULL, NULL, NULL, 0) == 0", r == 0);
}

/* ---- net section (live, SKIP-able) ------------------------------------ */

/* Poll for writability with select() in 250ms steps for up to
 * `timeout_ms_total`.  Returns 1 ready, 0 timed out, -1 select error. */
static int net_wait_writable(int fd, int timeout_ms_total) {
  uint64_t t0 = now_ms();
  while ((int)(now_ms() - t0) < timeout_ms_total) {
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    int n = select(fd + 1, 0, &wr, 0, 250);
    if (n > 0 && FD_ISSET(fd, &wr)) return 1;
    if (n < 0) return -1;
  }
  return 0;
}

static void net_section(void) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    skip("socket() for the live section");
    return;
  }
  if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
    skip("fcntl(F_SETFL, O_NONBLOCK) before connect");
    close(fd);
    return;
  }

  errno = 0;
  int r = connect_fd(fd, NET2_IP_1_1_1_1, be16(NET2_PORT_80));
  if (r == 0) {
    check("connect_fd() completed at once (fast path)", 1);
  } else if (r == -1 && errno == EINPROGRESS) {
    check("connect_fd() -> -1/EINPROGRESS (handshake started)", 1);
    int wr = net_wait_writable(fd, 15000);
    if (wr <= 0) {
      skip("select() writability on connect completion");
      skip("SO_ERROR after connect completion");
      skip("HTTP GET round-trip on the live socket");
      close(fd);
      return;
    }
    check("select() reports the connecting socket writable", 1);
  } else {
    print_console("  SOCK2TST connect_fd to 1.1.1.1:80: SKIP (errno=");
    print_dec(errno);
    print_console(")\n");
    skips++;
    close(fd);
    return;
  }

  /* Writable: SO_ERROR carries the handshake outcome. */
  int soerr = -12345;
  int len = (int)sizeof(soerr);
  r = getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
  if (r != 0 || soerr != 0) {
    print_console("  SOCK2TST SO_ERROR after connect: SKIP (errno=");
    print_dec(soerr);
    print_console(")\n");
    skips++;
    close(fd);
    return;
  }
  check("SO_ERROR == 0 after a completed non-blocking connect", 1);

  int type = -1;
  len = (int)sizeof(type);
  r = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len);
  check("SO_TYPE still SOCK_STREAM after connect",
        r == 0 && type == SOCK_STREAM);

  errno = 0;
  r = connect_fd(fd, NET2_IP_1_1_1_1, be16(NET2_PORT_80));
  check("connect_fd(already connected) -> -1/EINVAL",
        r == -1 && errno == EINVAL);

  const char *req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
  int n = 0;
  while (req[n]) n++;
  int wrote = (int)write(fd, req, n);
  check("write(HTTP GET) == request length", wrote == n);

  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  r = select(fd + 1, &rd, 0, 0, 10000);
  if (r > 0 && FD_ISSET(fd, &rd)) {
    check("select() reports the socket readable with a response pending", 1);
    char buf[128];
    int got = (int)read(fd, buf, sizeof(buf) - 1);
    if (got > 0) {
      buf[got] = 0;
      print_console("  SOCK2TST response prefix: ");
      for (int i = 0; i < got; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') break;
        char c[2];
        c[0] = buf[i];
        c[1] = 0;
        print_console(c);
      }
      print_console("\n");
      check("read() returned response bytes", 1);
    } else {
      check("read() returned response bytes", 0);
    }
  } else {
    skip("select(readable) / read() on the live socket");
  }

  close(fd);
}

__attribute__((section(".text._start"))) void _start(void) {
  print_console("SOCK2TST: F1 socket/select acceptance (browser.md F1.1-F1.3)\n");

  hard_socket_error_paths();
  hard_fd_set_macros();
  hard_select_args();
  net_section();

  if (fails == 0) {
    if (skips > 0) {
      print_console("SOCK2TST: hard section PASSED, ");
      print_dec(skips);
      print_console(" network check(s) SKIPPED (target unreachable)\n");
    }
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("SOCK2TST FAILED: ");
  print_dec(fails);
  print_console(" check(s)\n");
  exit(1);
}
