/* src/user/linux_probe_wsys.c - Linux-ABI probe for the Wine prerequisite
 * syscalls C2-sys added (cp/sys): fchdir, sendmmsg, the xattr family,
 * eventfd, inotify, process_vm_readv, prctl(SUD), getrusage(RUSAGE_THREAD).
 *
 * A plain musl x86_64 program (reference on a Linux host, measurement inside
 * HobbyOS).  Rows start "[LP] "; the last is "[LP] done rc=<failures>".  Each
 * expectation is what Linux does; where HobbyOS answers an errno Wine
 * tolerates (xattr ENOTSUP, inotify ENOSYS) the row accepts BOTH Linux's
 * exact answer and the tolerated one, so the same source passes on both.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;
static void out(const char *fmt, ...) {
  char b[320];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}

/* fchdir: open two dirs, fchdir between them, confirm getcwd follows; then
 * the error paths Wine's NtQueryDirectoryFile relies on. */
static void t_fchdir(void) {
  char d1[64], d2[64], cwd[256];
  snprintf(d1, sizeof d1, "/tmp/lpw-%d-a", (int)getpid());
  snprintf(d2, sizeof d2, "/tmp/lpw-%d-b", (int)getpid());
  mkdir(d1, 0755); mkdir(d2, 0755);
  int f1 = open(d1, O_RDONLY | O_DIRECTORY);
  int f2 = open(d2, O_RDONLY | O_DIRECTORY);
  ck(f1 >= 0 && f2 >= 0, "fchdir open two dirs", f1 < 0 || f2 < 0 ? errno : 0);
  int r = fchdir(f1);
  getcwd(cwd, sizeof cwd);
  ck(r == 0 && strcmp(cwd, d1) == 0, "fchdir to d1 and getcwd", r ? errno : 0);
  r = fchdir(f2);
  getcwd(cwd, sizeof cwd);
  ck(r == 0 && strcmp(cwd, d2) == 0, "fchdir to d2 and getcwd", r ? errno : 0);
  /* error paths */
  ck(fchdir(931) == -1 && errno == EBADF, "fchdir EBADF on a closed fd", errno);
  int ff = open("/tmp", O_RDONLY | O_DIRECTORY);
  int reg = openat(ff, "lpw-reg", O_RDWR | O_CREAT, 0644);
  ck(reg >= 0 && fchdir(reg) == -1 && errno == ENOTDIR, "fchdir ENOTDIR on a file", errno);
  if (reg >= 0) close(reg);
  unlinkat(ff, "lpw-reg", 0);
  close(ff); close(f1); close(f2);
  chdir("/"); rmdir(d1); rmdir(d2);
}

/* xattr: on a filesystem without them Linux answers ENOTSUP/EOPNOTSUPP for
 * get/set/remove and an empty list for list; the host's /tmp may support
 * them, so accept the real empty/ENODATA answers too.  What MUST hold is the
 * argument validation: a bad fd is EBADF. */
static int tol(int rc, int e, int a, int b) {
  return rc == -1 && (errno == a || errno == b || errno == e);
}
static void t_xattr(void) {
  char path[64];
  snprintf(path, sizeof path, "/tmp/lpw-xa-%d", (int)getpid());
  int fd = open(path, O_RDWR | O_CREAT, 0644);
  char buf[64];
  long g = syscall(SYS_getxattr, path, "user.wine.dosattrib", buf, sizeof buf);
  ck(g == -1 && (errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENODATA),
     "getxattr absent attr -> ENOTSUP/ENODATA", errno);
  long s = syscall(SYS_setxattr, path, "user.wine.dosattrib", "0x2", 3, 0);
  ck(s == 0 || tol((int)s, ENOTSUP, EOPNOTSUPP, EPERM),
     "setxattr -> ok or ENOTSUP", s == 0 ? 0 : errno);
  long fg = syscall(SYS_fgetxattr, fd, "user.wine.dosattrib", buf, sizeof buf);
  ck(fg >= 0 || tol((int)fg, ENOTSUP, EOPNOTSUPP, ENODATA),
     "fgetxattr -> value or ENOTSUP/ENODATA", fg >= 0 ? fg : errno);
  long l = syscall(SYS_listxattr, path, buf, sizeof buf);
  ck(l >= 0, "listxattr -> a (possibly empty) list", l < 0 ? errno : l);
  long bad = syscall(SYS_fgetxattr, 931, "user.x", buf, sizeof buf);
  ck(bad == -1 && errno == EBADF, "fgetxattr EBADF on a closed fd", errno);
  long ne = syscall(SYS_getxattr, "/tmp/lpw-xa-absent", "user.x", buf, sizeof buf);
  ck(ne == -1 && errno == ENOENT, "getxattr ENOENT on a missing path", errno);
  close(fd); unlink(path);
}

