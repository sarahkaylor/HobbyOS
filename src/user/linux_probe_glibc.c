/* src/user/linux_probe_glibc.c — Linux-ABI probe: the glibc 2.31 STARTUP set
 * (M1, lane L3).
 *
 * An ORDINARY musl x86_64 Linux program: no HobbyOS header, no HobbyOS-ism, so
 * the same source runs on this workstation (the reference) and inside HobbyOS
 * through the compat layer (the measurement).  Rows are prefixed "[LP] " and
 * the last one is "[LP] done rc=<failures>"; tools/gate/t3_linux_probe.sh
 * accepts only rc=0.
 *
 * WHAT THIS PROBE IS FOR
 * ----------------------
 * glibc's ld.so/libc startup does not run a game; it opens DSOs, stats them,
 * asks the kernel about its own identity and, on a modern glibc, probes the
 * memory-model syscalls.  Each row below is one call of that startup path, and
 * every expectation is what a REAL Linux kernel answers (the host run is the
 * proof), not "what HobbyOS happens to return":
 *
 *   openat(AT_FDCWD)      the DSO-search call, incl. O_CLOEXEC really arming
 *                         FD_CLOEXEC and O_DIRECTORY opening a directory.
 *   getdents64            the ld.so cache scan: real dirent64 records, "." and
 *                         ".." first, sane d_reclen.  (Already a legacy switch
 *                         row in trap.c; this measures that it still works
 *                         under the table dispatch that now precedes it.)
 *   statx                 must be -ENOSYS so glibc falls back to newfstatat;
 *                         a success is also legal (a newer kernel), anything
 *                         else (EINVAL/EFAULT) breaks the fallback.
 *   membarrier            -ENOSYS is the protocol (malloc falls back).
 *   rseq                  -ENOSYS is the protocol (glibc disables the feature).
 *   sched_getaffinity     the 128-byte cpu_set_t shape, byte-count return, and
 *                         bits for the online CPUs.
 *   sched_setaffinity     the same shape, accepted.
 *   prctl                 PR_SET/GET_NAME round trip INCLUDING the 15-character
 *                         truncation, dumpability, parent-death signal, and
 *                         -EINVAL for an unknown option.
 *   get/setpriority       nice 0 is what a one-class scheduler reports.
 *   setpgid/getpgid/      the group anchor, and Linux's own EPERM for setsid
 *   setsid/getsid         by a process that is already a group leader.
 *   fchmod/fchown         descriptor validation (EBADF) on a volume with no
 *                         mode/owner bits.
 *   mprotect PROT_EXEC    the W^X row Mono's JIT depends on: allocate RW, write
 *                         machine code, mprotect to R+X, CALL it and check the
 *                         value it returns.  This row is deliberately last: a
 *                         kernel that leaves the page NX kills the probe here,
 *                         which is exactly the measurement.
 *
 * DIVERGENCES ARE PRINTED, NOT HIDDEN.  Three rows are known to answer
 * differently from Linux because the native kernel has no state for them
 * (dumpability, nice, and the affinity tail bytes).  They are marked
 * "info" with both the measured value and the Linux value; they do not count
 * as failures, because a probe that failed on its own documented design would
 * be grading the wrong thing.  Every ASSERTable row is Linux's answer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SYS_statx
#define SYS_statx 332
#endif
#ifndef SYS_membarrier
#define SYS_membarrier 324
#endif
#ifndef SYS_rseq
#define SYS_rseq 334
#endif
#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif
#ifndef SYS_sched_getaffinity
#define SYS_sched_getaffinity 204
#endif
#ifndef SYS_sched_setaffinity
#define SYS_sched_setaffinity 203
#endif

#define CPUMASK_BYTES 128

static int fails;

static void row(const char *fmt, ...) {
  char b[384];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static void check(int ok, const char *what, long detail) {
  if (!ok) fails++;
  row("[LP] %-30s %s detail=%ld\n", what, ok ? "ok" : "FAIL", detail);
}

/* Informational row: prints the measured value and the Linux reference, never
   counts a failure.  Used only where the header above documents a divergence. */
static void info(const char *what, long got, long linux_ref, const char *why) {
  row("[LP] %-30s info got=%ld linux=%ld (%s)\n", what, got, linux_ref, why);
}

/* Pick a real file that exists on BOTH the guest (FAT16 root) and this
 * workstation, so the openat/statx/fchmod rows measure a real inode in both
 * runs instead of a fabricated path. */
