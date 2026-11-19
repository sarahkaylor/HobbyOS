/* src/user/linux_probe_ext2.c — Linux-ABI probe: the writable ext2 volume.
 *
 * An ORDINARY musl x86_64 Linux program (no HobbyOS header).  In HobbyOS it
 * runs against /EXT (the ext2 data disk, cp/ext2); on a Linux host the same
 * binary runs against a loop-mounted copy of the same image, which is the
 * reference behaviour.  Rows are "[LP] ..." and the last is
 * "[LP] done rc=<failures>" (tools/gate/t3_linux_probe.sh accepts rc=0).
 *
 *   argv[1]  volume root        (default /EXT)
 *   argv[2]  a file OUTSIDE the volume, reached through an absolute symlink
 *            (Wine's dosdevices/z: -> /); default /PROBE.ELF (the probe's own
 *            image on the FAT boot volume)
 *
 * Phase 1 (fresh volume: <root>/lp/phase1.done absent) builds a tree with
 * every operation the P3 gate names; phase 2 (marker present, i.e. after a
 * reboot) re-reads every byte phase 1 wrote, proving persistence, then
 * appends one record.  The host side (tools/cp/ext2_gate.sh) checks the
 * image after each boot with e2fsck -fn and debugfs, byte for byte.
 *
 * The content generators below are mirrored in tools/cp/ext2_expect.py.
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
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SYS_renameat2
#define SYS_renameat2 316
#endif
#define RENAME_NOREPLACE_ 1

static int fails;
static char R[256];       /* volume root */
static char LP[300];      /* <root>/lp */

