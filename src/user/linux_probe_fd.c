/* src/user/linux_probe_fd.c — Linux-ABI probe: descriptors, sockets, readiness.
 *
 * An ORDINARY musl x86_64 Linux program: no HobbyOS header, no HobbyOS-ism,
 * so the same source runs on this workstation (the reference) and inside
 * HobbyOS through the compat layer (the measurement).  Rows are prefixed
 * "[LP] " and the last one is "[LP] done rc=<failures>"; tools/gate/
 * t3_linux_probe.sh accepts only rc=0.
 *
 * What it pins down, in the order the Doom/libX11 transport needs it:
 *   1. socketpair(AF_UNIX, SOCK_STREAM) + write/read on BOTH ends
 *   2. socket(AF_UNIX) + connect() to a name that does not exist
 *      (ENOENT: it must FAIL FAST, not park forever)
 *   3. poll() with data ready, with a timeout, and POLLHUP on peer close
 *   4. select() ready / timeout
 *   5. fcntl F_GETFL/F_SETFL O_NONBLOCK, ioctl FIONBIO/FIONREAD
 *   6. dup / dup2 / dup3, pipe2, close_range
 *   7. bind + listen + connect + accept on a NAMED unix socket, then
 *      getsockname/getpeername, getsockopt/setsockopt, sendto/recvfrom,
 *      shutdown(SHUT_WR) -> EOF  (the exact X11 client/server shape)
 *
 * Checks are behavioural, never "matches the value HobbyOS returns": every
 * expectation below is what Linux does, and the host run is the proof.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fails;

/* This build's musl <unistd.h> has no close_range() wrapper; use the generic
 * syscall entry (the number is the x86_64 ABI's 436). */
#include <sys/syscall.h>
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

