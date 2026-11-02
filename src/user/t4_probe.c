/* src/user/t4_probe.c — GB-066 T4 device-leg probe (W0 row set).
 *
 * ONE source, two hosts, through the REAL syscall path on both:
 *
 *   leg A  reference : x86_64-linux-musl static ELF, run on real Linux.
 *   leg B  HobbyOS   : the SAME musl-built static ELF, staged onto disk.img,
 *                      loaded by HobbyOS's ELF loader (GB-013) and run inside
 *                      the guest.  PROC_FLAG_LINUX_ABI routes its write()
 *                      traps through the Linux-compat dispatch (GB-014) to the
 *                      console/serial.  This tests the kernel, never a host
 *                      shim.
 *
 * Output contract (byte-identical on both legs; the diff is on stdout):
 *   t4-probe
 *   row=<name> rc=<n> errno=<n> out=<hex>
 *   t4-probe-arrived     <- sentinel printed by the probe itself: existence
 *                           of this line PROVES the probe ran in the guest
 *                           (an image listing is never the proof).
 *   probe-exit
 *
 * Standard C + write() only, so the same object builds with musl-gcc -static
 * for both legs.  A tiny itoa avoids relying on printf differences.
 */
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/random.h>
#include <poll.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/socket.h> /* GB-018 socket remainder: socketpair/AF_UNIX */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>   /* GB-021: musl pthread_create/join through clone/futex */
#include <time.h>

static void pstr(const char *s) {
  size_t n = 0; while (s[n]) n++;
  (void)write(1, s, n);
}

static int itoa_dec(unsigned long v, char *dst) {
  char tmp[24]; int n = 0;
  if (v == 0) { dst[0] = '0'; return 1; }
  while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
  return n;
}

static void prow(const char *name, long rc, long err, const unsigned char *out, int outlen) {
  static const char hex[] = "0123456789abcdef";
  char buf[64]; int n = 0;
  /* "row=<name> rc=<rc> errno=<err> out=<hex>\n" */
  const char *p = "row="; while (*p) buf[n++] = *p++;
  p = name; while (*p) buf[n++] = *p++;
  p = " rc="; while (*p) buf[n++] = *p++;
  if (rc < 0) buf[n++] = '-';
  n += itoa_dec((unsigned long)(rc >= 0 ? rc : -rc), buf + n);
  p = " errno="; while (*p) buf[n++] = *p++;
  n += itoa_dec((unsigned long)(err >= 0 ? err : -err), buf + n);
  if (outlen > 0) {
    p = " out="; while (*p) buf[n++] = *p++;
    for (int i = 0; i < outlen; i++) {
      buf[n++] = hex[(out[i] >> 4) & 0xf];
      buf[n++] = hex[out[i] & 0xf];
    }
  }
  buf[n++] = '\n';
  (void)write(1, buf, (size_t)n);
}

/* GB-021: thread/futex workers.  Same row names and deterministic values as
 * the host model (src/host/t4_compat_probe.c), so one golden name set covers
 * both probes.  Per-thread storage goes through pthread keys rather than a C
 * __thread variable: the musl static-PIE loader here does not yet map a
 * PT_TLS segment (it faults inside musl static_init_tls), while the pthread
 * TCB path is exactly what clone(2)/SETTLS has to get right anyway. */
static pthread_key_t t4_key;
static pthread_once_t t4_key_once = PTHREAD_ONCE_INIT;

static void t4_key_make(void) { pthread_key_create(&t4_key, NULL); }

static void *t4_thr_tls(void *p) {
  long v = (long)p;
  pthread_once(&t4_key_once, t4_key_make);
  pthread_setspecific(t4_key, (void *)v);
  errno = (int)(v + 1000);   /* per-thread errno store */
  return (void *)(long)pthread_getspecific(t4_key);
}

static volatile int t4_counter;

static void *t4_thr_worker(void *p) {
  long n = (long)p;
  for (long i = 0; i < n; i++)
    __sync_fetch_and_add(&t4_counter, 1);
  return (void *)(long)(2 * n);
}

static volatile int t4_futex_val;