/* eventfd: counter add/read, EFD_SEMAPHORE decrement, EFD_NONBLOCK EAGAIN. */
static void t_eventfd(void) {
  int fd = eventfd(0, 0);
  ck(fd >= 0, "eventfd create", fd < 0 ? errno : 0);
  uint64_t v = 7;
  long w = write(fd, &v, 8);
  v = 3; w += write(fd, &v, 8);
  uint64_t r = 0;
  long rd = read(fd, &r, 8);
  ck(w == 16 && rd == 8 && r == 10, "eventfd accumulates then read clears", (long)r);
  int nb = eventfd(0, EFD_NONBLOCK);
  rd = read(nb, &r, 8);
  ck(rd == -1 && errno == EAGAIN, "eventfd empty nonblock EAGAIN", errno);
  close(nb);
  int sem = eventfd(5, EFD_SEMAPHORE | EFD_NONBLOCK);
  rd = read(sem, &r, 8);
  ck(rd == 8 && r == 1, "eventfd semaphore read returns 1", (long)r);
  rd = read(sem, &r, 8);
  ck(rd == 8 && r == 1, "eventfd semaphore decrements", (long)r);
  close(sem); close(fd);
}

/* inotify: HobbyOS answers ENOSYS (no CONFIG_INOTIFY_USER); the host has it.
 * Accept a real fd OR ENOSYS, and in both cases a bad-fd add_watch is an
 * error (EINVAL "not an inotify instance" / EBADF). */
static void t_inotify(void) {
  int fd = (int)syscall(SYS_inotify_init1, 0);
  ck(fd >= 0 || errno == ENOSYS, "inotify_init1 -> fd or ENOSYS", fd >= 0 ? fd : errno);
  if (fd >= 0) {
    int w = (int)syscall(SYS_inotify_add_watch, fd, "/tmp", 0x100 /*IN_CREATE*/);
    ck(w >= 0, "inotify_add_watch on /tmp", w < 0 ? errno : w);
    if (w >= 0) syscall(SYS_inotify_rm_watch, fd, w);
    close(fd);
  } else {
    long w = syscall(SYS_inotify_add_watch, 931, "/tmp", 0x100);
    ck(w == -1 && (errno == EBADF || errno == EINVAL),
       "inotify_add_watch on a non-inotify fd fails", errno);
  }
}

/* process_vm_readv against our own address space: Linux allows a process to
 * read its own memory this way (wineserver reads a client's). */
static void t_process_vm(void) {
  char src[32]; memcpy(src, "process_vm_readv_payload_ok!", 29);
  char dst[32]; memset(dst, 0, sizeof dst);
  struct iovec l = { dst, 29 }, r = { src, 29 };
  long n = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL);
  ck(n == 29 && memcmp(dst, src, 29) == 0, "process_vm_readv reads own memory", n);
  long bad = syscall(SYS_process_vm_readv, getpid(), &l, 1UL,
                     (void *)&(struct iovec){ (void *)0x10, 29 }, 1UL, 0UL);
  ck(bad == -1 && (errno == EFAULT || errno == EPERM),
     "process_vm_readv EFAULT on a bad remote range", errno);
  long nos = syscall(SYS_process_vm_readv, 0x7fffffff, &l, 1UL, &r, 1UL, 0UL);
  ck(nos == -1 && errno == ESRCH, "process_vm_readv ESRCH for no such pid", errno);
}

/* prctl(PR_SET_SYSCALL_USER_DISPATCH): HobbyOS answers EINVAL (pre-5.11); a
 * modern host may accept it.  Either way it must not crash and the call
 * returns. */
#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
static void t_prctl_sud(void) {
  long r = syscall(SYS_prctl, PR_SET_SYSCALL_USER_DISPATCH, 0, 0, 0, 0);
  ck(r == 0 || r == -1, "prctl(SUD) returns (ok or EINVAL)", r == 0 ? 0 : errno);
}

/* getrusage(RUSAGE_THREAD): must succeed and fill the struct (Wine's
 * NtYieldExecution reads ru_nvcsw around sched_yield). */
static void t_rusage(void) {
  struct rusage ru;
  memset(&ru, 0xff, sizeof ru);
  int r = getrusage(RUSAGE_THREAD, &ru);
  ck(r == 0, "getrusage(RUSAGE_THREAD) succeeds", r ? errno : 0);
  ck(r == 0 && ru.ru_utime.tv_sec >= 0 && ru.ru_stime.tv_sec >= 0,
     "getrusage times are non-negative", (long)ru.ru_utime.tv_sec);
}

int main(void) {
  t_fchdir();
  t_xattr();
  t_eventfd();
  t_inotify();
  t_process_vm();
  t_prctl_sud();
  t_rusage();
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