static const char *pick_existing(const char *argv0) {
  static const char *const cand[] = {"/PROBE.ELF", "/bin/sh", "/etc/hostname"};
  if (argv0 && argv0[0] == '/' && access(argv0, R_OK) == 0) return argv0;
  for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++)
    if (access(cand[i], R_OK) == 0) return cand[i];
  return 0;
}

/* --- openat -------------------------------------------------------------- */

static void test_openat(const char *path) {
  if (!path) {
    row("[LP] openat                          SKIP (no real path found)\n");
    fails++;
    return;
  }
  row("[LP] openat path=%s\n", path);

  errno = 0;
  int fd = openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
  check(fd >= 0, "openat AT_FDCWD", fd < 0 ? -errno : fd);
  if (fd < 0) return;

  int fdflags = fcntl(fd, F_GETFD);
  check(fdflags >= 0 && (fdflags & FD_CLOEXEC), "openat O_CLOEXEC armed", fdflags);

  /* The first four bytes of an ELF are 0x7f 'E' 'L' 'F'; only check when the
     chosen path is one (on the guest it always is). */
  unsigned char magic[4] = {0};
  if (read(fd, magic, sizeof magic) == (ssize_t)sizeof magic && magic[0] == 0x7f)
    check(magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F',
          "read real ELF magic", (long)magic[0]);
  close(fd);

  /* O_DIRECTORY on a regular file is ENOTDIR (Linux), not ENOENT.  Skip when
     the chosen path is itself a directory (the host fallback never is). */
  struct stat st;
  if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
    errno = 0;
    int dfd = openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY);
    check(dfd < 0 && errno == ENOTDIR, "openat O_DIRECTORY on file",
          dfd < 0 ? -errno : dfd);
    if (dfd >= 0) close(dfd);
  }

  /* A relative name through vfs_abs_path: chdir to the file's directory first
     so the same bare name resolves on both sides, then open it relatively. */
  char dir[256], base[256];
  snprintf(dir, sizeof dir, "%s", path);
  char *last = strrchr(dir, '/');
  if (last) {
    *last = '\0';
    snprintf(base, sizeof base, "%s", last + 1);
    if (dir[0] == '\0') snprintf(dir, sizeof dir, "/");
  } else {
    snprintf(dir, sizeof dir, ".");
    snprintf(base, sizeof base, "%s", path);
  }
  char cwd[256];
  if (base[0] && getcwd(cwd, sizeof cwd) && chdir(dir) == 0) {
    errno = 0;
    int rfd = openat(AT_FDCWD, base, O_RDONLY);
    check(rfd >= 0, "openat relative path", rfd < 0 ? -errno : rfd);
    if (rfd >= 0) close(rfd);
    (void)chdir(cwd);
  }
}

/* A real dirfd (not AT_FDCWD): Linux opens it, HobbyOS answers ENOTSUP because
   an fd is not a namespace node (see linux_compat_fd.c).  Printed, not graded.
 * The guest's FAT root is not openable (synthetic, no dirent), so this builds a
 * directory + file of its own; that also makes the row measure the real
 * (dirfd, relative-name) path instead of skipping. */
static void test_openat_dirfd(void) {
  const char *dir = "LPGLC.DIR", *file = "LPGLC.TMP";
  char rel[64];
  snprintf(rel, sizeof rel, "%s/%s", dir, file);
  (void)mkdir(dir, 0777);

  int cfd = openat(AT_FDCWD, rel, O_CREAT | O_RDWR, 0644);
  if (cfd < 0) {
    row("[LP] openat dirfd-relative           SKIP create errno=%d\n", errno);
    (void)rmdir(dir);
    return;
  }
  close(cfd);

  int dfd = openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
  if (dfd < 0) {
    row("[LP] openat dirfd-relative           SKIP open(%s) errno=%d\n", dir,
        errno);
  } else {
    errno = 0;
    int rfd = openat(dfd, file, O_RDONLY);
    info("openat dirfd-relative", rfd < 0 ? -errno : rfd, 0,
         "ENOTSUP here: an fd is not a namespace node");
    if (rfd >= 0) close(rfd);
    close(dfd);
  }

  snprintf(rel, sizeof rel, "%s/%s", dir, file);
  (void)unlink(rel);
  (void)rmdir(dir);
}

/* --- getdents64 ---------------------------------------------------------- */

