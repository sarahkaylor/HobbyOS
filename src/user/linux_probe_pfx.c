/* src/user/linux_probe_pfx.c - Linux-ABI probe: the file-system operations a
 * Wine prefix makes, on the ext2 volume (lane C3-pfx, docs/cyberpunk.md §6 P3,
 * docs/cp-pfx.md).  Each row names the Wine 11.0 code it stands for.
 *
 * A plain musl x86_64 program: the reference on a Linux host and the
 * measurement in HobbyOS.  argv[1] is the volume (default /EXT; every row is
 * skipped when it is absent, so the probe is harmless in a boot without the
 * data disk).  Rows "[LP] ...", last row "[LP] done rc=<fails>".
 *
 *   X1 dlls/ntdll/unix/server.c setup_config_dir: mkdir prefix, chdir in, then
 *      relative mkdir drive_c/dosdevices, symlink ../drive_c -> c:, / -> z:
 *   X2 server/registry.c save_branch: open(O_WRONLY)+lstat of the old file,
 *      "reg<pid><n>.tmp" O_CREAT|O_EXCL (2nd is EEXIST), stdio write, rename
 *      over system.reg; an fd opened before the rename keeps the old bytes
 *   X3 server/fd.c set_unix_lock / server/request.c: fcntl F_SETLK/F_GETLK
 *      between two processes, l_pid, EAGAIN, release on close, F_SETLKW wake
 *   X4 ftruncate grow + shrink, fsync, fdatasync, st_size/st_blocks
 *   X5 server/registry.c periodic_save: fchdir(config_dir_fd) + relative
 *      open + fchdir(server_dir_fd) round trips; getcwd
 *   X6 ntdll's NT path layer: openat/fstatat/mkdirat/renameat/unlinkat/
 *      symlinkat/readlinkat relative to dir fds, through dosdevices/c:
 *   X7 a PE-like file under drive_c mapped MAP_PRIVATE RW + PROT_EXEC view;
 *      read-through of a later write(); MAP_SHARED write-back reaches read()
 *   X8 utimensat/futimens, mtime survives reopen, write() moves mtime,
 *      read() does not
 *   X9 statfs/fstatfs (f_type 0xEF53 on ext2); fgetxattr/fsetxattr
 *      "user.DOSATTRIB" (info: Wine accepts ENOTSUP/ENODATA)
 *   X10 a system32-sized directory: 900 entries created, listed with
 *       getdents64, renamed, unlinked
 *   X11 a fake-DLL install: 2 MiB copy with read()/write() in 64 KiB pieces,
 *       compared byte for byte
 *   X12 rename of a directory over an empty one; rename of an open file;
 *       rmdir of a non-empty dir is ENOTEMPTY
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails;
static char R[200];

static void out(const char *fmt, ...) {
  char b[512];
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
  static char b[4][400];
  static int k;
  k = (k + 1) & 3;
  snprintf(b[k], sizeof b[k], "%s/%s", R, leaf);
  return b[k];
}
static int put(const char *path, const char *s) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  ssize_t n = write(fd, s, strlen(s));
  close(fd);
  return n == (ssize_t)strlen(s) ? 0 : -1;
}
static int has(const char *path, const char *s) {
  char b[256] = {0};
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  return n == (ssize_t)strlen(s) && !memcmp(b, s, (size_t)n);
}

static void x1_setup(void) {
  ck(mkdir(R, 0777) == 0, "X1 mkdir prefix", errno);
  ck(chdir(R) == 0, "X1 chdir into the prefix", errno);
  ck(mkdir("drive_c", 0777) == 0 && mkdir("dosdevices", 0777) == 0, "X1 relative mkdir drive_c, dosdevices", errno);
  ck(symlink("../drive_c", "dosdevices/c:") == 0, "X1 relative symlink dosdevices/c: -> ../drive_c", errno);
  ck(symlink("/", "dosdevices/z:") == 0, "X1 relative symlink dosdevices/z: -> /", errno);
  ck(mkdir("dosdevices/c:/windows", 0777) == 0 && mkdir("drive_c/windows/system32", 0777) == 0,
     "X1 mkdir through c: and directly", errno);
  struct stat st;
  ck(stat(R, &st) == 0 && st.st_uid == getuid(), "X1 prefix owned by the caller (Wine refuses otherwise)",
     (long)st.st_uid);
  ck(stat("dosdevices/z:/", &st) == 0 && S_ISDIR(st.st_mode), "X1 z: reaches /", errno);
}

static void x2_registry(void) {
  ck(put("system.reg", "WINE REGISTRY Version 2\n;; v1\n") == 0, "X2 first system.reg", errno);
  int old = open("system.reg", O_RDONLY);
  struct stat st;
  int fd = open("system.reg", O_WRONLY);
  ck(fd >= 0 && lstat("system.reg", &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1,
     "X2 open(O_WRONLY) + lstat: regular, nlink 1 -> temp-file path", (long)st.st_nlink);
  if (fd >= 0) close(fd);
  char tmp[32];
  snprintf(tmp, sizeof tmp, "reg%lx%04x.tmp", (long)getpid(), 0);
  fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0666);
  ck(fd >= 0, "X2 O_CREAT|O_EXCL temp", fd < 0 ? errno : 0);
  int fd2 = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0666);
  ck(fd2 == -1 && errno == EEXIST, "X2 2nd O_EXCL is EEXIST", errno);
  FILE *f = fd >= 0 ? fdopen(fd, "w") : 0;
  ck(f != 0, "X2 fdopen", errno);
  if (f) {
    for (int i = 0; i < 2000; i++) fprintf(f, "[Software\\\\Key%04d] 1700000000\n\"v\"=dword:%08x\n", i, i);
    ck(fclose(f) == 0, "X2 fclose (stdio flush, ~110 KB)", errno);
  }
  ck(rename(tmp, "system.reg") == 0, "X2 rename temp over system.reg", errno);
  int sr = stat("system.reg", &st);
  ck(sr == 0 && st.st_size >= 90000 && st.st_nlink == 1, "X2 new system.reg size", (long)st.st_size);
  ck(access(tmp, F_OK) == -1 && errno == ENOENT, "X2 temp name gone", errno);
  char b[40] = {0};
  ck(old >= 0 && pread(old, b, 30, 0) == 30 && !memcmp(b, "WINE REGISTRY Version 2\n;; v1\n", 30),
     "X2 fd opened before the rename still reads the old bytes", old);
  if (old >= 0) close(old);
  /* the same again (wineserver saves each branch periodically) */
  ck(put("rtmp", "again\n") == 0 && rename("rtmp", "system.reg") == 0 && has("system.reg", "again\n"),
     "X2 a second rename over it", errno);
}

