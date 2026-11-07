/*
 * src/user/linux_probe_proc.c — Linux-ABI probe: the process/identity/stat/
 * signal half of the compat layer (l_tab_proc).
 *
 * ORDINARY musl Linux program: no HobbyOS header, no HobbyOS-ism, so the same
 * binary runs here on the workstation (reference) and inside the intel guest
 * (measured).  Rows are "[LP] ..." and the last one is "[LP] done rc=<fails>";
 * tools/gate/t3_linux_probe.sh proc only accepts rc=0.
 *
 * What each row proves (in the order it depends on the compat layer):
 *
 *   readlink(/proc/self/exe)  the virtual symlink + the process's own name
 *   stat(exe)                 the 144-byte Linux layout: S_ISREG, REAL size
 *   open+read count           the size is not invented: bytes read == st_size
 *   fstat(fd)                 the fd form agrees with the path form
 *   stat(missing)             -ENOENT, not a zeroed struct
 *   fstat(bad fd)             -EBADF
 *   lstat("/")                directories are S_IFDIR (Doom stats its WAD dir)
 *   fstatat(AT_FDCWD, ...)    the *at form musl/glibc prefer
 *   readlink(regular file)    -EINVAL, so a caller can tell link from file
 *   getrlimit/prlimit64       RLIMIT_NOFILE and RLIMIT_STACK are real numbers
 *   sysinfo                   totalram/freeram/uptime are plausible and ordered
 *   times                     the CLK_TCK return value advances
 *   getrusage                 the 144-byte struct is accepted
 *   umask                     set-and-return round trip
 *   sigprocmask               an 8-byte mask round-trips through the kernel
 *   sigaction                 install + query returns the same handler
 *   kill(getpid,0)            the existence probe succeeds for self
 *   raise(SIGINT)             DELIVERY: the handler runs and rt_sigreturn
 *                             (musl's restorer) resumes this exact context
 *   access                    R_OK on a real file, ENOENT on a missing one
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static void fail_row(const char *what, int err) {
  row("[LP] %s FAIL errno=%d (%s)\n", what, err, strerror(err));
  fails++;
}

static volatile sig_atomic_t got_int;

static void on_int(int s) { got_int = s; }

int main(void) {
  row("[LP] proc probe start pid=%ld uid=%ld\n", (long)getpid(), (long)getuid());

  /* --- the process's own path, via the virtual symlink ----------------- */
  char exe[256];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) {
    fail_row("readlink(/proc/self/exe)", errno);
    memcpy(exe, "/PROBE.ELF", 11);
  } else {
    exe[n] = 0;
    row("[LP] readlink exe=%s len=%ld\n", exe, (long)n);
  }

  /* --- stat: the 144-byte layout, with a REAL size --------------------- */
  struct stat st;
  if (stat(exe, &st) != 0) {
    fail_row("stat(exe)", errno);
  } else {
    int isreg = S_ISREG(st.st_mode), sane = st.st_size > 0;
    row("[LP] stat exe size=%lld mode=%04o isreg=%d nlink=%lld ino=%llu %s\n",
        (long long)st.st_size, (unsigned)st.st_mode, isreg,
        (long long)st.st_nlink, (unsigned long long)st.st_ino,
        (isreg && sane) ? "ok" : "BAD");
    if (!isreg || !sane) fails++;
    /* FAT gives a 512-byte cluster-multiple blksize; st_blocks must at least
       cover the file in those units or a caller sizing a copy is short. */
    row("[LP] stat blksize=%lld blocks=%lld %s\n", (long long)st.st_blksize,
        (long long)st.st_blocks,
        (st.st_blksize > 0 && st.st_blocks * st.st_blksize >= st.st_size) ? "ok"
                                                                        : "BAD");
    if (!(st.st_blksize > 0 && st.st_blocks * st.st_blksize >= st.st_size))
      fails++;
  }

  /* --- the size is measured, not invented: read the bytes -------------- */
  int fd = open(exe, O_RDONLY);
  if (fd < 0) {
    fail_row("open(exe)", errno);
  } else {
    static char buf[65536];
    long long total = 0;
    long long r;
    while ((r = read(fd, buf, sizeof buf)) > 0) total += r;
    struct stat fst;
    if (fstat(fd, &fst) != 0) fail_row("fstat(fd)", errno);
    int match = total == (long long)st.st_size && fst.st_size == st.st_size &&
                fst.st_ino == st.st_ino;
    row("[LP] read bytes=%lld st_size=%lld fstat_size=%lld ino_eq=%d %s\n",
        total, (long long)st.st_size, (long long)fst.st_size,
        fst.st_ino == st.st_ino, match ? "ok" : "BAD");
    if (!match) fails++;
    close(fd);
  }

  /* --- error paths must be errors, not zeroed structs ------------------ */
  struct stat miss;
  errno = 0;
  if (stat("/NOPE.TXT", &miss) == 0) {
    row("[LP] stat(missing) FAIL returned 0\n");
    fails++;
  } else {
    row("[LP] stat(missing) errno=%d %s\n", errno, errno == ENOENT ? "ok" : "BAD");
    if (errno != ENOENT) fails++;
  }
  errno = 0;
  if (fstat(12345, &miss) == 0) {
    row("[LP] fstat(badfd) FAIL returned 0\n");
    fails++;
  } else {
    row("[LP] fstat(badfd) errno=%d %s\n", errno, errno == EBADF ? "ok" : "BAD");
    if (errno != EBADF) fails++;
  }

  /* --- directories, and the *at form musl prefers ---------------------- */
  struct stat dst;
  if (lstat("/", &dst) != 0) {
    fail_row("lstat(/)", errno);
  } else {
    row("[LP] lstat root isdir=%d mode=%04o %s\n", S_ISDIR(dst.st_mode),
        (unsigned)dst.st_mode, S_ISDIR(dst.st_mode) ? "ok" : "BAD");
    if (!S_ISDIR(dst.st_mode)) fails++;
  }
  struct stat atst;
  if (fstatat(AT_FDCWD, exe, &atst, 0) != 0) {
    fail_row("fstatat(AT_FDCWD,exe)", errno);
  } else {
    int same = atst.st_size == st.st_size && atst.st_ino == st.st_ino;
    row("[LP] fstatat size=%lld same_as_stat=%d %s\n", (long long)atst.st_size,
        same, same ? "ok" : "BAD");
    if (!same) fails++;
  }

  /* --- readlink on a regular file is EINVAL (link vs file) ------------- */
  char lb[64];
  errno = 0;
  if (readlink(exe, lb, sizeof lb) >= 0) {
    row("[LP] readlink(file) FAIL returned >=0\n");
    fails++;
  } else {
    row("[LP] readlink(file) errno=%d %s\n", errno, errno == EINVAL ? "ok" : "BAD");
    if (errno != EINVAL) fails++;
  }

  /* --- resource limits ------------------------------------------------- */
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    fail_row("getrlimit(NOFILE)", errno);
  } else {
    row("[LP] rlimit nofile cur=%llu max=%llu %s\n",
        (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max,
        (rl.rlim_cur > 2 && rl.rlim_max >= rl.rlim_cur) ? "ok" : "BAD");
    if (!(rl.rlim_cur > 2 && rl.rlim_max >= rl.rlim_cur)) fails++;
  }
  struct rlimit rs;
  if (getrlimit(RLIMIT_STACK, &rs) != 0) {
    fail_row("getrlimit(STACK)", errno);
  } else {
    row("[LP] rlimit stack cur=%llu %s\n", (unsigned long long)rs.rlim_cur,
        rs.rlim_cur > 0 ? "ok" : "BAD");
    if (!(rs.rlim_cur > 0)) fails++;
  }
#ifdef SYS_prlimit64
  struct rlimit pr;
  long prc = syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, (void *)0, &pr);
  int pok = prc == 0 && pr.rlim_cur == rl.rlim_cur;
  row("[LP] prlimit64 rc=%ld cur=%llu %s\n", prc,
      (unsigned long long)pr.rlim_cur, pok ? "ok" : "BAD");
  if (!pok) fails++;