static void *t4_thr_futex(void *p) {
  volatile int *v = (volatile int *)p;
  struct timespec ts = {2, 0};
  while (__atomic_load_n(v, __ATOMIC_RELAXED) == 0) {
    long r = syscall(SYS_futex, (int *)v, 0 /* FUTEX_WAIT */, 0, &ts, NULL, 0);
    if (r < 0 && errno != EAGAIN) break;
  }
  return (void *)(long)(__atomic_load_n(v, __ATOMIC_RELAXED) ? 0 : 1);
}


/* GB-111: a deterministic SKIP row for a whole block we could not run because the
 * primitives (socketpair/pipe2) failed — WITHOUT touching the uninitialised fds.
 * The reason is explicit so the row is never a silent omission.  Same bytes on
 * both legs when both fail (byte-identical), and only on the failing leg when one
 * of them succeeds (that asymmetry is the real differential, not a hang/UB). */
static void prow_skip(const char *name, const char *why) {
  char buf[80]; int n = 0; const char *p = "row=";
  while (*p) buf[n++] = *p++;
  p = name; while (*p) buf[n++] = *p++;
  p = " rc=-1 errno=9 SKIP reason="; while (*p) buf[n++] = *p++;
  p = why; while (*p) buf[n++] = *p++;
  buf[n++] = '\n';
  (void)write(1, buf, (size_t)n);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  pstr("t4-probe\n");

  /* Row 1: write(1, "hi\n", 3) -> rc=3, errno=0, out=68690a. */
  {
    static const unsigned char msg[] = "hi\n";
    long r = write(1, msg, 3);
    prow("write3", r, (r < 0) ? (r == -1 ? 9 : (long)(-r)) : 0, msg, 3);
  }

  /* GB-073: emit every row the M1 -V DoDs name.  All but write3 were the
   * defect's ground: uname/urandom/null/getrandom rows existed ONLY in the
   * host model (t4_compat_probe.c), never in the guest probe — so GB-016-V /
   * GB-017-V etc. were unmeasurable by construction.  Leg A (Linux) runs all
   * of these through the real syscalls; leg B (HobbyOS-intel) does the same
   * once the GB-014 dispatch lands.  rc/errno are the deterministic values
   * (payloads deliberately not compared for the random rows). */
  {
    struct utsname u;
    long urc = uname(&u);
    prow("uname_rc", urc, urc < 0 ? (long)(-urc) : 0, NULL, 0);
    prow("uname_sysname_linux", strcmp(u.sysname, "Linux") == 0, 0, NULL, 0);
    prow("uname_machine_x86_64", strcmp(u.machine, "x86_64") == 0, 0, NULL, 0);
  }
  {
    unsigned char rnd[16];
    long gr = getrandom(rnd, sizeof rnd, 0);
    prow("getrandom_16", gr, gr < 0 ? (long)(-gr) : 0, NULL, 0);
    int ur = open("/dev/urandom", O_RDONLY);
    prow("urandom_open", ur, ur < 0 ? 2 : 0, NULL, 0);
    long urb = read(ur, rnd, sizeof rnd);
    prow("urandom_read16", urb, urb < 0 ? 2 : 0, NULL, 0);
    int nw = open("/dev/null", O_WRONLY);
    prow("null_open_w", nw, nw < 0 ? 2 : 0, NULL, 0);
    long nwb = write(nw, "abc", 3);
    prow("null_write3", nwb, nwb < 0 ? 2 : 0, NULL, 0);
    int nr = open("/dev/null", O_RDONLY);
    prow("null_open_r", nr, nr < 0 ? 2 : 0, NULL, 0);
    char c; long nrb = read(nr, &c, 1);
    prow("null_read1", nrb, nrb < 0 ? 2 : 0, NULL, 0);
  }

  /* GB-018: poll/select/pselect6 under compat — socket+pipe cases.  The pipe
   * write makes the read end ready; poll/select/pselect must report it (count=1,
   * revents=POLLIN, fdset bit set).  A shim that never marks a readable pipe
   * ready, or that reports readiness without the right event mask, diverges.
   * poll_badfd: an invalid fd must report POLLNVAL (not hang, not count 0).
   * All deterministic on both legs (fresh pipe, timeout 0). */
  {
    int pfd[2];
    long prc = pipe(pfd);
    prow("pipe2", prc, prc < 0 ? 9 : 0, NULL, 0);
    if (prc == 0) {
      if (write(pfd[1], "x", 1) != 1) prow("pipe_write", -1, 9, NULL, 0);

      struct pollfd pf[1];
      pf[0].fd = pfd[0]; pf[0].events = POLLIN; pf[0].revents = 0;
      long pr = poll(pf, 1, 0);
      prow("poll_ready", pr, pr < 0 ? 9 : 0, NULL, 0);
      long pre = pf[0].revents;
      prow("poll_ready_revents", pre, pre < 0 ? 9 : 0, NULL, 0);

      fd_set rf; FD_ZERO(&rf); FD_SET(pfd[0], &rf);
      struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 0;
      long sr = select(pfd[0] + 1, &rf, NULL, NULL, &tv);
      prow("select_ready", sr, sr < 0 ? 9 : 0, NULL, 0);
      prow("select_ready_fdset", FD_ISSET(pfd[0], &rf) ? 1 : 0, 0, NULL, 0);

      sigset_t empt; sigemptyset(&empt);
      fd_set rf2; FD_ZERO(&rf2); FD_SET(pfd[0], &rf2);
      struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 0;
      long ps6 = pselect(pfd[0] + 1, &rf2, NULL, NULL, &ts, &empt);
      prow("pselect6_ready", ps6, ps6 < 0 ? 9 : 0, NULL, 0);
      prow("pselect6_ready_fdset", FD_ISSET(pfd[0], &rf2) ? 1 : 0, 0, NULL, 0);

      close(pfd[0]); close(pfd[1]);
    } else {
      /* GB-111: report the readiness block as SKIP with a reason instead of
       * running write/poll/select/pselect on the UNINITIALISED pfd[] fds. */
      prow_skip("pipe_write", "no-pipe");
      prow_skip("poll_ready", "no-pipe");
      prow_skip("poll_ready_revents", "no-pipe");
      prow_skip("select_ready", "no-pipe");
      prow_skip("select_ready_fdset", "no-pipe");
      prow_skip("pselect6_ready", "no-pipe");
      prow_skip("pselect6_ready_fdset", "no-pipe");
    }

    struct pollfd pf2[1];
    pf2[0].fd = 9999999; pf2[0].events = POLLIN; pf2[0].revents = 0;
    long pbr = poll(pf2, 1, 0);
    prow("poll_badfd", pbr, pbr < 0 ? 9 : 0, NULL, 0);
    prow("poll_badfd_revents", pf2[0].revents, 0, NULL, 0);
  }

  /* GB-018 (socket remainder, coord 2026-10-20): stream-socket readiness is a
   * DISTINCT wake path from the pipe above (socket/ring buffer, not the pipe
   * FIFO) — the one the compat layer is most likely to get wrong.  A
   * socketpair(AF_UNIX, SOCK_STREAM) gives two connected stream sockets; the
   * peer write flips the other end readable.  poll/select/pselect6 must report
   * it.  Deterministic on both legs (kernel: SYS_SOCKETPAIR). */
  {
    int sv[2];
    long spc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    prow("sockpair", spc, spc < 0 ? 9 : 0, NULL, 0);
    if (spc == 0) {
      if (write(sv[1], "s", 1) != 1) prow("sock_write", -1, 9, NULL, 0);

      struct pollfd spf[1];
      spf[0].fd = sv[0]; spf[0].events = POLLIN; spf[0].revents = 0;
      long spr = poll(spf, 1, 0);
      prow("sockpoll_ready", spr, spr < 0 ? 9 : 0, NULL, 0);
      prow("sockpoll_revents", spf[0].revents, 0, NULL, 0);

      fd_set srf; FD_ZERO(&srf); FD_SET(sv[0], &srf);
      struct timeval stv; stv.tv_sec = 0; stv.tv_usec = 0;
      long ssr = select(sv[0] + 1, &srf, NULL, NULL, &stv);
      prow("sockselect_ready", ssr, ssr < 0 ? 9 : 0, NULL, 0);
      prow("sockselect_fdset", FD_ISSET(sv[0], &srf) ? 1 : 0, 0, NULL, 0);

      sigset_t sempt; sigemptyset(&sempt);
      fd_set srf2; FD_ZERO(&srf2); FD_SET(sv[0], &srf2);
      struct timespec sts; sts.tv_sec = 0; sts.tv_nsec = 0;
      long sps = pselect(sv[0] + 1, &srf2, NULL, NULL, &sts, &sempt);
      prow("sockpselect_ready", sps, sps < 0 ? 9 : 0, NULL, 0);
      prow("sockpselect_fdset", FD_ISSET(sv[0], &srf2) ? 1 : 0, 0, NULL, 0);

      close(sv[0]); close(sv[1]);
    } else {
      /* GB-111: report the whole readiness block as SKIP with a reason instead of
       * running write/poll/select/pselect on the UNINITIALISED sv[] fds. */
      prow_skip("sock_write", "no-socketpair");
      prow_skip("sockpoll_ready", "no-socketpair");
      prow_skip("sockpoll_revents", "no-socketpair");
      prow_skip("sockselect_ready", "no-socketpair");
      prow_skip("sockselect_fdset", "no-socketpair");
      prow_skip("sockpselect_ready", "no-socketpair");
      prow_skip("sockpselect_fdset", "no-socketpair");
    }
  }

  /* GB-076: fcntl/ioctl/dent rows the GB-019/020/021 -V DoDs name — must be in
   * the shared golden + emitted by the guest probe so t4_device --check can
   * fail on them (vacuous-pass closure).  Mirrors the host model: F_GETFL
   * before/after F_SETFL(O_APPEND) + O_APPEND bit; TIOCGWINSZ/TCGETS on a
   * non-tty (/dev/null) -> ENOTTY (25); getdents64 over a fixed 3-entry dir. */
  {
    int f = open("/tmp/ho_t4_ioctl_device", O_RDWR | O_CREAT, 0600);
    long fgr = fcntl(f, F_GETFL);
    prow("fcntl_getfl_rdwr", fgr, fgr < 0 ? 9 : 0, NULL, 0);
    long fsa = fcntl(f, F_SETFL, O_APPEND);
    prow("fcntl_setfl_append", fsa, fsa < 0 ? 9 : 0, NULL, 0);
    long fga = fcntl(f, F_GETFL);
    prow("fcntl_getfl_after", fga, fga < 0 ? 9 : 0, NULL, 0);
    prow("fcntl_append_bit", (fga & O_APPEND) ? 1 : 0, 0, NULL, 0);
    close(f);
    int nt = open("/dev/null", O_RDWR);
    struct winsize ws; memset(&ws, 0, sizeof ws);
    long iw = ioctl(nt, TIOCGWINSZ, &ws);
    prow("ioctl_tiocgwinsz_entty", iw < 0 ? (long)(errno) : 0, 0, NULL, 0);
    struct termios ts; memset(&ts, 0, sizeof ts);
    long it = ioctl(nt, TCGETS, &ts);
    prow("ioctl_tcgets_entty", it < 0 ? (long)(errno) : 0, 0, NULL, 0);
    close(nt);

    const char *d = "/tmp/ho_t4_dent";
    mkdir(d, 0700);
    int created = 0;
    char p[128];
    snprintf(p, sizeof p, "%s/alpha", d);   FILE *fa = fopen(p, "w"); if (fa) { fputs("a", fa); fclose(fa); created++; }
    snprintf(p, sizeof p, "%s/beta", d);    FILE *fb = fopen(p, "w"); if (fb) { fputs("b", fb); fclose(fb); created++; }
    snprintf(p, sizeof p, "%s/charlie", d); FILE *fc = fopen(p, "w"); if (fc) { fputs("c", fc); fclose(fc); created++; }
    prow("dent_created", created, 0, NULL, 0);
    int dfd = open(d, O_RDONLY | O_DIRECTORY);
    prow("dent_opendir", dfd, dfd < 0 ? 9 : 0, NULL, 0);
    if (dfd >= 0) {
      char buf[8192];
      int saw_alpha = 0, saw_beta = 0, saw_charlie = 0, total = 0;
      for (;;) {
        long n = syscall(SYS_getdents64, dfd, buf, sizeof buf);
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
          struct linux_dirent64 { unsigned long d_ino; long d_off; unsigned short d_reclen; unsigned char d_type; char d_name[]; } *e =
            (struct linux_dirent64 *)(void *)(buf + off);
          char *nm = e->d_name;
          if (strcmp(nm, ".") && strcmp(nm, "..")) {
            total++;
            if (!strcmp(nm, "alpha"))   saw_alpha = 1;
            else if (!strcmp(nm, "beta"))  saw_beta = 1;
            else if (!strcmp(nm, "charlie")) saw_charlie = 1;
          }
          off += e->d_reclen;
        }
      }
      prow("dent_total", total, 0, NULL, 0);
      prow("dent_have_alpha", saw_alpha, 0, NULL, 0);
      prow("dent_have_beta", saw_beta, 0, NULL, 0);
      prow("dent_have_charlie", saw_charlie, 0, NULL, 0);
      close(dfd);
    }
  }

  /* GB-020: /proc/self/fd — a virtual directory of this process's own fds.
     Deterministic across legs: opendir(2) must succeed (dfd >= 0) on both
     real Linux and HobbyOS.  The fd contents differ per-leg (process state),
     so only the open-and-isdir contract is asserted, not the fd list. */
  {
    int fd_p = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
    prow("procfd_opendir", fd_p >= 0 ? 0 : -1, fd_p < 0 ? (long)(errno) : 0, NULL, 0);
    /* getdents64 over it must yield at least "." and fd entries -> bytes > 0 */
    long selfn = 0;
    if (fd_p >= 0) {
      char sb[8192];
      long nn = syscall(SYS_getdents64, fd_p, sb, sizeof sb);
      selfn = nn;
      close(fd_p);
    }
    prow("procfd_read", selfn > 0 ? 1 : 0, selfn == 0 ? 9 : 0, NULL, 0);
  }

  /* GB-021: threads under the Linux ABI.  musl's pthread_create reaches the
     kernel as clone(56), join as futex(202)/CLONE_CHILD_CLEARTID.  Values are
     deterministic: 4 threads x 1000 increments, exit value 2*1000 each. */
  {
    pthread_t a, b;
    void *ra = NULL, *rb = NULL;
    errno = 0;
    int ca = pthread_create(&a, NULL, t4_thr_tls, (void *)101L);
    prow("tls_create_a", ca, ca ? ca : 0, NULL, 0);
    int cb = pthread_create(&b, NULL, t4_thr_tls, (void *)202L);
    prow("tls_create_b", cb, cb ? cb : 0, NULL, 0);
    int ja = pthread_join(a, &ra);
    int jb = pthread_join(b, &rb);
    prow("tls_join_a", ja, ja ? ja : 0, NULL, 0);
    prow("tls_join_b", jb, jb ? jb : 0, NULL, 0);
    prow("tls_thread_a_seed", (long)ra, 0, NULL, 0);  /* must be 101 */
    prow("tls_thread_b_seed", (long)rb, 0, NULL, 0);  /* must be 202 */
    prow("tls_main_errno_untouched", errno, 0, NULL, 0);

    pthread_t t[4];
    t4_counter = 0;
    for (long i = 0; i < 4; i++) {
      errno = 0;
      int c = pthread_create(&t[i], NULL, t4_thr_worker, (void *)1000L);
      prow("thread_create", c, c ? c : 0, NULL, 0);
    }
    long sum = 0;
    for (int i = 0; i < 4; i++) {
      void *ret = NULL;
      pthread_join(t[i], &ret);
      sum += (long)ret;
    }
    prow("thread_join_sum", sum, 0, NULL, 0);    /* 4 * 2000 = 8000 */
    prow("thread_counter", t4_counter, 0, NULL, 0); /* 4000 */

    /* futex handshake: a waiter parks on t4_futex_val==0; main stores 1 and
       wakes it.  woken==0 on success (1 would be the timeout path).  The
       WAKE return itself is racy by construction, so report only >= 0. */
    t4_futex_val = 0;
    pthread_t w;
    pthread_create(&w, NULL, t4_thr_futex, (void *)&t4_futex_val);
    usleep(20000); /* give the waiter a chance to park; the row is safe either way */
    __atomic_store_n(&t4_futex_val, 1, __ATOMIC_RELEASE);
    long wk = syscall(SYS_futex, (int *)&t4_futex_val, 1 /* FUTEX_WAKE */, 1,
                      NULL, NULL, 0);
    prow("futex_wake_ok", wk >= 0 ? 1 : 0, 0, NULL, 0);
    void *wret = NULL;
    pthread_join(w, &wret);
    prow("futex_waiter_woken", (long)wret == 0 ? 1 : 0, 0, NULL, 0);
  }

  pstr("t4-probe-arrived\n");
  pstr("probe-exit\n");
  return 0;
}
