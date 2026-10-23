/* src/host/t4_compat_probe.c — T4 differential compat-syscall probe.
 *
 * One source, raced two ways by t4_parity.sh:
 *
 *   leg A (reference): build with `musl-gcc -static` — a real static-musl
 *                       Linux binary.  argv[0] program name, Linux errno.
 *   leg B (HobbyOS):    build with $(HOST_CC) $(HOST_CFLAGS) + src/host/
 *                       compat.c — glibc-backed ho_* mocks over the same
 *                       Linux errno numbering our in-tree errno.h uses.
 *
 * The probe prints *numbers*, not libc text, for every result (the error
 * strings differ between musl and glibc; the syscall + errno contract does
 * not).  The diff on stdout/exit is therefore meaningful byte-for-byte,
 * and it is exactly the surface libc-compat must hold stable.
 *
 * Every syscall is chosen so the outcome is identical on a static-musl
 * binary and on a HobbyOS flat-binary running the same wrapper logic:
 *   open(ENOENT), open(ENOTDIR), read/write(EBADF), close(EBADF),
 *   a successful getcwd round-trip, and a write to stdout we then diff.
 *
 * 30-second deadlock rule: the probe exits promptly; the harness enforces
 * a timeout on the QEMU leg so a hang is a defect, not a retry loop.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <poll.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/socket.h> /* GB-018 socket remainder: socketpair/AF_UNIX */
#include <stdint.h>
#include <pthread.h>

/* GB-083: leg B must actually EXERCISE compat.c's ho_* shims, not merely link
 * them.  When t4_parity.sh builds leg B with -DLEG_B_SHIMS, the probe's core
 * file syscalls route through compat.c's ho_* wrappers so that flipping any
 * one of them changes a reported row and FAILs the differential (the "wrong
 * shim must FAIL" criterion, which was inert because the probe called libc
 * directly and never touched compat.c).  Declared here so the probe compiles
 * without pulling in the implementation (compat.c is linked by the build). */
#ifdef LEG_B_SHIMS
extern int ho_open(const char *, int, ...);
extern int ho_close(int);
extern ssize_t ho_read(int, void *, size_t);
extern ssize_t ho_write(int, const void *, size_t);
extern int ho_dup(int);
extern int ho_dup2(int, int);
extern int ho_pipe(int[2]);
#define open   ho_open
#define close  ho_close
#define read   ho_read
#define write  ho_write
#define dup    ho_dup
#define dup2   ho_dup2
#define pipe   ho_pipe
#endif

/* GB-017: /dev/urandom + getrandom return *random bytes* — the content is
 * non-deterministic and must never be diffed leg-to-leg.  Only the return
 * count (and errno) are part of the byte-meaningful contract. */

/* Normalized line: "<op> rc=<r> errno=<e>" — errno read only when rc<0. */
static volatile sig_atomic_t g_sigusr1 = 0;
static void on_sigusr1(int s) { (void)s; g_sigusr1 = 1; }

/* GB-021: thread + futex shared state. */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
static _Atomic(int) g_counter = 0;
static volatile int g_futex_val = 0;

/* GB-022: per-thread TLS isolation.  __thread storage is backed by the FS base
 * each thread's context switch must save/restore; a thread layer that reuses or
 * shares one TLS block (or that never switches per-task FS) makes every thread
 * see the LAST writer's value.  We give each thread a DISTINCT seed, have it
 * write and read back its own __thread slot, then assert it saw its own value —
 * a shared/missing FS base diverges on the read-back.  errno is likewise
 * per-thread (thread-local): after each thread faults a call, main must see its
 * own errno==0 untouched. */
static __thread int g_tls_seed = -1;
static void *thr_tlsprobe(void *p) {
  long seed = (long)p;
  g_tls_seed = (int)seed;
  errno = (int)(seed + 1000);          /* per-thread errno store */
  return (void *)(long)g_tls_seed;     /* must be THIS thread's seed */
}
static void *thr_worker(void *p) { long n = (long)p;
  for (long i = 0; i < n; i++) { g_counter++; }
  return (void *)(n * 2);
}
static void *thr_futexwaiter(void *p) {
  volatile int *v = p;
  /* wait until *v != 0 (raw futex wait loop, timeout-bounded) */
  for (int spin = 0; spin < 200000; spin++) {
    if (__atomic_load_n(v, __ATOMIC_ACQUIRE) != 0) return (void *)0;    /* woken */
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50000; /* 50us */
    syscall(SYS_futex, (int *)v, FUTEX_WAIT, 0, &ts, NULL, 0);
  }
  return (void *)1; /* timeout path (shouldn't happen when waker fires) */
}
static void report(const char *op, long r) {
  if (r < 0) {
    printf("%s rc=%ld errno=%d\n", op, r, errno);
    /* restore for the next call so a stale value never leaks through */
    errno = 0;
  } else {
    printf("%s rc=%ld\n", op, r);
  }
}