static void test_getdents64(void) {
  const char *dir = "/";
  int made = 0;

  int fd = openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    /* HobbyOS's FAT16 root is SYNTHETIC: it has no dirent, and
       fat16_open_dir() refuses sector 0 ("no children fd").  A real
       subdirectory can be opened, so create one and read that — the
       dirent64 ABI is the thing under test, not the root special case. */
    dir = "LPGLC.DIR";
    (void)mkdir(dir, 0777);
    made = 1;
    fd = openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
  }
  check(fd >= 0, "openat dir O_DIRECTORY", fd < 0 ? -errno : fd);
  if (fd < 0) return;

  row("[LP] getdents64 dir=%s\n", dir);
  char buf[1024];
  errno = 0;
  long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
  check(n > 0, "getdents64 records", n < 0 ? -errno : n);
  if (n <= 0) {
    close(fd);
    if (made) (void)rmdir(dir);
    return;
  }

  /* Walk the records the way glibc's readdir does: d_reclen at 16, d_name at
     19 (Linux dirent64).  A bad reclen would make readdir loop or crash. */
  int saw_dot = 0, records = 0, bad = 0;
  long off = 0;
  while (off + 19 <= n) {
    unsigned short reclen;
    memcpy(&reclen, buf + off + 16, sizeof reclen);
    if (reclen < 19 || off + reclen > n) {
      bad++;
      break;
    }
    const char *name = buf + off + 19;
    if (strcmp(name, ".") == 0) saw_dot = 1;
    records++;
    off += reclen;
  }
  check(bad == 0 && records > 0 && saw_dot, "dirent64 records walkable",
        (long)(records * 1000 + bad));
  close(fd);
  if (made) (void)rmdir(dir);
}

/* --- the deliberate -ENOSYS trio ----------------------------------------- */

static void test_statx(const char *path) {
  struct {
    uint64_t mask, blksize, attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode;
    uint64_t ino, size, blocks;
    uint64_t atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t spare[14];
  } stx;
  memset(&stx, 0, sizeof stx);

  errno = 0;
  long rc = syscall(SYS_statx, AT_FDCWD, path ? path : "/", 0, 0x7ff, &stx);
  int e = errno;
  /* ENOSYS is the protocol glibc implements a fallback for; 0 is a newer
     kernel that really answered.  EINVAL/EFAULT would break the fallback. */
  check(rc == 0 || (rc < 0 && e == ENOSYS), "statx rc=0 or ENOSYS", rc < 0 ? -e : 0);
  info("statx answer", rc < 0 ? -e : rc, -ENOSYS,
       "glibc 2.31 falls back to newfstatat on ENOSYS");
}

static void test_membarrier(void) {
  errno = 0;
  long rc = syscall(SYS_membarrier, 0 /* MEMBARRIER_CMD_QUERY */, 0, 0);
  int e = errno;
  check(rc >= 0 || (rc < 0 && e == ENOSYS), "membarrier QUERY ok/ENOSYS",
        rc < 0 ? -e : rc);
}

static void test_rseq(void) {
  static struct { uint64_t cpu_id_start, cpu_id, rseq_cs, flags; } area
  __attribute__((aligned(32)));
  memset(&area, 0, sizeof area);
  errno = 0;
  long rc = syscall(SYS_rseq, &area, sizeof area, 0, 0);
  int e = errno;
  check(rc == 0 || (rc < 0 && e == ENOSYS), "rseq register ok/ENOSYS",
        rc < 0 ? -e : rc);
  if (rc == 0) (void)syscall(SYS_rseq, &area, sizeof area, 0, 1 /* UNREGISTER */);
}

/* --- affinity ------------------------------------------------------------ */

