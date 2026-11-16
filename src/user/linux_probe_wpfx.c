/* src/user/linux_probe_wpfx.c - Linux-ABI probe: the path shapes a Wine
 * prefix on the compat tmpfs needs (lane C2-wine2, Gate P2).
 *
 * Wine reaches every C:\ file through dosdevices/c: -> ../drive_c (a RELATIVE
 * symlink in the middle of the path), and its NT file layer opens and stats
 * relative to directory fds (openat/fstatat/fchdir).  A plain musl program:
 * the reference on a Linux host, the measurement in HobbyOS.  Rows "[LP] ...",
 * last row "[LP] done rc=<failures>".
 *
 *   P1 relative symlink mid-path: open, stat, lstat, readdir through c:
 *   P2 openat/fstatat/faccessat relative to a dir fd, through the link
 *   P3 the same with an absolute link (z: -> /)
 *   P4 realpath-style readlink chain; O_NOFOLLOW on the link itself
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static char R[96];

static void out(const char *fmt, ...) {
  char b[384];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static const char *P(const char *leaf) {
  static char b[4][256];
  static int k;
  k = (k + 1) & 3;
  snprintf(b[k], sizeof b[k], "%s/%s", R, leaf);
  return b[k];
}

int main(void) {
  struct stat st;
  snprintf(R, sizeof R, "/tmp/wpfx-%d", (int)getpid());
  ck(mkdir(R, 0755) == 0, "setup mkdir root", errno);
  mkdir(P("drive_c"), 0755);
  mkdir(P("drive_c/windows"), 0755);
  mkdir(P("drive_c/windows/system32"), 0755);
  mkdir(P("dosdevices"), 0755);
  int fd = open(P("drive_c/windows/system32/kernel32.dll"), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0 && write(fd, "MZ-probe", 8) == 8, "setup file", errno);
  if (fd >= 0) close(fd);
  ck(symlink("../drive_c", P("dosdevices/c:")) == 0, "setup symlink c: -> ../drive_c", errno);
  ck(symlink("/", P("dosdevices/z:")) == 0, "setup symlink z: -> /", errno);

  /* P1 */
  const char *via = P("dosdevices/c:/windows/system32/kernel32.dll");
  fd = open(via, O_RDONLY);
  char b[16] = {0};
  ck(fd >= 0 && read(fd, b, 8) == 8 && !memcmp(b, "MZ-probe", 8), "P1 open+read through relative link", fd < 0 ? errno : 0);
  if (fd >= 0) close(fd);
  ck(stat(via, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 8, "P1 stat through link", errno);
  ck(lstat(via, &st) == 0 && S_ISREG(st.st_mode), "P1 lstat (link mid-path is followed)", errno);
  ck(stat(P("dosdevices/c:"), &st) == 0 && S_ISDIR(st.st_mode), "P1 stat the link -> dir", errno);
  ck(lstat(P("dosdevices/c:"), &st) == 0 && S_ISLNK(st.st_mode), "P1 lstat the link -> link", errno);
  DIR *d = opendir(P("dosdevices/c:/windows/system32"));
  int seen = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) if (!strcmp(e->d_name, "kernel32.dll")) seen = 1;
  if (d) closedir(d);
  ck(seen, "P1 readdir through link", errno);

  /* P2 */
  int dd = open(P("dosdevices"), O_RDONLY | O_DIRECTORY);
  ck(dd >= 0, "P2 open dosdevices O_DIRECTORY", errno);
  if (dd >= 0) {
    int f2 = openat(dd, "c:/windows/system32/kernel32.dll", O_RDONLY);
    ck(f2 >= 0, "P2 openat(dirfd, c:/...)", f2 < 0 ? errno : 0);
    if (f2 >= 0) close(f2);
    ck(fstatat(dd, "c:/windows/system32/kernel32.dll", &st, 0) == 0 && st.st_size == 8,
       "P2 fstatat(dirfd, c:/...)", errno);
    ck(fstatat(dd, "c:", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode),
       "P2 fstatat(dirfd, c:, NOFOLLOW) is a link", errno);
    ck(faccessat(dd, "c:/windows", R_OK, 0) == 0, "P2 faccessat(dirfd, c:/windows)", errno);
    int sd = openat(dd, "c:/windows/system32", O_RDONLY | O_DIRECTORY);
    ck(sd >= 0, "P2 openat dir through link", sd < 0 ? errno : 0);
    if (sd >= 0) {
      ck(fstatat(sd, "kernel32.dll", &st, 0) == 0, "P2 fstatat(subdir fd, leaf)", errno);
      int f3 = openat(sd, "kernel32.dll", O_RDONLY);
      ck(f3 >= 0, "P2 openat(subdir fd, leaf)", f3 < 0 ? errno : 0);
      if (f3 >= 0) close(f3);
      ck(fchdir(sd) == 0 && open("kernel32.dll", O_RDONLY) >= 0, "P2 fchdir + relative open", errno);
      ck(chdir("/") == 0, "P2 chdir back", errno);
      close(sd);
    }
    close(dd);
  }

  /* P3 */
  ck(stat(P("dosdevices/z:/tmp"), &st) == 0 && S_ISDIR(st.st_mode), "P3 absolute link z: -> / mid-path", errno);
  char zp[300];
  snprintf(zp, sizeof zp, "%s/dosdevices/z:%s/drive_c/windows/system32/kernel32.dll", R, R);
  ck(stat(zp, &st) == 0 && st.st_size == 8, "P3 back into /tmp through z:", errno);

  /* P4 */
  char lb[64] = {0};
  ck(readlink(P("dosdevices/c:"), lb, sizeof lb - 1) == 10 && !strcmp(lb, "../drive_c"), "P4 readlink c:", errno);
  int nf = open(P("dosdevices/c:"), O_RDONLY | O_NOFOLLOW);
  ck(nf == -1 && errno == ELOOP, "P4 O_NOFOLLOW on the link -> ELOOP", errno);
  if (nf >= 0) close(nf);

  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