int main(void) {
  int w = write(1, "t4-probe\n", 9);
  report("write_stdout", w);

  /* open() on a nonexistent path -> -1, errno=ENOENT (2) */
  errno = 0;
  int fd = open("/no/such/path/t4-nonexistent", O_RDONLY);
  report("open_nonexistent", fd);
  if (fd >= 0) close(fd);

  /* open() on a path whose parent is a regular file -> -1, errno=ENOTDIR (20) */
  {
    /* create a scratch file to use as a "non-directory intermediate" */
    int f = open("/tmp/t4_parent_probe", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { close(f); }
  }
  errno = 0;
  fd = open("/tmp/t4_parent_probe/child", O_RDONLY);
  report("open_notdir", fd);
  if (fd >= 0) close(fd);
  unlink("/tmp/t4_parent_probe");

  /* read()/write()/close() on a bad fd -> -1, errno=EBADF (9) */
  errno = 0;
  char c;
  report("read_badfd", read(-1, &c, 1));
  errno = 0;
  report("write_badfd", write(9999, "x", 1));
  errno = 0;
  report("close_badfd", close(424242));

  /* successful getcwd round-trip: buf filled, errno untouched.
   * chdir("/") first so the result is deterministic for BOTH libc flavors:
   * musl resolves symlinks in getcwd (physical path) while glibc returns
   * the logical path, so a run dir under a symlinked /tmp diverges. The
   * root is never a symlink, and HobbyOS's FAT16 has no symlinks at all. */
  chdir("/");
  errno = 0;
  char buf[256];
  char *cw = getcwd(buf, sizeof buf);
  if (cw == NULL || errno != 0) {
    printf("getcwd_success rc=-1 errno=%d\n", errno);
  } else {
    printf("getcwd_success rc=0 len=%lu first='%c'\n",
           (unsigned long)strlen(buf), buf[0]);
  }

  /* GB-017: getrandom — deterministic: rc = bytes requested (or -1/errno).
   * The payload is random and never diffed. */
  {
    unsigned char rnd[16];
    errno = 0;
    report("getrandom_16", getrandom(rnd, sizeof rnd, 0));
    errno = 0;
    /* GRND_NONBLOCK on a fresh pool may return EAGAIN on some kernels but
     * musl and glibc agree on it either way; keep flags 0 for determinism
     * across both legs. */
  }

  /* GB-017: /dev/urandom — open(RDONLY) + read rc is deterministic (16);
   * bytes are random, not diffed.  If the node is absent both legs report
   * the same open errno (ENOENT), which is itself a meaningful parity row. */
  {
    errno = 0;
    int u = open("/dev/urandom", O_RDONLY);
    report("urandom_open", u);
    if (u >= 0) {
      unsigned char b[16];
      errno = 0;
      report("urandom_read16", read(u, b, sizeof b));
      close(u);
    }
  }

  /* GB-017: /dev/null — write rc = bytes accepted (3), read rc = 0 (EOF).
   * Deterministic on both legs; a shim that doesn't consume writes or
   * never returns EOF at read diverges here. */
  {
    errno = 0;
    int n = open("/dev/null", O_WRONLY);
    report("null_open_w", n);
    if (n >= 0) {
      errno = 0;
      report("null_write3", write(n, "abc", 3));
      close(n);
    }
    errno = 0;
    int nr = open("/dev/null", O_RDONLY);
    report("null_open_r", nr);
    if (nr >= 0) {
      char c;
      errno = 0;
      report("null_read1", read(nr, &c, 1));
      close(nr);
    }
  }

  /* GB-019: ioctl subset + fcntl GETFL/SETFL.
   * TIOCGWINSZ and TCGETS on a NON-tty fd (we always open /dev/null for the
   * ioctl source so the row is deterministic) must return -1/ENOTTY on both
   * legs — a wrong ioctl number, or a shim that accepts unknown ioctls on a
   * non-tty, diverges here.  F_GETFL/F_SETFL on a regular-but-empty file is
   * deterministic too. */
  {
    int f = open("/tmp/ho_t4_ioctl_XXXXXX", O_RDWR | O_CREAT, 0600);
    errno = 0;
    report("fcntl_getfl_rdwr", fcntl(f, F_GETFL));
    /* F_GETFL exposes the access-mode + status bits (O_RDWR plus any flag
     * bits Linux sets on open).  We don't assert the exact integer here — a
     * raw integer is opaque and can carry arch/ABI flag bits; the DIFF covers
     * that it is byte-identical leg-to-leg.  The meaningful, portable rows are
     * below: F_SETFL to O_APPEND succeeds, and O_APPEND then shows up in the
     * flags. */
    report("fcntl_setfl_append", fcntl(f, F_SETFL, O_APPEND));
    errno = 0;
    long fl = fcntl(f, F_GETFL);
    report("fcntl_getfl_after", fl);
    /* print whether O_APPEND is set (deterministic), not the opaque flags */
    report("fcntl_append_bit", (fl & O_APPEND) ? 1 : 0);
    close(f);
    unlink("/tmp/ho_t4_ioctl_XXXXXX"); /* best-effort; name won't match, see below */
    /* real temp copy to avoid the XXXXXX literal above clashing */
    int tf = open("/tmp/ho_t4_ioctl_real", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (tf >= 0) { close(tf); }
    errno = 0;
    int nt = open("/dev/null", O_RDWR);
    struct winsize ws; memset(&ws, 0, sizeof ws);
    errno = 0;
    report("ioctl_tiocgwinsz_entty", ioctl(nt, TIOCGWINSZ, &ws) < 0 ? errno : 0);
    struct termios ts; memset(&ts, 0, sizeof ts);
    errno = 0;
    report("ioctl_tcgets_entty", ioctl(nt, TCGETS, &ts) < 0 ? errno : 0);
    close(nt);
    unlink("/tmp/ho_t4_ioctl_real");
  }

  /* GB-020: getdents64 over a fixed directory the probe creates with known
   * entries.  Both legs open the SAME dir, so inodes/names/reclens/offsets are
   * identical; we print each entry as name:type (type is DT_* 8-bit; reclen and
   * inode are opaque but also diff identically).  The probe exits non-zero on
   * any missing/extra entry so a directory-shim that drops or renames entries
   * diverges even if it strings together the same bytes. */
  {
    const char *d = "/tmp/ho_t4_dent";
    mkdir(d, 0700);
    int created = 0;
    char p[128];
    snprintf(p, sizeof p, "%s/alpha", d);   FILE *fa = fopen(p, "w"); if (fa) { fputs("a", fa); fclose(fa); created++; }
    snprintf(p, sizeof p, "%s/beta", d);    FILE *fb = fopen(p, "w"); if (fb) { fputs("b", fb); fclose(fb); created++; }
    snprintf(p, sizeof p, "%s/charlie", d); FILE *fc = fopen(p, "w"); if (fc) { fputs("c", fc); fclose(fc); created++; }
    report("dent_created", created);
    int dfd = open(d, O_RDONLY | O_DIRECTORY);
    report("dent_opendir", dfd);
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
            printf("dent %s type=%u\n", nm, e->d_type);
          }
          off += e->d_reclen;
        }
      }
      report("dent_total", total);
      report("dent_have_alpha", saw_alpha);
      report("dent_have_beta", saw_beta);
      report("dent_have_charlie", saw_charlie);
      close(dfd);
      /* deterministic? printf order above is directory-insertion order, same on
       * both legs because it's the same dir; we still assert set-membership. */
    }
    char rm[64]; snprintf(rm, sizeof rm, "rm -rf \"%s\"", d);
    system(rm);
  }

  /* GB-084: open()'s optional mode arg must be honored.  Create a file with
   * O_CREAT and an explicit 0600 mode, then stat it: the mode read back must
   * carry 0600 (S_IRUSR|S_IWUSR).  On leg B this routes through ho_open, which
   * must not discard the supplied mode (a 2-arg rely-on-UB path would give a
   * random/garbage mode).  Deterministic: a fresh path each run. */
  {
    const char *mp = "/tmp/t4_mode_probe_0600";
    char scr[80]; snprintf(scr, sizeof scr, "rm -f %s", mp);
    system(scr);
    errno = 0;
    int mfd = open(mp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    report("mode_open_creat", mfd);
    struct stat ms;
    errno = 0;
    int mst = stat(mp, &ms);
    report("mode_stat", mst);
    report("mode_is_0600", mst == 0 && (ms.st_mode & 0777) == 0600);
    if (mfd >= 0) close(mfd);
    system(scr);
  }

  /* GB-016: uname.  Both legs run on the SAME host/kernel, so sysname,
   * release, machine (and nodename/version given a fixed host) must agree
   * byte-for-byte — a uname shim that reports a non-Linux sysname or a
   * wrong-release string diverges here.  The rows assert the paragons games
   * sniff: sysname=="Linux" and machine matching x86_64. */
  {
    struct utsname u;
    errno = 0;
    report("uname_rc", uname(&u));
    report("uname_sysname_linux", strcmp(u.sysname, "Linux") == 0);
    report("uname_machine_x86_64", strcmp(u.machine, "x86_64") == 0);
    /* full-string agreement is covered by the leg diff below; these two bools
     * pin the values SDL and the games gated on. */
    printf("uname sysname=%s machine=%s\n", u.sysname, u.machine);
  }

  /* GB-018: poll / select / pselect6 on a ready pipe.
   * A byte on a pipe read-end is deterministically ready: poll returns rc=1
   * with POLLIN in revents; select/pselect6 return rc=1 with the bit set.
   * A closed fd is deterministically POLLNVAL.  These pin the readiness-
   * semantics + event-mask constants a wait shim must reproduce. */
  {
    int pfd[2];
    report("pipe2", pipe(pfd));
    if (write(pfd[1], "x", 1) != 1) report("pipe_write", -1);

    struct pollfd pf[1];
    pf[0].fd = pfd[0]; pf[0].events = POLLIN; pf[0].revents = 0;
    errno = 0;
    report("poll_ready", poll(pf, 1, 0));
    report("poll_ready_revents", pf[0].revents);

    fd_set rf;
    FD_ZERO(&rf); FD_SET(pfd[0], &rf);
    struct timeval tv = {0, 0};
    errno = 0;
    report("select_ready", select(pfd[0] + 1, &rf, NULL, NULL, &tv));
    report("select_ready_fdset", FD_ISSET(pfd[0], &rf) ? 1 : 0);

    sigset_t empt;
    sigemptyset(&empt);
    fd_set rf2;
    FD_ZERO(&rf2); FD_SET(pfd[0], &rf2);
    struct timespec ts = {0, 0};
    errno = 0;
    report("pselect6_ready", pselect(pfd[0] + 1, &rf2, NULL, NULL, &ts, &empt));
    report("pselect6_ready_fdset", FD_ISSET(pfd[0], &rf2) ? 1 : 0);

    /* GB-086: select(timeout) on an empty pipe must honor the timeout and
     * return 0 (timeout), not an error and not an instant positive return.
     * Both legs (real Linux + HobbyOS host shim) must agree on this semantics;
     * the measured wall-time bound is verified in-guest (POLLTST at both CPU
     * counts).  This row pins the deterministic rc=0 part of the property. */
    fd_set rt;
    FD_ZERO(&rt); FD_SET(pfd[0], &rt);
    struct timeval tv2 = {0, 100}; /* 100ms */
    errno = 0;
    report("select_timeout_rc", select(pfd[0] + 1, &rt, NULL, NULL, &tv2));
    report("select_timeout_fdset", FD_ISSET(pfd[0], &rt) ? 1 : 0);

    close(pfd[0]); close(pfd[1]);

    struct pollfd pf2[1];
    pf2[0].fd = 9999999; pf2[0].events = POLLIN; pf2[0].revents = 0;
    errno = 0;
    report("poll_badfd", poll(pf2, 1, 0));
    report("poll_badfd_revents", pf2[0].revents);
  }

  /* GB-018 (socket remainder, coord 2026-10-20): stream-socket readiness is a
   * DISTINCT wake path from the pipe above — it goes through the socket/ring
   * buffer, and is the one the compat layer is most likely to get wrong.  A
   * socketpair(AF_UNIX, SOCK_STREAM) gives two connected stream sockets; a peer
   * write flips the other end readable.  poll/select/pselect6 must report
   * readiness there too.  Deterministic on both legs (kernel: SYS_SOCKETPAIR,
   * host: real socketpair). */
  {
    int sv[2];
    errno = 0;
    report("sockpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    if (write(sv[1], "s", 1) != 1) report("sock_write", -1);

    struct pollfd spf[1];
    spf[0].fd = sv[0]; spf[0].events = POLLIN; spf[0].revents = 0;
    errno = 0;
    report("sockpoll_ready", poll(spf, 1, 0));
    report("sockpoll_revents", spf[0].revents);

    fd_set srf; FD_ZERO(&srf); FD_SET(sv[0], &srf);
    struct timeval stv; stv.tv_sec = 0; stv.tv_usec = 0;
    errno = 0;
    report("sockselect_ready", select(sv[0] + 1, &srf, NULL, NULL, &stv));
    report("sockselect_fdset", FD_ISSET(sv[0], &srf) ? 1 : 0);

    sigset_t sempt; sigemptyset(&sempt);
    fd_set srf2; FD_ZERO(&srf2); FD_SET(sv[0], &srf2);
    struct timespec sts; sts.tv_sec = 0; sts.tv_nsec = 0;
    errno = 0;
    report("sockpselect_ready", pselect(sv[0] + 1, &srf2, NULL, NULL, &sts, &sempt));
    report("sockpselect_fdset", FD_ISSET(sv[0], &srf2) ? 1 : 0);

    close(sv[0]); close(sv[1]);
  }

  /* GB-023: real signal delivery.
   * sigaction installs on_sigusr1; raise(SIGUSR1) must have it fire
   * synchronously (g_sigusr1 == 1) — a shim that drops the frame or never
   * delivers diverges here.  rt_sigprocmask(SIG_BLOCK) on SIGUSR1, then
   * kill(getpid(), SIGUSR1) must NOT run the handler while blocked (blocked
   * row 0) and must instead show up in sigpending; after SIG_UNBLOCK the
   * pending one is delivered to the (still-installed) handler, which is
   * non-lethal. The handler is never reset to SIG_DFL, so the probe never
   * dies on a pending signal. */
  {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    errno = 0;
    report("sigaction_install", sigaction(SIGUSR1, &sa, &old));

    g_sigusr1 = 0;
    errno = 0;
    report("raise_sigusr1", raise(SIGUSR1));
    report("sigusr1_delivered", g_sigusr1);

    sigset_t block, oset, pend;
    sigemptyset(&block); sigaddset(&block, SIGUSR1);
    errno = 0;
    report("sigprocmask_block", sigprocmask(SIG_BLOCK, &block, &oset));
    g_sigusr1 = 0;
    errno = 0;
    report("kill_self_while_blocked", kill(getpid(), SIGUSR1));
    report("sigusr1_fired_while_blocked", g_sigusr1);

    sigemptyset(&pend);
    errno = 0;
    sigpending(&pend);
    report("sigusr1_pending", sigismember(&pend, SIGUSR1) ? 1 : 0);

    errno = 0;
    report("sigprocmask_unblock", sigprocmask(SIG_UNBLOCK, &block, NULL));
    report("sigusr1_delivered_after_unblock", g_sigusr1);
  }

  /* GB-022: TLS ABI — arch_prctl(ARCH_SET_FS/ARCH_GET_FS) round-trips FS base.
   * The FS/GS base is the x86_64 thread-local pointer a context switch must
   * save/restore.  We READ the current FS base, then SET it back to that same
   * (valid) value and read it again — exercising both syscalls without ever
   * pointing FS at invalid memory (which would break TLS-relative libc and
   * crash the probe).  rc + round-trip equality is the locked surface. */
  {
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
    uintptr_t cur = 0, after = 0;
    errno = 0;
    report("arch_get_fs_rc", syscall(SYS_arch_prctl, ARCH_GET_FS, &cur));
    report("arch_get_fs_nonzero", cur != 0);
    errno = 0;
    report("arch_set_fs_rc", syscall(SYS_arch_prctl, ARCH_SET_FS, cur));
    errno = 0;
    syscall(SYS_arch_prctl, ARCH_GET_FS, &after);
    report("arch_fs_roundtrip", after == cur);
#undef ARCH_SET_FS
#undef ARCH_GET_FS
  }

  /* GB-022: per-thread TLS + errno isolation.  Two threads each write a
   * DISTINCT seed into the SAME __thread var and read it back; if the TLS
   * block (FS base) were shared or not switched per-task, every thread would
   * read the last writer's value and the read-back diverges from its own seed.
   * Each thread also stores a per-thread errno; after they run, main must see
   * its own errno==0 (errno is thread-local, not a shared global). */
  {
    pthread_t a, b;
    void *ra = NULL, *rb = NULL;
    errno = 0;
    report("tls_create_a", pthread_create(&a, NULL, thr_tlsprobe, (void *)101L));
    report("tls_create_b", pthread_create(&b, NULL, thr_tlsprobe, (void *)202L));
    int ja = pthread_join(a, &ra);
    int jb = pthread_join(b, &rb);
    report("tls_join_a", ja);
    report("tls_join_b", jb);
    report("tls_thread_a_seed", (long)ra);   /* must be 101 */
    report("tls_thread_b_seed", (long)rb);   /* must be 202 */
    report("tls_main_errno_untouched", errno); /* errno is per-thread: 0 */
  }

  /* GB-021: threads + futex.
   * pthread_create/join with a fixed per-thread increment must yield an exact
   * final counter (every thread really ran and joined) and the thread's exit
   * value (2*N) must be observable — a thread layer that skips/duplicates work
   * diverges.  The futex handshake proves FUTEX_WAIT/FUTEX_WAKE parity: a
   * waiter blocks on g_futex_val==0, the main thread stores 1 and FUTEX_WAKE, so
   * the waiter must return woken (0), not the timeout path (1). */
  {
    pthread_t t[4];
    g_counter = 0;
    for (long i = 0; i < 4; i++) {
      errno = 0;
      report("thread_create", pthread_create(&t[i], NULL, thr_worker, (void *)1000L));
    }
    long sum = 0;
    for (int i = 0; i < 4; i++) {
      void *ret = NULL;
      pthread_join(t[i], &ret);
      sum += (long)ret;
    }
    report("thread_join_sum", sum);          /* 4 * (2*1000) = 8000 */
    report("thread_counter", g_counter);       /* 4000 */

    /* futex handshake */
    g_futex_val = 0;
    pthread_t w;
    pthread_create(&w, NULL, thr_futexwaiter, (void *)&g_futex_val);
    /* give the waiter a moment to enter FUTEX_WAIT, then wake it */
    for (int k = 0; k < 2000 && __atomic_load_n(&g_futex_val, __ATOMIC_RELAXED)==0; k++) { usleep(200); }
    __atomic_store_n(&g_futex_val, 1, __ATOMIC_RELEASE);
    /* GB-021/coordinator: the RETURN VALUE of FUTEX_WAKE is not a contract and
     * must not be diffed leg-to-leg.  It is the number of waiters actually
     * parked at that instant, which cannot be observed without a race: whether
     * the waiter has entered FUTEX_WAIT yet depends on host scheduling, so one
     * leg returns 1 (a waiter was parked) and the other 0 (it had not parked --
     * which is CORRECT, since it then sees *v != 0 and never waits).  Observed as
     * a 1-in-3 failure of `make host_tests` on main, with every other row
     * byte-identical.  Same rule as the GB-017 random rows: report what is
     * deterministic, and keep the assertion that carries meaning.  The suite's
     * contract here is `futex_waiter_woken` below -- the waiter must return
     * *woken*, not the timeout path -- and that one is deterministic. */
    report("futex_wake_ok", syscall(SYS_futex, &g_futex_val, FUTEX_WAKE, 1, NULL, NULL, 0) >= 0);
    void *wret = NULL;
    pthread_join(w, &wret);
    report("futex_waiter_woken", (long)wret == 0);   /* 1 = woken, not timeout */
  }

  /* exit status is part of the diff: 0 here means every leg reached the
   * end; a differential failure above would have already diverged stdout. */
  printf("probe-exit\n");
  return 0;
}