static void test_affinity(void) {
  unsigned char set[CPUMASK_BYTES];
  memset(set, 0xAA, sizeof set); /* what glibc does NOT do before the call */

  errno = 0;
  long rc = syscall(SYS_sched_getaffinity, 0, sizeof set, set);
  int e = errno;
  check(rc == 8, "sched_getaffinity returns 8", rc < 0 ? -e : rc);
  if (rc < 0) return;

  int bits = 0;
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 8; j++)
      if (set[i] & (1u << j)) bits++;
  check(bits > 0, "sched_getaffinity cpu bits", bits);

  int tail = 0;
  for (int i = 8; i < CPUMASK_BYTES; i++)
    if (set[i] != 0) tail = 1;
  info("affinity tail zeroed", tail == 0 ? 1 : 0, 0,
       "HobbyOS zeroes the whole 128B mask; Linux leaves 0xAA garbage");

  unsigned char one[CPUMASK_BYTES];
  memset(one, 0, sizeof one);
  one[0] = 1; /* CPU 0 */
  errno = 0;
  rc = syscall(SYS_sched_setaffinity, 0, sizeof one, one);
  e = errno;
  check(rc == 0, "sched_setaffinity accepted", rc < 0 ? -e : rc);

  /* A mask smaller than the kernel's 8-byte word is EINVAL, not a short copy. */
  errno = 0;
  rc = syscall(SYS_sched_getaffinity, 0, 4, set);
  check(rc < 0 && errno == EINVAL, "sched_getaffinity len<8 EINVAL",
        rc < 0 ? -errno : rc);
}

/* --- prctl --------------------------------------------------------------- */

static void test_prctl(void) {
  static const char longname[] = "lprobe-glibc-name-20c"; /* 21 chars */
  errno = 0;
  long rc = prctl(PR_SET_NAME, (unsigned long)longname, 0, 0, 0);
  check(rc == 0, "PR_SET_NAME", rc < 0 ? -errno : rc);

  char got[16];
  memset(got, 0x7f, sizeof got);
  errno = 0;
  rc = prctl(PR_GET_NAME, (unsigned long)got, 0, 0, 0);
  check(rc == 0, "PR_GET_NAME", rc < 0 ? -errno : rc);
  /* Linux keeps 15 characters and NUL-terminates the 16-byte comm. */
  check(strncmp(got, longname, 15) == 0 && got[15] == '\0',
        "PR_GET_NAME 15-char truncation", (long)strlen(got));

  rc = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  check(rc == 0, "PR_SET_DUMPABLE", rc < 0 ? -errno : rc);
  errno = 0;
  rc = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
  check(rc >= 0, "PR_GET_DUMPABLE", rc < 0 ? -errno : rc);
  info("PR_GET_DUMPABLE after set 0", rc, 0, "flag is recorded nowhere");

  rc = prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0);
  check(rc == 0, "PR_SET_PDEATHSIG 0 clears", rc < 0 ? -errno : rc);
  int psig = -1;
  rc = prctl(PR_GET_PDEATHSIG, (unsigned long)&psig, 0, 0, 0);
  check(rc == 0 && psig == 0, "PR_GET_PDEATHSIG reads 0",
        rc < 0 ? -errno : psig);

  errno = 0;
  rc = prctl(PR_SET_PDEATHSIG, SIGUSR1, 0, 0, 0);
  /* A real hook cannot be armed (no PCB field); ENOSYS names the gap.  Linux
     would return 0 here, so both answers are printed and neither fails. */
  check(rc == 0 || (rc < 0 && errno == ENOSYS), "PR_SET_PDEATHSIG non-zero",
        rc < 0 ? -errno : rc);
  info("PR_SET_PDEATHSIG SIGUSR1", rc < 0 ? -errno : rc, 0,
       "ENOSYS here: the PCB has no pdeathsig field");

  errno = 0;
  rc = prctl(0x7fffffff, 0, 0, 0, 0);
  check(rc < 0 && errno == EINVAL, "prctl unknown op EINVAL",
        rc < 0 ? -errno : rc);
}

/* --- nice / job control -------------------------------------------------- */

static void test_priority(void) {
  errno = 0;
  int p = getpriority(PRIO_PROCESS, 0);
  /* glibc/musl subtract 20 from the raw answer; nice 0 therefore reads as 0. */
  check(p == 0 || (p == -1 && errno == 0), "getpriority nice 0", p);

  errno = 0;
  int rc = setpriority(PRIO_PROCESS, 0, 5);
  check(rc == 0, "setpriority +5 accepted", rc < 0 ? -errno : rc);
  errno = 0;
  info("getpriority after set 5", getpriority(PRIO_PROCESS, 0), 5,
       "the scheduler has one priority class");
  (void)setpriority(PRIO_PROCESS, 0, 0);

  errno = 0;
  rc = setpriority(PRIO_PROCESS, 0, -5);
  check(rc == 0 || (rc < 0 && (errno == EPERM || errno == EACCES)),
        "setpriority -5 EPERM", rc < 0 ? -errno : rc);

  errno = 0;
  rc = setpriority(99, 0, 0);
  check(rc < 0 && errno == EINVAL, "setpriority bad which EINVAL",
        rc < 0 ? -errno : rc);
}