static void row(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

/* One check: prints the row and counts a failure.  The whole line is built
 * in one buffer and written with ONE write(): the guest console is shared
 * with other processes, and two writes let a kernel diagnostic land in the
 * middle of a row (measured: "[LP] socketpair [RESUME-SPSR] ..."). */
static void verdict(const char *name, int cond, const char *fmt, ...) {
  char b[320];
  int n = snprintf(b, sizeof b, "[LP] %s ", name);
  if (n < 0) n = 0;
  if ((size_t)n < sizeof b) {
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
  }
  if ((size_t)n < sizeof b)
    n += snprintf(b + n, sizeof b - (size_t)n, " %s\n", cond ? "ok" : "BAD");
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
  if (!cond) fails++;
}

static long long now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A long name for the X11-shaped socket (a filesystem path on Linux, a
 * kernel-table name on HobbyOS — the guest has no filesystem socket inodes). */
static const char LP_PATH[] = "/tmp/lpfd-probe.sock";

int main(void) {
  int sv[2] = {-1, -1};
  int ok;
  struct sockaddr_un sa;

  /* ---- 1. socketpair + bidirectional traffic ------------------------- */
  errno = 0;
  int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  ok = (r == 0 && sv[0] >= 3 && sv[1] >= 3 && sv[0] != sv[1]);
  verdict("socketpair", ok, "rc=%d fds=%d,%d errno=%d", r, sv[0], sv[1], errno);
  if (!ok) goto finish;

  char buf[64];
  memset(buf, 0, sizeof buf);
  ssize_t w1 = write(sv[0], "ping", 4);
  ssize_t r1 = read(sv[1], buf, sizeof buf);
  int ab = (w1 == 4 && r1 == 4 && memcmp(buf, "ping", 4) == 0);
  memset(buf, 0, sizeof buf);
  ssize_t w2 = write(sv[1], "pong!", 5);
  ssize_t r2 = read(sv[0], buf, sizeof buf);
  int ba = (w2 == 5 && r2 == 5 && memcmp(buf, "pong!", 5) == 0);
  verdict("pair_rw", ab && ba, "a->b=%d/%d b->a=%d/%d", (int)w1, (int)r1,
          (int)w2, (int)r2);

  /* ---- 5a. FIONREAD: queued bytes on a socket ------------------------ */
  (void)!write(sv[0], "xyz", 3);
  int avail = -1;
  errno = 0;
  int ir = ioctl(sv[1], FIONREAD, &avail);
  verdict("fionread", ir == 0 && avail == 3, "rc=%d n=%d errno=%d", ir, avail,
          errno);
  (void)!read(sv[1], buf, sizeof buf); /* drain */

  /* ---- 3a. poll: readable with data ---------------------------------- */
  (void)!write(sv[0], "q", 1);
  struct pollfd pf;
  pf.fd = sv[1];
  pf.events = POLLIN;
  pf.revents = 0;
  errno = 0;
  r = poll(&pf, 1, 0);
  verdict("poll_ready", r == 1 && (pf.revents & POLLIN) != 0,
          "rc=%d revents=0x%x errno=%d", r, (unsigned)pf.revents, errno);
  (void)!read(sv[1], buf, sizeof buf); /* drain */

  /* ---- 3b. poll: timeout accounting ---------------------------------- */
  long long t0 = now_ms();
  pf.revents = 0;
  errno = 0;
  r = poll(&pf, 1, 120);
  long long dt = now_ms() - t0;
  verdict("poll_timeout", r == 0 && dt >= 100 && dt < 5000,
          "rc=%d elapsed_ms=%lld errno=%d", r, dt, errno);

  /* ---- 4a. select: readable with data -------------------------------- */
  (void)!write(sv[0], "s", 1);
  fd_set rs;
  FD_ZERO(&rs);
  FD_SET(sv[1], &rs);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  errno = 0;
  r = select(sv[1] + 1, &rs, NULL, NULL, &tv);
  verdict("select_ready", r == 1 && FD_ISSET(sv[1], &rs), "rc=%d errno=%d", r,
          errno);
  (void)!read(sv[1], buf, sizeof buf); /* drain */

  /* ---- 4b. select: timeout ------------------------------------------- */
  FD_ZERO(&rs);
  FD_SET(sv[1], &rs);
  tv.tv_sec = 0;
  tv.tv_usec = 60000;
  t0 = now_ms();
  errno = 0;
  r = select(sv[1] + 1, &rs, NULL, NULL, &tv);
  dt = now_ms() - t0;
  verdict("select_timeout", r == 0 && !FD_ISSET(sv[1], &rs) && dt >= 40,
          "rc=%d elapsed_ms=%lld errno=%d", r, dt, errno);

  /* ---- 5b. fcntl F_GETFL / F_SETFL O_NONBLOCK ------------------------ */
  errno = 0;
  int fl0 = fcntl(sv[0], F_GETFL);
  int sr = fcntl(sv[0], F_SETFL, fl0 | O_NONBLOCK);
  int fl1 = fcntl(sv[0], F_GETFL);
  int cr = fcntl(sv[0], F_SETFL, fl0 & ~O_NONBLOCK);
  int fl2 = fcntl(sv[0], F_GETFL);
  verdict("fcntl_nonblock",
          fl0 >= 0 && sr == 0 && (fl1 & O_NONBLOCK) && cr == 0 &&
              !(fl2 & O_NONBLOCK),
          "getfl=0x%x setfl=%d fl_nb=0x%x clr=%d fl_blk=0x%x", fl0, sr, fl1, cr,
          fl2);

  /* A non-blocking read on an empty socket must say EAGAIN, not block. */
  (void)fcntl(sv[0], F_SETFL, fl0 | O_NONBLOCK);
  errno = 0;
  r = (int)read(sv[0], buf, sizeof buf);
  int eagain = (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
  verdict("nonblock_eagain", eagain, "rc=%d errno=%d", r, errno);
  (void)fcntl(sv[0], F_SETFL, fl0 & ~O_NONBLOCK);

  /* ---- 5c. ioctl FIONBIO -------------------------------------------- */
  int on = 1;
  errno = 0;
  int br = ioctl(sv[0], FIONBIO, &on);
  int fl3 = fcntl(sv[0], F_GETFL);
  on = 0;
  int br2 = ioctl(sv[0], FIONBIO, &on);
  int fl4 = fcntl(sv[0], F_GETFL);
  verdict("ioctl_fionbio",
          br == 0 && br2 == 0 && (fl3 & O_NONBLOCK) && !(fl4 & O_NONBLOCK),
          "set=%d fl_nb=0x%x clr=%d fl_blk=0x%x errno=%d", br, fl3, br2, fl4,
          errno);

  /* ---- 6a. dup / dup2 / dup3 ---------------------------------------- */
  errno = 0;
  int d1 = dup(sv[0]);
  int dup_ok = (d1 >= 3 && d1 != sv[0] && d1 != sv[1]);
  /* the duplicate is the SAME open file: bytes written through it arrive */
  ssize_t dw = dup_ok ? write(d1, "D", 1) : -1;
  ssize_t dr = dup_ok ? read(sv[1], buf, sizeof buf) : -1;
  verdict("dup", dup_ok && dw == 1 && dr == 1 && buf[0] == 'D',
          "fd=%d wr=%d rd=%d errno=%d", d1, (int)dw, (int)dr, errno);

  int target = 20;
  errno = 0;
  int d2 = dup2(d1, target);
  int same = (d2 == target) && (fcntl(target, F_GETFD) >= 0);
  verdict("dup2", same, "rc=%d want=%d errno=%d", d2, target, errno);

  errno = 0;
  int d3 = dup3(target, 21, O_CLOEXEC);
  int fdfl = fcntl(21, F_GETFD);
  int d3s = dup3(target, target, 0); /* oldfd == newfd: EINVAL in dup3 */
  int d3e = errno;
  verdict("dup3", d3 == 21 && fdfl >= 0 && (fdfl & FD_CLOEXEC) && d3s == -1 &&
                      d3e == EINVAL,
          "rc=%d fdflags=0x%x same=%d errno=%d", d3, fdfl, d3s, d3e);
  if (d3 == 21) close(21);
  if (d2 == target) close(target);
  if (d1 >= 3) close(d1);

  /* ---- 6b. pipe2 flags + traffic ------------------------------------ */
  int pp[2] = {-1, -1};
  errno = 0;
  r = pipe2(pp, O_NONBLOCK | O_CLOEXEC);
  int pfl = (r == 0) ? fcntl(pp[0], F_GETFL) : -1;
  int pfd = (r == 0) ? fcntl(pp[0], F_GETFD) : -1;
  /* Write before reading: the guest's pipe read path is blocking whatever
   * O_NONBLOCK says, so only the flag itself and real traffic are asserted. */
  ssize_t pw = (r == 0) ? write(pp[1], "pipe", 4) : -1;
  ssize_t pr = (r == 0) ? read(pp[0], buf, sizeof buf) : -1;
  verdict("pipe2", r == 0 && (pfl & O_NONBLOCK) && (pfd & FD_CLOEXEC) &&
                       pw == 4 && pr == 4 && memcmp(buf, "pipe", 4) == 0,
          "rc=%d getfl=0x%x getfd=0x%x wr=%d rd=%d errno=%d", r, pfl, pfd,
          (int)pw, (int)pr, errno);
  if (r == 0) {
    close(pp[0]);
    close(pp[1]);
  }

  /* ---- 6c. close_range ---------------------------------------------- */
  /* Use dup2 into a known band so the range is deterministic on both. */
  int c1 = dup2(sv[0], 40);
  int c2 = dup2(sv[0], 41);
  errno = 0;
  r = (int)syscall(SYS_close_range, 40u, 41u, 0u);
  int cr_errno = errno;
  /* F_GETFL, not F_GETFD: HobbyOS answers F_GETFD from a bitmap that keeps
   * no record of a closed slot, so only F_GETFL reveals the close. */
  errno = 0;
  int f40 = fcntl(40, F_GETFL);
  int e40 = errno;
  errno = 0;
  int f41 = fcntl(41, F_GETFL);
  int e41 = errno;
  int closed_ok = (r == 0) && (f40 == -1) && (f41 == -1);
  verdict("close_range", closed_ok,
          "rc=%d c1=%d c2=%d f40=%d/e%d f41=%d/e%d errno=%d", r, c1, c2, f40,
          e40, f41, e41, cr_errno);

  /* ---- 7. the named socket: bind / listen / connect / accept --------- */
  (void)unlink(LP_PATH); /* Linux needs this; HobbyOS has no socket inode */
  int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  errno = 0;
  if (ls >= 0) {
    int c0 = socket(AF_UNIX, SOCK_STREAM, 0);
    /* (2) connect to a name that does not exist: must fail, must not hang */
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/tmp/lpfd-absent.sock");
    (void)unlink(sa.sun_path);
    errno = 0;
    int mr = connect(c0, (struct sockaddr *)&sa, sizeof sa);
    int me = errno;
    verdict("connect_missing",
            mr == -1 && (me == ENOENT || me == ECONNREFUSED),
            "rc=%d errno=%d", mr, me);
    close(c0);
  } else {
    verdict("connect_missing", 0, "socket rc=%d errno=%d", ls, errno);
  }

  memset(&sa, 0, sizeof sa); /* reuse: bind name */
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, LP_PATH);
  errno = 0;
  int bindr = bind(ls, (struct sockaddr *)&sa, sizeof sa);
  int listenr = (bindr == 0) ? listen(ls, 4) : -1;
  verdict("bind_listen", bindr == 0 && listenr == 0, "bind=%d listen=%d errno=%d",
          bindr, listenr, errno);
  if (bindr != 0 || listenr != 0) {
    if (ls >= 0) close(ls);
    goto finish;
  }

  /* getsockname on the listener: the bound path, family AF_UNIX. */
  struct sockaddr_un gsa;
  socklen_t gsl = sizeof gsa;
  memset(&gsa, 0, sizeof gsa);
  errno = 0;
  int gsr = getsockname(ls, (struct sockaddr *)&gsa, &gsl);
  verdict("getsockname", gsr == 0 && gsa.sun_family == AF_UNIX && gsl > 2 &&
                             strcmp(gsa.sun_path, LP_PATH) == 0,
          "rc=%d len=%u fam=%d path=%s errno=%d", gsr, (unsigned)gsl,
          (int)gsa.sun_family, gsa.sun_path, errno);

  /* A pending connection makes the listener readable. */
  int cl = socket(AF_UNIX, SOCK_STREAM, 0);
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, LP_PATH);
  errno = 0;
  int crc = connect(cl, (struct sockaddr *)&sa, sizeof sa);
  verdict("connect_named", crc == 0, "rc=%d errno=%d", crc, errno);
  if (crc != 0) {
    close(cl);
    close(ls);
    goto finish;
  }

  struct pollfd lpf;
  lpf.fd = ls;
  lpf.events = POLLIN;
  lpf.revents = 0;
  r = poll(&lpf, 1, 0);
  verdict("listen_pollin", r == 1 && (lpf.revents & POLLIN) != 0,
          "rc=%d revents=0x%x", r, (unsigned)lpf.revents);

  struct sockaddr_un psa;
  socklen_t psl = sizeof psa;
  memset(&psa, 0, sizeof psa);
  errno = 0;
  int ar = accept(ls, (struct sockaddr *)&psa, &psl);
  verdict("accept", ar >= 3, "fd=%d len=%u fam=%d errno=%d", ar, (unsigned)psl,
          (int)psa.sun_family, errno);
  if (ar < 3) {
    close(cl);
    close(ls);
    goto finish;
  }

  /* getpeername on BOTH ends of the accepted pair. */
  struct sockaddr_un nsa;
  socklen_t nsl = sizeof nsa;
  memset(&nsa, 0, sizeof nsa);
  errno = 0;
  int pnr = getpeername(cl, (struct sockaddr *)&nsa, &nsl);
  int peer_path_ok = (pnr == 0) && nsa.sun_family == AF_UNIX &&
                     (nsl > 2) && strcmp(nsa.sun_path, LP_PATH) == 0;
  memset(&nsa, 0, sizeof nsa);
  nsl = sizeof nsa;
  int snr = getsockname(ar, (struct sockaddr *)&nsa, &nsl);
  int srv_name_ok = (snr == 0) && nsa.sun_family == AF_UNIX && nsl > 2 &&
                    strcmp(nsa.sun_path, LP_PATH) == 0;
  verdict("peer_names", peer_path_ok && srv_name_ok,
          "peer(rc=%d len=%u path=%s) self(rc=%d len=%u path=%s)", pnr,
          (unsigned)(peer_path_ok ? nsl : 0), nsa.sun_path, snr, (unsigned)nsl,
          nsa.sun_path);

  /* ---- 6d. getsockopt / setsockopt ---------------------------------- */
  int soerr = -1;
  socklen_t sel = sizeof soerr;
  errno = 0;
  int g1 = getsockopt(cl, SOL_SOCKET, SO_ERROR, &soerr, &sel);
  int sotype = -1;
  socklen_t stl = sizeof sotype;
  int g2 = getsockopt(cl, SOL_SOCKET, SO_TYPE, &sotype, &stl);
  int sndbuf = -1;
  socklen_t sbl = sizeof sndbuf;
  int g3 = getsockopt(cl, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sbl);
  int want = 4096;
  int s1 = setsockopt(cl, SOL_SOCKET, SO_SNDBUF, &want, sizeof want);
  int rcvbuf = -1;
  socklen_t rbl = sizeof rcvbuf;
  int s2 = setsockopt(cl, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
  int g4 = getsockopt(cl, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rbl);
  verdict("sockopts",
          g1 == 0 && soerr == 0 && g2 == 0 && sotype == SOCK_STREAM &&
              g3 == 0 && sndbuf > 0 && s1 == 0 && s2 == 0 && g4 == 0 &&
              rcvbuf > 0,
          "so_error=%d type=%d sndbuf=%d rcvbuf=%d set=%d/%d errno=%d", soerr,
          sotype, sndbuf, rcvbuf, s1, s2, errno);

  /* ---- 7b. sendto / recvfrom on the connected pair ------------------- */
  memset(buf, 0, sizeof buf);
  errno = 0;
  ssize_t sto = sendto(cl, "x11", 3, 0, NULL, 0);
  struct sockaddr_un fsa;
  socklen_t fsl = sizeof fsa;
  memset(&fsa, 0, sizeof fsa);
  ssize_t rfr = recvfrom(ar, buf, sizeof buf, 0, (struct sockaddr *)&fsa, &fsl);
  /* Linux leaves the source length at 0 for a connected stream socket whose
   * peer never bound a name (measured on this host); a kernel that fills the
   * peer in is equally correct, so only the DATA is asserted and the
   * address length is printed for the differential. */
  int rfr_addr_ok = (fsl == 0) || (fsa.sun_family == AF_UNIX);
  verdict("sendto_recvfrom",
          sto == 3 && rfr == 3 && memcmp(buf, "x11", 3) == 0 && rfr_addr_ok,
          "send=%d recv=%d data=%c%c%c srclen=%u errno=%d", (int)sto, (int)rfr,
          buf[0], buf[1], buf[2], (unsigned)fsl, errno);

  /* ---- 3c. poll on the accepted end with data ----------------------- */
  (void)!write(cl, "M", 1);
  pf.fd = ar;
  pf.events = POLLIN;
  pf.revents = 0;
  r = poll(&pf, 1, 0);
  verdict("poll_accepted", r == 1 && (pf.revents & POLLIN) != 0,
          "rc=%d revents=0x%x", r, (unsigned)pf.revents);
  (void)!read(ar, buf, sizeof buf);

  /* ---- 7d. sendmsg / recvmsg: scattered iovecs ----------------------- */
  memset(buf, 0, sizeof buf);
  {
    char part1[3] = "ms";
    char part2[3] = "g!";
    struct iovec siv[2];
    siv[0].iov_base = part1;
    siv[0].iov_len = 2;
    siv[1].iov_base = part2;
    siv[1].iov_len = 2;
    struct msghdr smh;
    memset(&smh, 0, sizeof smh);
    smh.msg_iov = siv;
    smh.msg_iovlen = 2;
    errno = 0;
    ssize_t sr = sendmsg(sv[0], &smh, 0);

    char rbuf[16];
    struct iovec riv[1];
    riv[0].iov_base = rbuf;
    riv[0].iov_len = sizeof rbuf;
    struct msghdr rmh;
    memset(&rmh, 0, sizeof rmh);
    rmh.msg_iov = riv;
    rmh.msg_iovlen = 1;
    errno = 0;
    ssize_t rr = recvmsg(sv[1], &rmh, 0);
    verdict("sendmsg_recvmsg",
            sr == 4 && rr == 4 && memcmp(rbuf, "msg!", 4) == 0,
            "send=%d recv=%d data=%.4s flags=0x%x errno=%d", (int)sr, (int)rr,
            rbuf, (unsigned)rmh.msg_flags, errno);
  }

  /* ---- 7e. accept4 with SOCK_NONBLOCK | SOCK_CLOEXEC ----------------- */
  {
    int c2s = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, LP_PATH);
    int cc = (c2s >= 0) ? connect(c2s, (struct sockaddr *)&sa, sizeof sa) : -1;
    errno = 0;
    int a4 = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int afl = (a4 >= 0) ? fcntl(a4, F_GETFL) : -1;
    int afd = (a4 >= 0) ? fcntl(a4, F_GETFD) : -1;
    verdict("accept4",
            cc == 0 && a4 >= 3 && (afl & O_NONBLOCK) && (afd & FD_CLOEXEC),
            "connect=%d fd=%d getfl=0x%x getfd=0x%x errno=%d", cc, a4, afl, afd,
            errno);
    if (a4 >= 3) close(a4);
    if (c2s >= 0) close(c2s);
  }

  /* ---- 7c. shutdown(SHUT_WR) -> EOF on the peer --------------------- */
  errno = 0;
  int shr = shutdown(cl, SHUT_WR);
  ssize_t eofr = read(ar, buf, sizeof buf);
  verdict("shutdown_eof", shr == 0 && eofr == 0, "shutdown=%d read=%d errno=%d",
          shr, (int)eofr, errno);

  close(ar);
  close(cl);
  close(ls);
  (void)unlink(LP_PATH);

  /* ---- 3d. POLLHUP when the peer goes away -------------------------- */
  (void)close(sv[0]);
  sv[0] = -1;
  pf.fd = sv[1];
  pf.events = POLLIN;
  pf.revents = 0;
  r = poll(&pf, 1, 200);
  verdict("poll_hup", r == 1 && (pf.revents & POLLHUP) != 0, "rc=%d revents=0x%x",
          r, (unsigned)pf.revents);

finish:
  if (sv[0] >= 0) close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  row("[LP] done rc=%d\n", fails);
  return fails;
}