static void row(const char *fmt, ...) {
  char b[1600];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

#define CK(cond, ...)                                                          \
  do {                                                                         \
    int _ok = (cond);                                                          \
    char _m[1400];                                                             \
    snprintf(_m, sizeof _m, __VA_ARGS__);                                      \
    row("[LP] %s %s (errno=%d)\n", _ok ? "ok  " : "FAIL", _m, _ok ? 0 : errno);\
    if (!_ok) fails++;                                                         \
  } while (0)

static const char *P(const char *rel) {
  static char bufs[8][400];
  static int k;
  char *b = bufs[k++ & 7];
  snprintf(b, 400, "%s/%s", LP, rel);
  return b;
}

/* ---- content generators (mirrored by tools/cp/ext2_expect.py) ---------- */
static uint8_t pat_a(uint64_t i) { return (uint8_t)(i * 131u + (i >> 8) * 7u + 0x5Au); }
static uint8_t pat_b(uint64_t i) { return (uint8_t)(i * 17u + 0xC3u); }

#define BIG_LEN (1048576 + 333)
#define PATCH_OFF 300000
#define PATCH_LEN 4096
#define SPARSE_OFF (5ull << 30)
static const char SPARSE_REC[] = "SPARSE-AT-5GiB\n";
static const char HELLO1[] = "hello ext2\nappended\n";

static char SLOW[160];
static void mk_slow(void) {
  /* 120-byte target: forces a slow (block-backed) symlink */
  memcpy(SLOW, "sub2/", 5);
  for (int i = 5; i < 120; i++) SLOW[i] = (char)('a' + i % 26);
  SLOW[120] = 0;
}

static int write_all(int fd, const void *b, size_t n) {
  const uint8_t *p = b;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int put_file(const char *path, const void *b, size_t n, int mode) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
  if (fd < 0) return -1;
  int r = write_all(fd, b, n);
  close(fd);
  return r;
}

static ssize_t get_file(const char *path, void *b, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t got = 0;
  for (;;) {
    ssize_t r = read(fd, (uint8_t *)b + got, cap - got);
    if (r <= 0) break;
    got += (size_t)r;
    if (got == cap) break;
  }
  close(fd);
  return (ssize_t)got;
}

static int is_text(const char *path, const char *want) {
  char b[256];
  ssize_t n = get_file(path, b, sizeof b - 1);
  if (n < 0) return 0;
  b[n] = 0;
  return strcmp(b, want) == 0;
}

static int cmp_names(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sorted, space-joined listing of a directory (no "." / "..") */
static void listing(const char *dir, char *out, size_t cap) {
  char *names[64];
  int n = 0;
  out[0] = 0;
  DIR *d = opendir(dir);
  if (!d) {
    snprintf(out, cap, "<opendir errno=%d>", errno);
    return;
  }
  struct dirent *e;
  while ((e = readdir(d)) && n < 64) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (!strcmp(e->d_name, "lost+found")) continue;
    names[n++] = strdup(e->d_name);
  }
  closedir(d);
  qsort(names, (size_t)n, sizeof names[0], cmp_names);
  for (int i = 0; i < n; i++) {
    strncat(out, names[i], cap - strlen(out) - 2);
    if (i + 1 < n) strncat(out, " ", cap - strlen(out) - 1);
    free(names[i]);
  }
}

static int check_big(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  static uint8_t b[65536];
  uint64_t off = 0;
  int ok = 1;
  for (;;) {
    ssize_t r = read(fd, b, sizeof b);
    if (r <= 0) break;
    for (ssize_t i = 0; i < r; i++) {
      uint64_t pos = off + (uint64_t)i;
      uint8_t want = (pos >= PATCH_OFF && pos < PATCH_OFF + PATCH_LEN)
                         ? pat_b(pos - PATCH_OFF)
                         : pat_a(pos);
      if (b[i] != want) ok = 0;
    }
    off += (uint64_t)r;
  }
  close(fd);
  return ok && off == BIG_LEN;
}

static const char *EXPECT_LP1 =
    "big.bin dosdevices hello.hard hello.txt link_dir link_fast link_slow "
    "sparse5g sub sub2 trunc.bin trunc2.bin victim.txt";

/* ---- phase 1 ---------------------------------------------------------- */

static void phase1(const char *outside) {
  struct stat st;
  struct statfs sf;
  CK(statfs(R, &sf) == 0 && sf.f_type == 0xEF53,
     "statfs root f_type=0x%lx bsize=%ld", (long)sf.f_type, (long)sf.f_bsize);
  long free0 = (long)sf.f_bfree;
  CK(mkdir(LP, 0755) == 0, "mkdir lp");
  CK(mkdir(P("sub"), 0755) == 0, "mkdir lp/sub");
  errno = 0;
  CK(mkdir(P("sub"), 0755) == -1 && errno == EEXIST, "mkdir existing -> EEXIST");
  int dfd = open(LP, O_RDONLY | O_DIRECTORY);
  CK(dfd >= 0, "open lp O_DIRECTORY");
  CK(mkdirat(dfd, "sub2", 0750) == 0, "mkdirat(dirfd, sub2)");
  CK(stat(P("sub2"), &st) == 0 && S_ISDIR(st.st_mode) &&
         (st.st_mode & 07777) == 0750,
     "sub2 mode=%o", st.st_mode & 07777);

  /* create / write / append / read back */
  int fd = open(P("hello.txt"), O_CREAT | O_TRUNC | O_WRONLY, 0640);
  CK(fd >= 0 && write_all(fd, "hello ext2\n", 11) == 0, "create+write hello.txt");
  close(fd);
  fd = open(P("hello.txt"), O_WRONLY | O_APPEND);
  CK(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0 && write_all(fd, "appended\n", 9) == 0,
     "O_APPEND write (after lseek 0)");
  close(fd);
  CK(is_text(P("hello.txt"), HELLO1), "hello.txt reads back appended text");
  fd = openat(dfd, "hello.txt", O_RDONLY);
  char b4[8] = {0};
  CK(fd >= 0 && read(fd, b4, 5) == 5 && !memcmp(b4, "hello", 5),
     "openat(dirfd) relative read");
  CK(fstat(fd, &st) == 0 && st.st_size == 20 && S_ISREG(st.st_mode) &&
         (st.st_mode & 0777) == 0640 && st.st_nlink == 1,
     "fstat size=%lld mode=%o nlink=%ld", (long long)st.st_size,
     st.st_mode & 07777, (long)st.st_nlink);
  close(fd);
  fd = open(P("hello.txt"), O_RDONLY);
  errno = 0;
  CK(write(fd, "x", 1) == -1 && errno == EBADF, "write on O_RDONLY -> EBADF");
  close(fd);
  errno = 0;
  CK(open(P("hello.txt"), O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST,
     "O_CREAT|O_EXCL existing -> EEXIST");
  errno = 0;
  CK(open(P("nope/x"), O_RDONLY) == -1 && errno == ENOENT, "open missing -> ENOENT");

  /* big file: sequential writes, pwrite patch, pread checks */
  static uint8_t buf[65536];
  fd = open(P("big.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
  int ok = fd >= 0;
  for (uint64_t off = 0; ok && off < BIG_LEN;) {
    size_t n = BIG_LEN - off < sizeof buf ? (size_t)(BIG_LEN - off) : sizeof buf;
    for (size_t i = 0; i < n; i++) buf[i] = pat_a(off + i);
    ok = write_all(fd, buf, n) == 0;
    off += n;
  }
  CK(ok, "write big.bin %d bytes in 64 KiB chunks", BIG_LEN);
  for (int i = 0; i < PATCH_LEN; i++) buf[i] = pat_b((uint64_t)i);
  CK(pwrite(fd, buf, PATCH_LEN, PATCH_OFF) == PATCH_LEN, "pwrite patch @%d", PATCH_OFF);
  CK(lseek(fd, 0, SEEK_END) == BIG_LEN, "lseek SEEK_END = %d", BIG_LEN);
  uint8_t pb[16];
  CK(pread(fd, pb, 16, PATCH_OFF - 8) == 16 && pb[7] == pat_a(PATCH_OFF - 1) &&
         pb[8] == pat_b(0),
     "pread across the patch edge");
  CK(pread(fd, pb, 16, BIG_LEN - 4) == 4, "pread at EOF is short (4)");
  CK(fsync(fd) == 0, "fsync big.bin");
  CK(fdatasync(fd) == 0, "fdatasync big.bin");
  close(fd);
  CK(check_big(P("big.bin")), "big.bin content (read back whole file)");

  /* sparse file with data at 5 GiB */
  fd = open(P("sparse5g"), O_CREAT | O_RDWR, 0644);
  CK(fd >= 0 && pwrite(fd, SPARSE_REC, 15, (off_t)SPARSE_OFF) == 15,
     "pwrite 15 bytes at 5 GiB");
  CK(fstat(fd, &st) == 0 && (uint64_t)st.st_size == SPARSE_OFF + 15 &&
         st.st_blocks <= 64,
     "sparse size=%lld blocks=%lld", (long long)st.st_size,
     (long long)st.st_blocks);
  char sb[16] = {0};
  CK(lseek(fd, (off_t)SPARSE_OFF, SEEK_SET) == (off_t)SPARSE_OFF &&
         read(fd, sb, 15) == 15 && !memcmp(sb, SPARSE_REC, 15),
     "lseek 5 GiB + read back");
  memset(sb, 1, sizeof sb);
  CK(pread(fd, sb, 16, 4096) == 16 && sb[0] == 0 && sb[15] == 0, "hole reads zero");
  close(fd);

  /* ftruncate / truncate */
  for (int i = 0; i < 10000; i++) buf[i] = pat_a((uint64_t)i);
  CK(put_file(P("trunc.bin"), buf, 10000, 0644) == 0, "trunc.bin 10000");
  fd = open(P("trunc.bin"), O_RDWR);
  CK(ftruncate(fd, 6000) == 0 && fstat(fd, &st) == 0 && st.st_size == 6000,
     "ftruncate 6000");
  CK(ftruncate(fd, 8192) == 0 && fstat(fd, &st) == 0 && st.st_size == 8192,
     "ftruncate up 8192");
  memset(buf, 0xEE, 4096);
  CK(pread(fd, buf, 4096, 4096) == 4096 && buf[6000 - 4096 - 1] == pat_a(5999) &&
         buf[6000 - 4096] == 0 && buf[4095] == 0,
     "regrown tail reads zero");
  close(fd);
  for (int i = 0; i < 5000; i++) buf[i] = pat_a((uint64_t)i);
  CK(put_file(P("trunc2.bin"), buf, 5000, 0644) == 0 &&
         truncate(P("trunc2.bin"), 100) == 0 && stat(P("trunc2.bin"), &st) == 0 &&
         st.st_size == 100,
     "truncate(path) 100");

  /* symlinks */
  mk_slow();
  CK(symlink("hello.txt", P("link_fast")) == 0, "symlink fast");
  CK(symlink(SLOW, P("link_slow")) == 0, "symlink slow (120 bytes)");
  CK(symlinkat("sub", dfd, "link_dir") == 0, "symlinkat(dirfd)");
  char t[256];
  ssize_t n = readlink(P("link_fast"), t, sizeof t);
  CK(n == 9 && !memcmp(t, "hello.txt", 9), "readlink fast n=%zd", n);
  n = readlinkat(dfd, "link_slow", t, sizeof t);
  CK(n == 120 && !memcmp(t, SLOW, 120), "readlinkat slow n=%zd", n);
  n = readlink(P("link_fast"), t, 4);
  CK(n == 4 && !memcmp(t, "hell", 4), "readlink truncates to bufsiz");
  errno = 0;
  CK(readlink(P("hello.txt"), t, sizeof t) == -1 && errno == EINVAL,
     "readlink regular -> EINVAL");
  CK(lstat(P("link_fast"), &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 9,
     "lstat link_fast is a link size=%lld", (long long)st.st_size);
  CK(stat(P("link_fast"), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 20,
     "stat link_fast follows");
  CK(fstatat(dfd, "link_dir", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode),
     "fstatat NOFOLLOW");
  CK(fstatat(dfd, "link_dir", &st, 0) == 0 && S_ISDIR(st.st_mode), "fstatat follows to dir");
  CK(is_text(P("link_fast"), HELLO1), "open through symlink");
  errno = 0;
  CK(open(P("link_fast"), O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP,
     "O_NOFOLLOW on link -> ELOOP");
  CK(mkdir(P("dosdevices"), 0755) == 0 && symlink("/", P("dosdevices/z:")) == 0 &&
         symlink("../sub", P("dosdevices/c:")) == 0,
     "dosdevices z: -> /, c: -> ../sub");
  char zp[400];
  snprintf(zp, sizeof zp, "%s/dosdevices/z:%s", LP, outside);
  fd = open(zp, O_RDONLY);
  CK(fd >= 0 && read(fd, b4, 4) == 4, "open %s through z:", zp);
  if (fd >= 0) close(fd);
  CK(stat(P("dosdevices/c:"), &st) == 0 && S_ISDIR(st.st_mode), "c: resolves to a dir");

  /* rename */
  CK(put_file(P("r1.txt"), "r1\n", 3, 0644) == 0, "r1.txt");
  CK(rename(P("r1.txt"), P("r2.txt")) == 0, "rename same dir");
  CK(rename(P("r2.txt"), P("sub/r3.txt")) == 0, "rename across dirs");
  CK(renameat(dfd, "sub/r3.txt", dfd, "sub2/r4.txt") == 0, "renameat(dirfd)");
  CK(put_file(P("victim.txt"), "victim\n", 7, 0644) == 0 &&
         put_file(P("repl.txt"), "replacement\n", 12, 0644) == 0 &&
         rename(P("repl.txt"), P("victim.txt")) == 0 &&
         is_text(P("victim.txt"), "replacement\n"),
     "rename over an existing file");
  errno = 0;
  CK(syscall(SYS_renameat2, dfd, "hello.txt", dfd, "victim.txt", RENAME_NOREPLACE_) == -1 &&
         errno == EEXIST,
     "renameat2 RENAME_NOREPLACE -> EEXIST");
  CK(mkdir(P("sub/inner"), 0755) == 0 &&
         rename(P("sub/inner"), P("sub2/inner_moved")) == 0,
     "rename a directory across parents");
  errno = 0;
  CK(rename(P("sub2"), P("sub2/inner_moved/x")) == -1 && errno == EINVAL,
     "rename into own subtree -> EINVAL");

  /* hard link */
  CK(link(P("hello.txt"), P("hello.hard")) == 0 && stat(P("hello.txt"), &st) == 0 &&
         st.st_nlink == 2,
     "link -> nlink=%ld", (long)st.st_nlink);

  /* unlink / rmdir */
  CK(put_file(P("gone.txt"), "g", 1, 0644) == 0 && unlink(P("gone.txt")) == 0,
     "unlink");
  errno = 0;
  CK(stat(P("gone.txt"), &st) == -1 && errno == ENOENT, "stat unlinked -> ENOENT");
  errno = 0;
  CK(unlink(P("gone.txt")) == -1 && errno == ENOENT, "unlink missing -> ENOENT");
  CK(mkdir(P("gonedir"), 0755) == 0 && rmdir(P("gonedir")) == 0, "rmdir");
  errno = 0;
  CK(rmdir(P("sub2")) == -1 && errno == ENOTEMPTY, "rmdir nonempty -> ENOTEMPTY");
  errno = 0;
  CK(unlinkat(dfd, "sub2", AT_REMOVEDIR) == -1 && errno == ENOTEMPTY,
     "unlinkat AT_REMOVEDIR nonempty");
  errno = 0;
  CK(rmdir(P("hello.txt")) == -1 && errno == ENOTDIR, "rmdir file -> ENOTDIR");
  errno = 0;
  CK(unlink(P("sub")) == -1 && errno == EISDIR, "unlink dir -> EISDIR");

  /* unlink while open: data survives until close */
  for (int i = 0; i < 5000; i++) buf[i] = pat_b((uint64_t)i);
  CK(put_file(P("tmp.bin"), buf, 5000, 0644) == 0, "tmp.bin");
  fd = open(P("tmp.bin"), O_RDONLY);
  CK(fd >= 0 && unlink(P("tmp.bin")) == 0, "unlink open tmp.bin");
  memset(buf, 0, 5000);
  CK(read(fd, buf, 5000) == 5000 && buf[4999] == pat_b(4999), "read after unlink");
  close(fd);

  /* directory listing (getdents64) */
  char ls[1024];
  listing(LP, ls, sizeof ls);
  CK(strcmp(ls, EXPECT_LP1) == 0, "readdir lp = [%s]", ls);
  listing(P("sub2"), ls, sizeof ls);
  CK(strcmp(ls, "inner_moved r4.txt") == 0, "readdir sub2 = [%s]", ls);

  /* cwd-relative names */
  char cwd[300];
  CK(chdir(LP) == 0 && getcwd(cwd, sizeof cwd) && strcmp(cwd, LP) == 0 &&
         is_text("victim.txt", "replacement\n"),
     "chdir + relative open (cwd=%s)", cwd);
  CK(access("hello.txt", F_OK) == 0, "access F_OK");
  (void)!chdir("/");

  CK(fstatfs(dfd, &sf) == 0 && sf.f_type == 0xEF53 && (long)sf.f_bfree < free0,
     "fstatfs: free blocks %ld -> %ld", free0, (long)sf.f_bfree);
  close(dfd);

  CK(put_file(P("phase1.done"), "1\n", 2, 0644) == 0, "write phase1.done");
  fd = open(P("phase1.done"), O_RDONLY);
  CK(fd >= 0 && fsync(fd) == 0, "fsync marker");
  if (fd >= 0) close(fd);
  row("[LP] phase1 complete\n");
}

/* ---- phase 2: after a reboot ------------------------------------------- */

static void phase2(void) {
  struct stat st;
  row("[LP] phase2: reading what phase 1 wrote before the reboot\n");
  CK(is_text(P("hello.txt"), HELLO1), "hello.txt persisted");
  CK(stat(P("hello.hard"), &st) == 0 && st.st_nlink == 2, "hard link persisted");
  CK(check_big(P("big.bin")), "big.bin persisted (every byte)");
  int fd = open(P("sparse5g"), O_RDONLY);
  char sb[16] = {0};
  CK(fd >= 0 && pread(fd, sb, 15, (off_t)SPARSE_OFF) == 15 && !memcmp(sb, SPARSE_REC, 15),
     "sparse5g @5 GiB persisted");
  CK(fstat(fd, &st) == 0 && (uint64_t)st.st_size == SPARSE_OFF + 15, "sparse5g size");
  if (fd >= 0) close(fd);
  CK(stat(P("trunc.bin"), &st) == 0 && st.st_size == 8192, "trunc.bin size");
  CK(stat(P("trunc2.bin"), &st) == 0 && st.st_size == 100, "trunc2.bin size");
  mk_slow();
  char t[256];
  ssize_t n = readlink(P("link_slow"), t, sizeof t);
  CK(n == 120 && !memcmp(t, SLOW, 120), "slow symlink persisted");
  n = readlink(P("dosdevices/z:"), t, sizeof t);
  CK(n == 1 && t[0] == '/', "z: persisted");
  CK(is_text(P("sub2/r4.txt"), "r1\n"), "renamed file persisted");
  CK(is_text(P("victim.txt"), "replacement\n"), "replaced file persisted");
  char ls[1024];
  listing(LP, ls, sizeof ls);
  char want[1024];
  snprintf(want, sizeof want, "%s", EXPECT_LP1);
  /* phase1.done sorts between link_slow and sparse5g */
  char *at = strstr(want, " sparse5g");
  if (at) {
    char tail[512];
    snprintf(tail, sizeof tail, "%s", at);
    snprintf(at, sizeof want - (size_t)(at - want), " phase1.done%s", tail);
  }
  CK(strcmp(ls, want) == 0, "readdir lp after reboot = [%s]", ls);
  fd = open(P("hello.txt"), O_WRONLY | O_APPEND);
  CK(fd >= 0 && write_all(fd, "phase2\n", 7) == 0 && fsync(fd) == 0,
     "append phase2 record");
  if (fd >= 0) close(fd);
  CK(put_file(P("phase2.done"), "2\n", 2, 0644) == 0, "write phase2.done");
  row("[LP] phase2 complete\n");
}

int main(int argc, char **argv) {
  snprintf(R, sizeof R, "%s", argc > 1 ? argv[1] : "/EXT");
  const char *outside = argc > 2 ? argv[2] : "/PROBE.ELF";
  snprintf(LP, sizeof LP, "%s/lp", R);
  struct stat st;
  if (stat(P("phase1.done"), &st) == 0)
    phase2();
  else
    phase1(outside);
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