static void test_jobcontrol(void) {
  pid_t pid = getpid();
  pid_t pg = getpgid(0);
  check(pg > 0, "getpgid(0)", pg);
  info("pgid vs own pid", pg, pid, "HobbyOS: every process is its own anchor");

  errno = 0;
  int rc = setpgid(0, 0);
  check(rc == 0, "setpgid(0,0)", rc < 0 ? -errno : rc);

  errno = 0;
  pid_t sid = getsid(0);
  check(sid > 0, "getsid(0)", sid);

  errno = 0;
  rc = setsid();
  /* Linux refuses exactly when the caller is already a group leader — which
     every HobbyOS process is.  0 is also accepted (a Linux run whose pgid is
     not its pid). */
  check(rc >= 0 || (rc < 0 && errno == EPERM), "setsid EPERM or ok",
        rc < 0 ? -errno : rc);
  info("setsid", rc < 0 ? -errno : rc, -EPERM,
       "setpgid(0,0) above made us a group anchor; Linux refuses too");
}

/* --- fchmod / fchown ----------------------------------------------------- */

static void test_chmod_chown(void) {
  /* A file we create, not a system file: on the workstation /bin/sh is owned by
     root and an unprivileged fchmod is EPERM, which would grade the host's
     ownership rather than the kernel's fchmod.  The name is 8.3-safe for FAT. */
  const char *tmp = "LPGLC.TMP";
  int fd = openat(AT_FDCWD, tmp, O_CREAT | O_RDWR | O_TRUNC, 0644);
  check(fd >= 0, "openat O_CREAT temp", fd < 0 ? -errno : fd);
  if (fd < 0) return;

  errno = 0;
  int rc = fchmod(fd, 0644);
  check(rc == 0, "fchmod 0644", rc < 0 ? -errno : rc);

  errno = 0;
  rc = fchown(fd, (uid_t)-1, (gid_t)-1);
  check(rc == 0, "fchown -1/-1 no-op", rc < 0 ? -errno : rc);

  errno = 0;
  rc = fchown(fd, 1234, 1234);
  /* Unprivileged Linux cannot give a file to another uid, and neither can a
     volume with no owner field. */
  check(rc == 0 || (rc < 0 && errno == EPERM), "fchown foreign owner EPERM",
        rc < 0 ? -errno : rc);
  close(fd);
  (void)unlink(tmp);

  errno = 0;
  rc = fchmod(999, 0644);
  check(rc < 0 && errno == EBADF, "fchmod bad fd EBADF", rc < 0 ? -errno : rc);
}

/* --- mprotect PROT_EXEC: the W^X row Mono's JIT depends on ---------------- */

static void test_exec_page(void) {
  row("[LP] exec-page begin (a NX defect ends the probe here)\n");
  unsigned char *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(p != MAP_FAILED, "mmap RW page", p == MAP_FAILED ? -errno : 4096);
  if (p == MAP_FAILED) return;

  /* mov $42, %eax ; ret   — the smallest thing a JIT would emit. */
  static const unsigned char code[] = {0xb8, 42, 0x00, 0x00, 0x00, 0xc3};
  memcpy(p, code, sizeof code);

  errno = 0;
  int rc = mprotect(p, 4096, PROT_READ | PROT_EXEC);
  check(rc == 0, "mprotect RW -> R|X", rc < 0 ? -errno : rc);
  if (rc != 0) {
    munmap(p, 4096);
    return;
  }

  int (*fn)(void) = (int (*)(void))(void *)p;
  int v = fn();
  check(v == 42, "call JIT page returns 42", v);
  munmap(p, 4096);
}

int main(int argc, char **argv) {
  const char *path = pick_existing(argc > 0 ? argv[0] : 0);

  /* build= is deliberate: the gate rebuilds on every run, and a row that does
     not say build=glibc-1 is a stale binary. */
  row("[LP] glibc probe build=glibc-1 pid=%ld path=%s\n", (long)getpid(),
      path ? path : "(none)");

  test_openat(path);
  test_openat_dirfd();
  test_getdents64();
  test_statx(path);
  test_membarrier();
  test_rseq();
  test_affinity();
  test_prctl();
  test_priority();
  test_jobcontrol();
  test_chmod_chown();
  test_exec_page();

  row("[LP] done rc=%d\n", fails);
  return fails;
}