#endif

  /* --- sysinfo: 112 bytes, ordered totals ------------------------------ */
  struct sysinfo si;
  if (sysinfo(&si) != 0) {
    fail_row("sysinfo", errno);
  } else {
    int ok = si.totalram > 0 && si.freeram <= si.totalram && si.mem_unit > 0;
    row("[LP] sysinfo uptime=%ld totalram=%llu freeram=%llu procs=%u unit=%u %s\n",
        (long)si.uptime, (unsigned long long)si.totalram,
        (unsigned long long)si.freeram, (unsigned)si.procs,
        (unsigned)si.mem_unit, ok ? "ok" : "BAD");
    if (!ok) fails++;
  }

  /* --- times: the CLK_TCK return value must advance with the clock ----- */
  struct tms t0, t1;
  clock_t k0 = times(&t0);
  if (k0 == (clock_t)-1) {
    fail_row("times", errno);
  } else {
    struct timespec nap = {0, 60 * 1000 * 1000};
    nanosleep(&nap, 0);
    clock_t k1 = times(&t1);
    /* 60 ms is 6 ticks at 100 Hz; require >=2 so a coarse tick cannot fail
       this spuriously, and a frozen counter cannot pass it. */
    int ok = k1 - k0 >= 2;
    row("[LP] times t0=%lld t1=%lld ticks_delta=%lld %s\n", (long long)k0,
        (long long)k1, (long long)(k1 - k0), ok ? "ok" : "BAD");
    if (!ok) fails++;
  }

  /* --- getrusage: the 144-byte struct is accepted ---------------------- */
  struct rusage ru;
  memset(&ru, 0xAA, sizeof ru);
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    fail_row("getrusage(SELF)", errno);
  } else {
    int ok = ru.ru_utime.tv_sec == 0 && ru.ru_stime.tv_sec == 0 &&
             ru.ru_maxrss >= 0;
    row("[LP] getrusage utime=%ld.%06ld maxrss=%ld %s\n", (long)ru.ru_utime.tv_sec,
        (long)ru.ru_utime.tv_usec, (long)ru.ru_maxrss, ok ? "ok" : "BAD");
    if (!ok) fails++;
  }

  /* --- umask round trip ------------------------------------------------ */
  mode_t old_um = umask(027);
  mode_t now_um = umask(old_um);
  int um_ok = now_um == 027;
  row("[LP] umask old=%04o roundtrip=%04o %s\n", (unsigned)old_um,
      (unsigned)now_um, um_ok ? "ok" : "BAD");
  if (!um_ok) fails++;

  /* --- signal mask: 8-byte sigset, per process ------------------------- */
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
    fail_row("sigprocmask(BLOCK)", errno);
  } else {
    sigset_t q;
    sigemptyset(&q);
    int qok = sigprocmask(SIG_BLOCK, NULL, &q) == 0;
    int blocked = qok && sigismember(&q, SIGUSR1) == 1;
    /* UNBLOCK is a different `how` (a different kernel branch): the bit must
       come back out again, otherwise a program that blocks around a critical
       section would never see the signal. */
    int un_ok = sigprocmask(SIG_UNBLOCK, &set, NULL) == 0;
    sigset_t q2;
    int cleared = un_ok && sigprocmask(SIG_BLOCK, NULL, &q2) == 0 &&
                  sigismember(&q2, SIGUSR1) == 0;
    row("[LP] sigprocmask block_seen=%d unblock_seen=%d %s\n", blocked, cleared,
        (blocked && cleared) ? "ok" : "BAD");
    if (!(blocked && cleared)) fails++;
  }

  /* --- sigaction: install, query, restore ------------------------------ */
  struct sigaction sa, old_sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_int;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGINT, &sa, &old_sa) != 0) {
    fail_row("sigaction(SIGINT)", errno);
  } else {
    struct sigaction got;
    int q = sigaction(SIGINT, NULL, &got) == 0;
    int same = q && got.sa_handler == on_int;
    row("[LP] sigaction installed=%d queried=%d same=%d %s\n", 1, q, same,
        same ? "ok" : "BAD");
    if (!same) fails++;
  }

  /* --- kill(pid, 0): the existence probe ------------------------------- */
  errno = 0;
  int kself = kill(getpid(), 0);
  int kgroup = kill(0, 0);
  row("[LP] kill pid0=%d group0=%d %s\n", kself, kgroup,
      (kself == 0 && kgroup == 0) ? "ok" : "BAD");
  if (!(kself == 0 && kgroup == 0)) fails++;

  /* --- DELIVERY: raise(SIGINT) must run on_int and RETURN here ---------
   * This is the row that exercises the whole signal path: process_signal()
   * sets the pending bit, the kernel plants the native frame at its next
   * return-to-user boundary, the handler runs with rdi = 2, and musl's
   * restorer executes rt_sigreturn (Linux 15) which must resume THIS context.
   * If the frame or the restore is wrong the probe dies here — the done marker
   * is the verdict.
   *
   * The wait loop is part of the evidence, not hedging: a signal raised by a
   * compat syscall is pending when that syscall returns, and this kernel
   * delivers at the next return-to-user boundary it actually reaches (the
   * 100 Hz tick), so delivery lands within a tick or two.  A guest that never
   * delivers never satisfies the loop and the row fails. */
  row("[LP] raise signal=%d before got=%d\n", SIGINT, (int)got_int);
  if (raise(SIGINT) != 0) fail_row("raise(SIGINT)", errno);
  int waited = -1;
  for (int ms = 0; ms <= 200; ms += 2) {
    if (got_int == SIGINT) {
      waited = ms;
      break;
    }
    struct timespec nap = {0, 2 * 1000 * 1000};
    nanosleep(&nap, 0);
  }
  row("[LP] raise handler_ran=%d got=%d waited_ms=%d %s\n", got_int == SIGINT,
      (int)got_int, waited, got_int == SIGINT ? "ok" : "BAD");
  if (got_int != SIGINT) fails++;
  /* And the context must still be usable: a second round trip proves the
     first restore did not corrupt the stack/PC. */
  got_int = 0;
  raise(SIGINT);
  waited = -1;
  for (int ms = 0; ms <= 200; ms += 2) {
    if (got_int == SIGINT) {
      waited = ms;
      break;
    }
    struct timespec nap = {0, 2 * 1000 * 1000};
    nanosleep(&nap, 0);
  }
  row("[LP] raise2 handler_ran=%d waited_ms=%d %s\n", got_int == SIGINT, waited,
      got_int == SIGINT ? "ok" : "BAD");
  if (got_int != SIGINT) fails++;

  /* --- access ---------------------------------------------------------- */
  int a_ok = access(exe, R_OK) == 0;
  errno = 0;
  int a_miss = access("/NOPE.TXT", F_OK) != 0 && errno == ENOENT;
  row("[LP] access read=%d missing_enoent=%d %s\n", a_ok, a_miss,
      (a_ok && a_miss) ? "ok" : "BAD");
  if (!(a_ok && a_miss)) fails++;

  row("[LP] done rc=%d\n", fails);
  return fails != 0;
}