static void x3_locks(void) {
  int fd = open("system.reg", O_RDWR);
  ck(fd >= 0, "X3 open system.reg O_RDWR", errno);
  if (fd < 0) return;
  struct flock l = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
  ck(fcntl(fd, F_SETLK, &l) == 0, "X3 F_SETLK F_WRLCK [0,1)", errno);
  int pp[2], qq[2];
  (void)!pipe(pp);
  (void)!pipe(qq);
  pid_t me = getpid();
  pid_t c = fork();
  if (c == 0) {
    int cfd = open("system.reg", O_RDWR);
    struct flock g = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    int bad = 0;
    if (fcntl(cfd, F_GETLK, &g) != 0 || g.l_type != F_WRLCK || g.l_pid != me) bad |= 1;
    struct flock s = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    if (fcntl(cfd, F_SETLK, &s) != -1 || (errno != EAGAIN && errno != EACCES)) bad |= 2;
    struct flock o = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 5};
    if (fcntl(cfd, F_SETLK, &o) != 0) bad |= 4; /* a disjoint range is free */
    (void)!write(qq[1], "r", 1);                /* ready: now block on [0,1) */
    if (fcntl(cfd, F_SETLKW, &s) != 0) bad |= 8;
    _exit(bad);
  }
  char r;
  (void)!read(qq[0], &r, 1);
  usleep(100000);
  struct flock g = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 1};
  ck(fcntl(fd, F_GETLK, &g) == 0 && g.l_type == F_WRLCK && g.l_pid == c, "X3 F_GETLK reports the child's range",
     (long)g.l_pid);
  close(fd); /* POSIX: closing any fd releases the process's locks; wakes the child */
  int st = 0;
  waitpid(c, &st, 0);
  ck(WIFEXITED(st) && WEXITSTATUS(st) == 0, "X3 child: GETLK l_pid, EAGAIN, disjoint ok, SETLKW woken by close",
     WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  close(pp[0]); close(pp[1]); close(qq[0]); close(qq[1]);
}

static void x4_trunc(void) {
  int fd = open("trunc.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
  struct stat st;
  ck(fd >= 0 && ftruncate(fd, 1 << 20) == 0 && fstat(fd, &st) == 0 && st.st_size == (1 << 20),
     "X4 ftruncate grow to 1 MiB", (long)st.st_size);
  ck(pwrite(fd, "tail", 4, (1 << 20) - 4) == 4 && fsync(fd) == 0 && fdatasync(fd) == 0, "X4 pwrite + fsync + fdatasync",
     errno);
  ck(ftruncate(fd, 5000) == 0 && fstat(fd, &st) == 0 && st.st_size == 5000, "X4 ftruncate shrink to 5000",
     (long)st.st_size);
  char b[8] = {1, 1, 1, 1};
  ck(ftruncate(fd, 9000) == 0 && pread(fd, b, 4, 6000) == 4 && !b[0] && !b[3], "X4 regrow reads zeros", b[0]);
  ck(fstat(fd, &st) == 0 && st.st_blocks <= 32, "X4 st_blocks small after shrink (sparse regrow)", (long)st.st_blocks);
  close(fd);
}

static void x5_fchdir(void) {
  int cfg = open(R, O_RDONLY | O_DIRECTORY);
  int srv = open("/", O_RDONLY | O_DIRECTORY);
  ck(cfg >= 0 && srv >= 0, "X5 open config dir + server dir fds", errno);
  int ok = 1;
  for (int i = 0; i < 50 && ok; i++) {
    if (fchdir(srv) != 0 || fchdir(cfg) != 0) ok = 0;
    int fd = open("system.reg", O_RDONLY);
    if (fd < 0) ok = 0; else close(fd);
  }
  ck(ok, "X5 50 x fchdir(server) / fchdir(config) + relative open", errno);
  char cwd[300] = {0};
  ck(getcwd(cwd, sizeof cwd) && !strcmp(cwd, R), "X5 getcwd after fchdir", errno);
  if (cfg >= 0) close(cfg);
  if (srv >= 0) close(srv);
}

static void x6_at(void) {
  int dd = open("dosdevices", O_RDONLY | O_DIRECTORY);
  ck(dd >= 0, "X6 open dosdevices O_DIRECTORY", errno);
  if (dd < 0) return;
  int fd = openat(dd, "c:/windows/system32/kernel32.dll", O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0 && write(fd, "MZ\x90\0kernel32", 12) == 12, "X6 openat create through c:", fd < 0 ? errno : 0);
  if (fd >= 0) close(fd);
  struct stat st;
  ck(fstatat(dd, "c:/windows/system32/kernel32.dll", &st, 0) == 0 && st.st_size == 12, "X6 fstatat through c:",
     errno);
  int sd = openat(dd, "c:/windows/system32", O_RDONLY | O_DIRECTORY);
  ck(sd >= 0, "X6 openat dir through c:", errno);
  if (sd >= 0) {
    ck(mkdirat(sd, "drivers", 0777) == 0, "X6 mkdirat", errno);
    ck(renameat(sd, "kernel32.dll", sd, "drivers/k32.dll") == 0, "X6 renameat into a subdir", errno);
    ck(symlinkat("drivers/k32.dll", sd, "k32link") == 0, "X6 symlinkat", errno);
    char lb[64] = {0};
    ck(readlinkat(sd, "k32link", lb, sizeof lb - 1) == 15 && !strcmp(lb, "drivers/k32.dll"), "X6 readlinkat", errno);
    ck(fstatat(sd, "k32link", &st, 0) == 0 && st.st_size == 12, "X6 fstatat follows the link", errno);
    ck(unlinkat(sd, "k32link", 0) == 0 && renameat(sd, "drivers/k32.dll", sd, "kernel32.dll") == 0,
       "X6 unlinkat + renameat back", errno);
    ck(unlinkat(sd, "drivers", AT_REMOVEDIR) == 0, "X6 unlinkat AT_REMOVEDIR", errno);
    ck(faccessat(sd, "kernel32.dll", R_OK | W_OK, 0) == 0, "X6 faccessat", errno);
    close(sd);
  }
  close(dd);
}

static void x7_map(void) {
  int fd = open("drive_c/windows/system32/pe.dll", O_RDWR | O_CREAT | O_TRUNC, 0755);
  static unsigned char img[3 * 4096];
  memset(img, 0xCC, sizeof img);
  memcpy(img, "MZ", 2);
  img[4096] = 0xB8; img[4097] = 42; img[4098] = 0; img[4099] = 0; img[4100] = 0; img[4101] = 0xC3; /* mov eax,42; ret */
  ck(fd >= 0 && write(fd, img, sizeof img) == (ssize_t)sizeof img, "X7 write a 3-page PE-like file", errno);
  if (fd < 0) return;
  volatile unsigned char *p = mmap(0, sizeof img, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED && p[0] == 'M', "X7 MAP_PRIVATE RW view", p == MAP_FAILED ? errno : 0);
  void *x = mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 4096);
  ck(x != MAP_FAILED && ((int (*)(void))x)() == 42, "X7 PROT_EXEC view runs the file's code", x == MAP_FAILED ? errno : 0);
  if (p != MAP_FAILED) {
    (void)!pwrite(fd, "ZZ", 2, 2 * 4096 + 1);
    ck(p[2 * 4096 + 1] == 'Z', "X7 private view reads a later write() through", p[2 * 4096 + 1]);
    p[10] = 0x11;
    unsigned char c = 0;
    ck(pread(fd, &c, 1, 10) == 1 && c == 0xCC, "X7 private store does not reach the file", c);
    munmap((void *)p, sizeof img);
  }
  if (x != MAP_FAILED) munmap(x, 4096);
  volatile unsigned char *s = mmap(0, sizeof img, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (s != MAP_FAILED) {
    s[20] = 0x77;
    msync((void *)s, sizeof img, MS_SYNC);
    unsigned char c = 0;
    ck(pread(fd, &c, 1, 20) == 1 && c == 0x77, "X7 MAP_SHARED store reaches read() after msync", c);
    munmap((void *)s, sizeof img);
  } else ck(0, "X7 MAP_SHARED RW view", errno);
  close(fd);
}

static void x8_times(void) {
  ck(put("tstamp", "x\n") == 0, "X8 file", errno);
  struct timespec ts[2] = {{1000000000, 0}, {1234567890, 0}};
  ck(utimensat(AT_FDCWD, "tstamp", ts, 0) == 0, "X8 utimensat", errno);
  struct stat st;
  ck(stat("tstamp", &st) == 0 && st.st_mtime == 1234567890 && st.st_atime == 1000000000, "X8 stat shows the times",
     (long)st.st_mtime);
  int fd = open("tstamp", O_RDONLY);
  char b[4];
  (void)!read(fd, b, 2);
  close(fd);
  ck(stat("tstamp", &st) == 0 && st.st_mtime == 1234567890, "X8 open+read leaves mtime", (long)st.st_mtime);
  fd = open("tstamp", O_WRONLY | O_APPEND);
  (void)!write(fd, "y\n", 2);
  struct timespec t2[2] = {{0, UTIME_OMIT}, {1500000000, 0}};
  int r = futimens(fd, t2);
  close(fd);
  ck(r == 0 && stat("tstamp", &st) == 0 && st.st_mtime == 1500000000 && st.st_size == 4, "X8 append + futimens",
     (long)st.st_mtime);
  fd = open("tstamp", O_WRONLY);
  (void)!write(fd, "z", 1);
  close(fd);
  ck(stat("tstamp", &st) == 0 && st.st_mtime > 1500000000, "X8 write() moves mtime to now", (long)st.st_mtime);
}

static void x9_statfs(void) {
  struct statfs sf;
  memset(&sf, 0, sizeof sf);
  int sr = statfs(R, &sf);
  ck(sr == 0 && sf.f_bsize > 0 && sf.f_blocks > 0, "X9 statfs", (long)sf.f_type);
  int fd = open("system.reg", O_RDONLY);
  struct statfs sf2;
  ck(fd >= 0 && fstatfs(fd, &sf2) == 0 && sf2.f_type == sf.f_type, "X9 fstatfs agrees", (long)sf2.f_type);
  out("[LP] info X9 f_type=0x%lx (ext2 0xef53)\n", (long)sf.f_type);
  char v[64];
  errno = 0;
  ssize_t g = fgetxattr(fd, "user.DOSATTRIB", v, sizeof v);
  out("[LP] info X9 fgetxattr user.DOSATTRIB -> %ld errno=%d\n", (long)g, g < 0 ? errno : 0);
  ck(g >= 0 || errno == ENODATA || errno == ENOTSUP || errno == ENOSYS, "X9 fgetxattr answers a Wine-tolerated errno",
     errno);
  if (fd >= 0) close(fd);
}

static void x10_bigdir(void) {
  ck(mkdir("drive_c/windows/syswow", 0777) == 0, "X10 mkdir", errno);
  char n[300];
  int ok = 1;
  for (int i = 0; i < 900 && ok; i++) {
    snprintf(n, sizeof n, "%s/drive_c/windows/syswow/api-ms-win-core-file-l1-%04d.dll", R, i);
    int fd = open(n, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 || write(fd, "MZ", 2) != 2) ok = 0;
    if (fd >= 0) close(fd);
  }
  ck(ok, "X10 900 files created", errno);
  DIR *d = opendir("drive_c/windows/syswow");
  int cnt = 0, dot = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) { if (e->d_name[0] == '.') dot++; else cnt++; }
  if (d) closedir(d);
  ck(cnt == 900 && dot == 2, "X10 getdents64 lists all 900 (+ . ..)", cnt);
  ok = 1;
  for (int i = 0; i < 900 && ok; i += 3) {
    char a[300], b[300];
    snprintf(a, sizeof a, "drive_c/windows/syswow/api-ms-win-core-file-l1-%04d.dll", i);
    snprintf(b, sizeof b, "drive_c/windows/syswow/renamed-%04d.dll", i);
    if (rename(a, b) != 0) ok = 0;
  }
  ck(ok, "X10 300 renames in the big directory", errno);
  ok = 1;
  d = opendir("drive_c/windows/syswow");
  cnt = 0;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    cnt++;
    snprintf(n, sizeof n, "drive_c/windows/syswow/%s", e->d_name);
    if (unlink(n) != 0) ok = 0; /* unlink while iterating, as rm -r does */
  }
  if (d) closedir(d);
  ck(ok && cnt == 900, "X10 unlink all while iterating", cnt);
  ck(rmdir("drive_c/windows/syswow") == 0, "X10 rmdir the emptied dir", errno);
}

static void x11_copy(void) {
  int s = open("srcdll", O_RDWR | O_CREAT | O_TRUNC, 0644);
  static unsigned char b[1 << 16], c[1 << 16];
  int ok = s >= 0;
  for (int k = 0; k < 32 && ok; k++) {
    for (unsigned i = 0; i < sizeof b; i++) b[i] = (unsigned char)(i * 7 + k * 13);
    if (write(s, b, sizeof b) != (ssize_t)sizeof b) ok = 0;
  }
  ck(ok, "X11 2 MiB source", errno);
  if (s < 0) return;
  lseek(s, 0, SEEK_SET);
  int d = open("drive_c/windows/system32/copy.dll", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ssize_t n;
  long tot = 0;
  while (d >= 0 && (n = read(s, b, sizeof b)) > 0) {
    if (write(d, b, (size_t)n) != n) { ok = 0; break; }
    tot += n;
  }
  if (d >= 0) close(d);
  ck(ok && tot == (32L << 16), "X11 copied with read()/write()", tot);
  d = open("drive_c/windows/system32/copy.dll", O_RDONLY);
  lseek(s, 0, SEEK_SET);
  long diff = 0;
  for (int k = 0; k < 32 && d >= 0; k++) {
    if (read(s, b, sizeof b) != (ssize_t)sizeof b || read(d, c, sizeof c) != (ssize_t)sizeof c) { diff++; break; }
    if (memcmp(b, c, sizeof b)) diff++;
  }
  ck(d >= 0 && diff == 0, "X11 copy identical", diff);
  if (d >= 0) close(d);
  close(s);
  unlink("srcdll");
}

static void x12_dirs(void) {
  ck(mkdir("d1", 0777) == 0 && put("d1/f", "f\n") == 0 && mkdir("d2", 0777) == 0, "X12 d1/f, empty d2", errno);
  ck(rename("d1", "d2") == 0 && has("d2/f", "f\n") && access("d1", F_OK) == -1, "X12 rename dir over an empty dir",
     errno);
  ck(rmdir("d2") == -1 && errno == ENOTEMPTY, "X12 rmdir non-empty -> ENOTEMPTY", errno);
  int fd = open("d2/f", O_RDWR);
  ck(fd >= 0 && rename("d2/f", "moved") == 0 && write(fd, "g", 1) == 1, "X12 rename an open file, write via old fd",
     errno);
  if (fd >= 0) close(fd);
  ck(has("moved", "g\n") && rmdir("d2") == 0, "X12 bytes at the new name; rmdir the emptied dir", errno);
}

int main(int argc, char **argv) {
  const char *vol = argc > 1 ? argv[1] : "/EXT";
  struct stat st;
  out("[LP] pfx probe (C3-pfx): Wine prefix operations on %s\n", vol);
  if (stat(vol, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP all rows: %s absent\n", vol);
    out("[LP] done rc=0\n");
    return 0;
  }
  snprintf(R, sizeof R, "%s/pfxprobe-%d", vol, (int)getpid());
  x1_setup();
  x2_registry();
  x3_locks();
  x4_trunc();
  x5_fchdir();
  x6_at();
  x7_map();
  x8_times();
  x9_statfs();
  x10_bigdir();
  x11_copy();
  x12_dirs();
  (void)P;
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
