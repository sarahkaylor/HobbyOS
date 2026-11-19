/* src/user/linux_probe_ext2b.c — Linux-ABI probe: what a Wine prefix needs
 * from the /EXT ext2 volume beyond cp/ext2 (lane C3-ext2b, docs/cp-ext2b.md).
 *
 * An ORDINARY musl x86_64 Linux program (no HobbyOS header): the same binary
 * runs inside HobbyOS (the measurement) and on a Linux host against a
 * loop-mounted copy of the same image (the reference).  Rows are "[LP] ...";
 * the main instance ends with "[LP] done rc=<failures>" (the helper's
 * failures are added in), which tools/gate/t3_linux_probe.sh grades.
 *
 *   argv[1]  volume root           (default /EXT)
 *   argv[2]  role: "main" | "helper" (default main)
 *   argv[3]  a FAT16 file          (default /PROBE.ELF; absent = rows skipped)
 *
 * Sections (main):
 *   mmap    MAP_PRIVATE / MAP_SHARED read of an ext2 file, the Linux EACCES
 *           rules, copy-on-write isolation, write()->mapping coherence,
 *           MAP_SHARED writable stores reaching read() and the disk (msync /
 *           munmap), growth under a mapping, mapping past EOF
 *   attr    chmod/fchmod/fchmodat, chown/fchown/lchown/fchownat,
 *           utimensat/futimens/utimes/utime, round-tripped through stat; the
 *           FAT16 answers
 *   path    a path of more than 256 (and 600) bytes through every call kind;
 *           1024+ is ENAMETOOLONG
 *   locks   fcntl F_SETLK/F_SETLKW/F_GETLK and flock between TWO processes
 *           (this one and the "helper" wave member), blocking wake-up,
 *           EDEADLK, release on unlock / on close / on exit; OFD locks;
 *           range split
 *   exec    the static and dynamic glibc 2.31 programs the gate staged on
 *           <root>/lpx/bin ran from ext2 (their markers in <root>/lpx/out)
 *
 * The host gate (tools/cp/ext2_gate.sh) then checks the left-behind state with
 * e2fsck -fn + debugfs: modes, owners, times, and the mapped-store bytes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif

static int fails;
static char R[256];  /* volume root */
static char X[300];  /* <root>/lpx */
static const char *FAT = "/PROBE.ELF";
static int is_helper;

static void row(const char *fmt, ...) {
  char b[1600];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

#define CK(cond, ...)                                                          \
  do {                                                                         \
    int _ok = (cond);                                                          \
    int _e = errno;                                                            \
    char _m[1400];                                                             \
    snprintf(_m, sizeof _m, __VA_ARGS__);                                      \
    row("[LP] %s%s %s (errno=%d)\n", is_helper ? "helper " : "",               \
        _ok ? "ok  " : "FAIL", _m, _ok ? 0 : _e);                              \
    if (!_ok) fails++;                                                         \
  } while (0)

static const char *P(const char *rel) {
  static char bufs[8][512];
  static int k;
  char *b = bufs[k++ & 7];
  snprintf(b, 512, "%s/%s", X, rel);
  return b;
}

static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&t, 0);
}

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* wait for a marker file; 1 = appeared */
static int wait_for(const char *p, int timeout_ms) {
  int64_t t0 = now_ms();
  while (now_ms() - t0 < timeout_ms) {
    if (exists(p)) return 1;
    sleep_ms(20);
  }
  return exists(p);
}

static int put_text(const char *p, const char *s) {
  char tmp[600];
  snprintf(tmp, sizeof tmp, "%s.tmp", p);
  int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return -1;
  size_t n = strlen(s);
  int ok = write(fd, s, n) == (ssize_t)n;
  close(fd);
  return ok && rename(tmp, p) == 0 ? 0 : -1; /* the marker appears whole */
}

static int get_text(const char *p, char *b, size_t cap) {
  int fd = open(p, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, b, cap - 1);
  close(fd);
  if (n < 0) return -1;
  b[n] = 0;
  return (int)n;
}

static uint8_t pat(uint64_t i) { return (uint8_t)(i * 37u + (i >> 9) * 11u + 0x21u); }

/* ===================================================================== */
/* mmap                                                                  */
/* ===================================================================== */

#define M_LEN (3 * 4096 + 100)

static void sec_mmap(void) {
  static uint8_t want[8 * 4096];
  for (int i = 0; i < M_LEN; i++) want[i] = pat((uint64_t)i);
  const char *mf = P("m.bin");
  int wfd = open(mf, O_CREAT | O_TRUNC | O_RDWR, 0644);
  CK(wfd >= 0 && write(wfd, want, M_LEN) == M_LEN, "create m.bin (%d bytes)", M_LEN);
  int rfd = open(mf, O_RDONLY);
  CK(rfd >= 0, "open m.bin O_RDONLY");

  /* MAP_PRIVATE read */
  uint8_t *pr = mmap(0, 4 * 4096, PROT_READ, MAP_PRIVATE, rfd, 0);
  CK(pr != MAP_FAILED, "mmap PRIVATE PROT_READ %p", (void *)pr);
  if (pr == MAP_FAILED) return;
  int eq = memcmp(pr, want, M_LEN) == 0;
  int tailz = 1;
  for (int i = M_LEN; i < 4 * 4096; i++)
    if (pr[i]) tailz = 0;
  CK(eq, "private view == file bytes");
  CK(tailz, "bytes past EOF in the last page read as 0");

  /* MAP_SHARED read through an O_RDONLY fd */
  uint8_t *sh = mmap(0, 4 * 4096, PROT_READ, MAP_SHARED, rfd, 0);
  CK(sh != MAP_FAILED && memcmp(sh, want, M_LEN) == 0, "mmap SHARED PROT_READ == file");

  /* Linux's access rules */
  errno = 0;
  void *bad = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rfd, 0);
  CK(bad == MAP_FAILED && errno == EACCES, "SHARED+PROT_WRITE on O_RDONLY fd -> EACCES");
  int wo = open(mf, O_WRONLY);
  errno = 0;
  bad = mmap(0, 4096, PROT_READ, MAP_PRIVATE, wo, 0);
  CK(bad == MAP_FAILED && errno == EACCES, "mmap of an O_WRONLY fd -> EACCES");
  close(wo);
  errno = 0;
  bad = mmap(0, 4096, PROT_READ, MAP_PRIVATE, rfd, 1);
  CK(bad == MAP_FAILED && errno == EINVAL, "unaligned offset -> EINVAL");

  /* offset mapping */
  uint8_t *po = mmap(0, 4096, PROT_READ, MAP_PRIVATE, rfd, 2 * 4096);
  CK(po != MAP_FAILED && memcmp(po, want + 2 * 4096, 4096) == 0, "mmap at offset 8192");

  /* copy-on-write: PROT_WRITE on a MAP_PRIVATE mapping of an O_RDONLY fd */
  uint8_t *cw = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, rfd, 0);
  CK(cw != MAP_FAILED, "mmap PRIVATE RW (O_RDONLY fd is enough)");
  cw[4096 + 5] = 0xEE;
  CK(cw[4096 + 5] == 0xEE, "private store visible in its own mapping");
  CK(sh[4096 + 5] == want[4096 + 5], "... not in a MAP_SHARED view");
  CK(pr[4096 + 5] == want[4096 + 5], "... not in another private view");
  uint8_t b1;
  CK(pread(rfd, &b1, 1, 4096 + 5) == 1 && b1 == want[4096 + 5], "... not in the file");

  /* write() coherence: a later write is seen by the shared view and by the
     private views' NOT-YET-COPIED pages (Linux), but not by a copied page */
  uint8_t nb[16];
  memset(nb, 0x5A, sizeof nb);
  CK(pwrite(wfd, nb, sizeof nb, 2 * 4096 + 7) == (ssize_t)sizeof nb, "pwrite 16 bytes at 8199");
  memcpy(want + 2 * 4096 + 7, nb, sizeof nb);
  CK(memcmp(sh + 2 * 4096 + 7, nb, sizeof nb) == 0, "shared view sees the write()");
  CK(memcmp(pr + 2 * 4096 + 7, nb, sizeof nb) == 0, "private untouched page sees the write()");
  CK(memcmp(cw + 2 * 4096 + 7, nb, sizeof nb) == 0, "private RW mapping's uncopied page sees it");
  CK(pwrite(wfd, nb, 1, 4096 + 5) == 1, "pwrite into the copied page");
  want[4096 + 5] = 0x5A;
  CK(cw[4096 + 5] == 0xEE, "the private COPY does not see it");
  CK(sh[4096 + 5] == 0x5A, "the shared view does");

  /* MAP_SHARED writable through an O_RDWR fd: stores reach read() and disk */
  uint8_t *sw = mmap(0, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, wfd, 0);
  CK(sw != MAP_FAILED, "mmap SHARED RW (O_RDWR fd)");
  if (sw != MAP_FAILED) {
    memcpy(sw + 100, "MAPPED-STORE-1", 14);
    memcpy(want + 100, "MAPPED-STORE-1", 14);
    CK(memcmp(sh + 100, "MAPPED-STORE-1", 14) == 0, "store visible in the other SHARED view");
    char rb[16];
    CK(pread(rfd, rb, 14, 100) == 14 && memcmp(rb, "MAPPED-STORE-1", 14) == 0,
       "read() sees the mapped store (before msync)");
    memcpy(sw + 3 * 4096 + 50, "MAPPED-STORE-2", 14);
    memcpy(want + 3 * 4096 + 50, "MAPPED-STORE-2", 14);
    CK(msync(sw, 4 * 4096, MS_SYNC) == 0, "msync MS_SYNC");
    /* a store past EOF in the last page must NOT extend the file */
    sw[M_LEN + 10] = 0x77;
    memcpy(sw + 2 * 4096, "MAPPED-STORE-3", 14);
    memcpy(want + 2 * 4096, "MAPPED-STORE-3", 14);
    CK(munmap(sw, 4 * 4096) == 0, "munmap SHARED RW (writes back)");
  }
  struct stat st;
  CK(fstat(wfd, &st) == 0 && st.st_size == M_LEN, "size unchanged by mapped stores (%ld)",
     (long)st.st_size);
  static uint8_t got[8 * 4096];
  int gfd = open(mf, O_RDONLY);
  ssize_t gn = read(gfd, got, sizeof got);
  close(gfd);
  CK(gn == M_LEN && memcmp(got, want, M_LEN) == 0, "re-read == every write() + mapped store (%zd)", gn);
  /* reference copy for the host gate: written with plain write() */
  int efd = open(P("m.expect"), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  CK(efd >= 0 && write(efd, want, M_LEN) == M_LEN, "write m.expect (host compares)");
  close(efd);

  /* growth under a mapping: the mapping is longer than the file */
  CK(ftruncate(wfd, 5 * 4096) == 0, "ftruncate grow to 20480");
  uint8_t *gr = mmap(0, 6 * 4096, PROT_READ, MAP_SHARED, rfd, 0);
  CK(gr != MAP_FAILED, "mmap 6 pages of a 5-page file");
  if (gr != MAP_FAILED) {
    CK(gr[4 * 4096 + 1] == 0, "grown region reads 0");
    CK(pwrite(wfd, "GROWN", 5, 4 * 4096 + 1) == 5, "pwrite into the grown region");
    CK(memcmp(gr + 4 * 4096 + 1, "GROWN", 5) == 0, "mapping sees the write in grown pages");
    munmap(gr, 6 * 4096);
  }
  CK(ftruncate(wfd, M_LEN) == 0, "ftruncate back to the original length");
  CK(sh[M_LEN + 10] == 0, "a shrink zeroes the cached tail (no stale past-EOF bytes)");
  munmap(pr, 4 * 4096);
  munmap(sh, 4 * 4096);
  munmap(po, 4096);
  munmap(cw, 4 * 4096);

  /* an unlinked file stays mapped and readable */
  int ufd = open(P("unl.bin"), O_CREAT | O_TRUNC | O_RDWR, 0644);
  CK(ufd >= 0 && write(ufd, want, 4096) == 4096, "create unl.bin");
  uint8_t *um = mmap(0, 4096, PROT_READ, MAP_SHARED, ufd, 0);
  close(ufd);
  CK(unlink(P("unl.bin")) == 0, "unlink while mapped (fd closed)");
  CK(um != MAP_FAILED && memcmp(um, want, 4096) == 0, "mapping of the unlinked file still reads");
  if (um != MAP_FAILED) munmap(um, 4096);
  close(rfd);
  close(wfd);
}

/* ===================================================================== */
/* setattr                                                               */
/* ===================================================================== */

static void sec_attr(void) {
  const char *f = P("attr.f");
  int fd = open(f, O_CREAT | O_TRUNC | O_RDWR, 0644);
  CK(fd >= 0, "create attr.f");
  struct stat st;
  CK(stat(f, &st) == 0 && (st.st_mode & 07777) == 0644, "create mode 0644 (umask 022) %o",
     st.st_mode & 07777);
  CK(chmod(f, 0640) == 0 && stat(f, &st) == 0 && st.st_mode == (S_IFREG | 0640),
     "chmod 0640 -> st_mode 0%o", st.st_mode);
  CK(fchmod(fd, 04755) == 0 && fstat(fd, &st) == 0 && st.st_mode == (S_IFREG | 04755),
     "fchmod 04755 -> 0%o", st.st_mode);
  int dfd = open(X, O_RDONLY | O_DIRECTORY);
  CK(dfd >= 0 && fchmodat(dfd, "attr.f", 06775, 0) == 0 && stat(f, &st) == 0 &&
         st.st_mode == (S_IFREG | 06775),
     "fchmodat(dirfd) 06775 -> 0%o", st.st_mode);
  CK(chown(f, 1234, 5678) == 0 && stat(f, &st) == 0 && st.st_uid == 1234 && st.st_gid == 5678,
     "chown 1234:5678 -> %u:%u", st.st_uid, st.st_gid);
  CK((st.st_mode & 06000) == 0, "chown cleared S_ISUID and S_ISGID (g+x) -> 0%o",
     st.st_mode & 07777);
  CK(fchown(fd, (uid_t)-1, 99) == 0 && fstat(fd, &st) == 0 && st.st_uid == 1234 && st.st_gid == 99,
     "fchown -1:99 keeps uid -> %u:%u", st.st_uid, st.st_gid);
  CK(fchownat(dfd, "attr.f", 1234, 5678, 0) == 0 && stat(f, &st) == 0 && st.st_gid == 5678,
     "fchownat(dirfd) back to 1234:5678");
  CK(fchmod(fd, 0751) == 0, "fchmod 0751 (left for the host check)");
  /* symlinks: lchown changes the link, chown follows it */
  unlink(P("attr.l"));
  CK(symlink("attr.f", P("attr.l")) == 0, "symlink attr.l -> attr.f");
  CK(lchown(P("attr.l"), 4242, 4343) == 0, "lchown attr.l 4242:4343");
  CK(lstat(P("attr.l"), &st) == 0 && S_ISLNK(st.st_mode) && st.st_uid == 4242 &&
         st.st_gid == 4343,
     "lstat link -> %u:%u", st.st_uid, st.st_gid);
  CK(stat(f, &st) == 0 && st.st_uid == 1234, "target owner untouched (%u)", st.st_uid);
  CK(fchownat(dfd, "attr.l", 4242, 4444, AT_SYMLINK_NOFOLLOW) == 0 &&
         lstat(P("attr.l"), &st) == 0 && st.st_gid == 4444,
     "fchownat AT_SYMLINK_NOFOLLOW -> gid %u", st.st_gid);
  errno = 0;
  CK(fchownat(dfd, "attr.f", 1, 1, 0x4000) == -1 && errno == EINVAL, "fchownat bad flag EINVAL");
  CK(chmod(P("dir.a"), 0755) == -1 && errno == ENOENT, "chmod missing -> ENOENT");

  /* times */
  struct timespec ts[2] = {{1000000000, 123456789}, {1234567890, 987654321}};
  CK(utimensat(AT_FDCWD, f, ts, 0) == 0 && stat(f, &st) == 0 && st.st_atime == 1000000000 &&
         st.st_mtime == 1234567890,
     "utimensat explicit -> atime %ld mtime %ld", (long)st.st_atime, (long)st.st_mtime);
  row("[LP] info mtime nsec=%ld (ext2 stores seconds; Linux ext2 too)\n", (long)st.st_mtim.tv_nsec);
  time_t t0 = time(0);
  struct timespec on[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
  CK(utimensat(dfd, "attr.f", on, 0) == 0 && stat(f, &st) == 0 && st.st_atime == 1000000000 &&
         st.st_mtime >= t0 - 5 && st.st_mtime <= t0 + 120,
     "UTIME_OMIT atime / UTIME_NOW mtime -> %ld / %ld (now %ld)", (long)st.st_atime,
     (long)st.st_mtime, (long)t0);
  CK(st.st_ctime >= t0 - 5 && st.st_ctime <= t0 + 120, "ctime is now (%ld)", (long)st.st_ctime);
  struct timespec oo[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
  CK(utimensat(AT_FDCWD, P("no-such"), oo, 0) == 0, "both UTIME_OMIT: 0 without a lookup");
  struct timespec bad[2] = {{0, 1000000000}, {0, 0}};
  errno = 0;
  CK(utimensat(AT_FDCWD, f, bad, 0) == -1 && errno == EINVAL, "tv_nsec 1e9 -> EINVAL");
  CK(futimens(fd, 0) == 0 && fstat(fd, &st) == 0 && st.st_atime >= t0 - 5,
     "futimens(fd, NULL) -> now (%ld)", (long)st.st_atime);
  struct timeval tv[2] = {{1500000000, 0}, {1500000001, 999999}};
  CK(utimes(f, tv) == 0 && stat(f, &st) == 0 && st.st_atime == 1500000000 &&
         st.st_mtime == 1500000001,
     "utimes -> %ld %ld", (long)st.st_atime, (long)st.st_mtime);
  struct utimbuf ub = {-86400, 86400};
  CK(utime(P("attr.l"), &ub) == 0 && stat(f, &st) == 0 && st.st_atime == -86400 &&
         st.st_mtime == 86400,
     "utime through the symlink: pre-1970 atime %ld", (long)st.st_atime);
  struct timespec fin[2] = {{1000000000, 0}, {1234567890, 0}};
  CK(utimensat(dfd, "attr.f", fin, 0) == 0, "final times 1000000000 / 1234567890 (host check)");
  struct timespec lk[2] = {{777, 0}, {888, 0}};
  CK(utimensat(dfd, "attr.l", lk, AT_SYMLINK_NOFOLLOW) == 0 && lstat(P("attr.l"), &st) == 0 &&
         st.st_mtime == 888 && stat(f, &st) == 0 && st.st_mtime == 1234567890,
     "utimensat AT_SYMLINK_NOFOLLOW sets the link only");
  /* a directory keeps its set-gid bit across chown */
  mkdir(P("attr.d"), 0755);
  CK(chmod(P("attr.d"), 02775) == 0 && chown(P("attr.d"), 7, 8) == 0 &&
         stat(P("attr.d"), &st) == 0 && st.st_mode == (S_IFDIR | 02775) && st.st_uid == 7,
     "dir chmod 02775 + chown 7:8 -> 0%o", st.st_mode);
  /* access(2) with root's rules */
  CK(access(f, X_OK) == 0, "access X_OK on 0751");
  CK(chmod(P("m.expect"), 0644) == 0 && access(P("m.expect"), X_OK) == -1 && errno == EACCES,
     "access X_OK on 0644 -> EACCES (root needs an x bit)");
  CK(faccessat(dfd, "attr.f", R_OK | W_OK, 0) == 0, "faccessat R_OK|W_OK");
  close(dfd);
  close(fd);

  /* FAT16: chmod accepted (not stored), chown only to root, times accepted */
  if (exists(FAT)) {
    struct stat s0, s1;
    stat(FAT, &s0);
    CK(chmod(FAT, 0700) == 0 && stat(FAT, &s1) == 0 && s1.st_mode == s0.st_mode,
       "FAT16 chmod: 0, mode stays the synthesized 0%o", s1.st_mode & 07777);
    CK(chown(FAT, 0, 0) == 0, "FAT16 chown 0:0 -> 0");
    errno = 0;
    CK(chown(FAT, 5, 5) == -1 && errno == EPERM, "FAT16 chown 5:5 -> EPERM (vfat)");
    CK(utimensat(AT_FDCWD, FAT, 0, 0) == 0, "FAT16 utimensat -> 0 (not stored)");
    errno = 0;
    CK(chmod("/NO-SUCH.XYZ", 0644) == -1 && errno == ENOENT, "FAT16 chmod missing -> ENOENT");
  } else {
    row("[LP] info no FAT16 file %s: FAT rows skipped (Linux reference run)\n", FAT);
  }
}

/* ===================================================================== */
/* long paths                                                            */
/* ===================================================================== */

static void sec_path(void) {
  static char p[1400];
  int n = snprintf(p, sizeof p, "%s/long", X);
  mkdir(p, 0755);
  int depth = 0;
  while (n < 680) {
    n += snprintf(p + n, sizeof p - (size_t)n, "/%02d_the_quick_brown_fox_jumps_over_the_lazy_dog",
                  depth++);
    if (mkdir(p, 0755) != 0 && errno != EEXIST) break;
  }
  CK(n > 600 && exists(p), "mkdir chain to a %d-byte path (%d levels)", n, depth);
  char f[1500];
  snprintf(f, sizeof f, "%s/leaf_file.txt", p);
  int fd = open(f, O_CREAT | O_TRUNC | O_RDWR, 0600);
  CK(fd >= 0 && write(fd, "deep-data", 9) == 9, "create %zu-byte path file", strlen(f));
  close(fd);
  struct stat st;
  CK(stat(f, &st) == 0 && st.st_size == 9, "stat it (size %ld)", (long)st.st_size);
  char b[32];
  CK(get_text(f, b, sizeof b) == 9 && strcmp(b, "deep-data") == 0, "read it back");
  CK(access(f, R_OK) == 0, "access on the long path");
  CK(chmod(f, 0640) == 0 && stat(f, &st) == 0 && (st.st_mode & 0777) == 0640, "chmod on it");
  char l[1500];
  snprintf(l, sizeof l, "%s/abs.link", p);
  CK(symlink(f, l) == 0, "symlink with a %zu-byte absolute target", strlen(f));
  char t[1500];
  ssize_t tn = readlink(l, t, sizeof t);
  CK(tn == (ssize_t)strlen(f) && memcmp(t, f, (size_t)tn) == 0, "readlink returns it whole (%zd)",
     tn);
  CK(get_text(l, b, sizeof b) == 9, "open through it");
  /* a 256+-byte name relative to a dirfd */
  int dfd = open(X, O_RDONLY | O_DIRECTORY);
  const char *rel = p + strlen(X) + 1;
  char rf[1500];
  snprintf(rf, sizeof rf, "%s/leaf_file.txt", rel);
  int rfd = openat(dfd, rf, O_RDONLY);
  CK(rfd >= 0, "openat(dirfd, %zu-byte relative path)", strlen(rf));
  close(rfd);
  close(dfd);
  char ren[1500];
  snprintf(ren, sizeof ren, "%s/renamed.txt", p);
  CK(rename(f, ren) == 0 && exists(ren) && !exists(f), "rename inside the long path");
  /* ".." inside a long path */
  char dd[1500];
  snprintf(dd, sizeof dd, "%s/../%s/renamed.txt", p, strrchr(p, '/') + 1);
  CK(stat(dd, &st) == 0 && st.st_size == 9, "'..' inside a long path resolves");
  /* the limit is refused, never truncated */
  static char too[2200];
  int k = snprintf(too, sizeof too, "%s", p);
  while (k < 1100) too[k++] = 'z';
  too[k] = 0;
  errno = 0;
  CK(open(too, O_CREAT | O_WRONLY, 0644) == -1 && errno == ENAMETOOLONG,
     "a %d-byte path -> ENAMETOOLONG", k);
  CK(unlink(ren) == 0 && unlink(l) == 0, "unlink both");
  row("[LP] info longest path %d bytes\n", n);
}

/* ===================================================================== */
/* locks (main side; the helper is helper_main below)                    */
/* ===================================================================== */

static int setlk(int fd, int cmd, int type, off_t start, off_t len) {
  struct flock fl;
  memset(&fl, 0, sizeof fl);
  fl.l_type = (short)type;
  fl.l_whence = SEEK_SET;
  fl.l_start = start;
  fl.l_len = len;
  return fcntl(fd, cmd, &fl);
}

static int getlk(int fd, int cmd, int type, off_t start, off_t len, struct flock *out) {
  memset(out, 0, sizeof *out);
  out->l_type = (short)type;
  out->l_whence = SEEK_SET;
  out->l_start = start;
  out->l_len = len;
  return fcntl(fd, cmd, out);
}

static void sec_locks_main(void) {
  char d[512];
  snprintf(d, sizeof d, "%s/lk", X);
  mkdir(d, 0755);
  const char *lf = P("lk/lockfile");
  int fd = open(lf, O_CREAT | O_RDWR, 0644);
  CK(fd >= 0, "open lockfile");
  struct flock fl;
  CK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0, "F_SETLK WRLCK [0,1)");
  CK(getlk(fd, F_GETLK, F_WRLCK, 0, 1, &fl) == 0 && fl.l_type == F_UNLCK,
     "F_GETLK on our own lock -> F_UNLCK");
  int ffd = open(lf, O_RDWR);
  CK(ffd >= 0 && flock(ffd, LOCK_EX) == 0, "flock LOCK_EX (separate open)");
  CK(setlk(fd, F_SETLK, F_WRLCK, 500, 1) == 0, "flock and fcntl locks are independent");
  setlk(fd, F_SETLK, F_UNLCK, 500, 1);

  /* range split, observed through a second owner (an OFD lock of our own
     process conflicts with our POSIX locks: different owners) */
  int ofd2 = open(lf, O_RDWR);
  CK(setlk(fd, F_SETLK, F_WRLCK, 200, 100) == 0 && setlk(fd, F_SETLK, F_UNLCK, 240, 20) == 0,
     "WRLCK [200,300) then UNLCK [240,260)");
  CK(getlk(ofd2, F_OFD_GETLK, F_WRLCK, 250, 1, &fl) == 0 && fl.l_type == F_UNLCK,
     "OFD_GETLK 250 -> unlocked hole");
  CK(getlk(ofd2, F_OFD_GETLK, F_WRLCK, 210, 1, &fl) == 0 && fl.l_type == F_WRLCK &&
         fl.l_start == 200 && fl.l_len == 40 && fl.l_pid == getpid(),
     "OFD_GETLK 210 -> [%ld,+%ld) pid %d", (long)fl.l_start, (long)fl.l_len, fl.l_pid);
  CK(getlk(ofd2, F_OFD_GETLK, F_RDLCK, 280, 1, &fl) == 0 && fl.l_start == 260 && fl.l_len == 40,
     "OFD_GETLK 280 -> [%ld,+%ld)", (long)fl.l_start, (long)fl.l_len);
  CK(setlk(fd, F_SETLK, F_UNLCK, 0x100, 0) == 0, "UNLCK [256, EOF)");
  CK(getlk(ofd2, F_OFD_GETLK, F_WRLCK, 220, 1, &fl) == 0 && fl.l_start == 200 && fl.l_len == 40,
     "  [200,240) survives");
  setlk(fd, F_SETLK, F_UNLCK, 200, 40);
  /* OFD lock vs our own POSIX lock */
  CK(setlk(ofd2, F_OFD_SETLK, F_WRLCK, 10, 10) == 0, "F_OFD_SETLK [10,20) on a 2nd open");
  errno = 0;
  CK(setlk(fd, F_SETLK, F_WRLCK, 15, 1) == -1 && errno == EAGAIN,
     "our own POSIX lock conflicts with it -> EAGAIN");
  CK(getlk(fd, F_GETLK, F_WRLCK, 15, 1, &fl) == 0 && fl.l_type == F_WRLCK && fl.l_pid == -1,
     "F_GETLK reports the OFD lock with l_pid -1 (%d)", fl.l_pid);
  struct flock bad;
  memset(&bad, 0, sizeof bad);
  bad.l_type = F_WRLCK;
  bad.l_pid = 5;
  errno = 0;
  CK(fcntl(ofd2, F_OFD_SETLK, &bad) == -1 && errno == EINVAL, "OFD with l_pid != 0 -> EINVAL");
  close(ofd2); /* the description's last close drops the OFD lock */
  CK(setlk(fd, F_SETLK, F_WRLCK, 15, 1) == 0, "after close(): the OFD lock is gone");
  setlk(fd, F_SETLK, F_UNLCK, 15, 1);
  /* access-mode rules */
  int ro = open(lf, O_RDONLY);
  errno = 0;
  CK(setlk(ro, F_SETLK, F_WRLCK, 900, 1) == -1 && errno == EBADF, "WRLCK on O_RDONLY fd -> EBADF");
  CK(setlk(ro, F_SETLK, F_RDLCK, 900, 1) == 0, "RDLCK on O_RDONLY fd");
  /* negative l_len covers [start+len, start) */
  CK(setlk(fd, F_SETLK, F_WRLCK, 1000, -10) == 0, "WRLCK start 1000 len -10");
  close(ro); /* closing ANY fd of the file drops ALL our POSIX locks on it */
  CK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0, "re-take [0,1) after close() released it");

  /* ---- two processes ------------------------------------------------- */
  char pidtxt[64];
  snprintf(pidtxt, sizeof pidtxt, "%d\n", getpid());
  CK(put_text(P("lk/stage1"), pidtxt) == 0, "stage1: holding [0,1) WRLCK + flock EX");
  int h1 = wait_for(P("lk/h1"), 90000);
  CK(h1, "helper reached h1 (EAGAIN/GETLK/flock NB rows) and is about to block");
  if (!h1) goto out;
  sleep_ms(400); /* the helper is parked in F_SETLKW [0,1) */
  /* EDEADLK: the helper holds [100,101) and waits for our [0,1) */
  errno = 0;
  int64_t td = now_ms();
  CK(setlk(fd, F_SETLKW, F_WRLCK, 100, 1) == -1 && errno == EDEADLK,
     "F_SETLKW on the helper's [100,101) -> EDEADLK (%lld ms)", (long long)(now_ms() - td));
  int64_t t_unlock = now_ms();
  char tb[64];
  snprintf(tb, sizeof tb, "%lld\n", (long long)t_unlock);
  CK(setlk(fd, F_SETLK, F_UNLCK, 0, 1) == 0, "unlock [0,1): the parked helper must wake");
  int h2 = wait_for(P("lk/h2"), 30000);
  char hb[128] = "";
  get_text(P("lk/h2"), hb, sizeof hb);
  if (strchr(hb, '\n')) *strchr(hb, '\n') = 0;
  CK(h2, "helper's F_SETLKW returned after our unlock (%lld ms) [%s]",
     (long long)(now_ms() - t_unlock), hb);
  if (!h2) goto out;
  sleep_ms(300); /* helper now blocks in flock(LOCK_EX) */
  CK(!exists(P("lk/h3")), "helper still blocked in flock(LOCK_EX)");
  int64_t t_fl = now_ms();
  close(ffd); /* last close of the description -> flock released */
  ffd = -1;
  int h3 = wait_for(P("lk/h3"), 30000);
  CK(h3, "helper's blocking flock returned after our close (%lld ms)",
     (long long)(now_ms() - t_fl));
  if (!h3) goto out;
  /* the helper exits holding [0,1) and its flock: both must be released */
  int64_t te = now_ms();
  CK(setlk(fd, F_SETLKW, F_WRLCK, 0, 1) == 0,
     "F_SETLKW [0,1) returned once the helper EXITED holding it (%lld ms)",
     (long long)(now_ms() - te));
  int nf = open(lf, O_RDWR);
  CK(nf >= 0 && flock(nf, LOCK_EX | LOCK_NB) == 0, "flock EX NB succeeds: exit released it");
  close(nf);
  CK(getlk(fd, F_GETLK, F_WRLCK, 100, 1, &fl) == 0 && fl.l_type == F_UNLCK,
     "the helper's [100,101) is gone too");
  char hrc[32] = "";
  CK(wait_for(P("lk/hrc"), 10000) && get_text(P("lk/hrc"), hrc, sizeof hrc) > 0,
     "helper result file [%s]", hrc);
  int hf = atoi(hrc);
  CK(hf == 0, "helper reported %d failures", hf);
out:
  if (ffd >= 0) close(ffd);
  close(fd);
}

static void helper_main(void) {
  const char *lf = P("lk/lockfile");
  if (!wait_for(P("lk/stage1"), 120000)) {
    CK(0, "stage1 never appeared");
    goto done;
  }
  char pidtxt[64] = "";
  get_text(P("lk/stage1"), pidtxt, sizeof pidtxt);
  int mpid = atoi(pidtxt);
  int fd = open(lf, O_RDWR);
  CK(fd >= 0, "open lockfile");
  errno = 0;
  CK(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == -1 && (errno == EAGAIN || errno == EACCES),
     "F_SETLK on the main's [0,1) -> EAGAIN");
  struct flock fl;
  CK(getlk(fd, F_GETLK, F_WRLCK, 0, 1, &fl) == 0 && fl.l_type == F_WRLCK && fl.l_pid == mpid &&
         fl.l_start == 0 && fl.l_len == 1,
     "F_GETLK names the holder: pid %d (main %d) [%ld,+%ld)", fl.l_pid, mpid, (long)fl.l_start,
     (long)fl.l_len);
  int ffd = open(lf, O_RDWR);
  errno = 0;
  CK(flock(ffd, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "flock EX|NB -> EWOULDBLOCK");
  CK(flock(ffd, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK, "flock SH|NB -> EWOULDBLOCK");
  CK(setlk(fd, F_SETLK, F_WRLCK, 100, 1) == 0, "take [100,101) (the deadlock bait)");
  CK(put_text(P("lk/h1"), "h1\n") == 0, "h1 written; blocking in F_SETLKW [0,1)");
  int64_t t0 = now_ms();
  int r = setlk(fd, F_SETLKW, F_WRLCK, 0, 1);
  char b[96];
  snprintf(b, sizeof b, "rc=%d waited=%lld", r, (long long)(now_ms() - t0));
  CK(r == 0 && now_ms() - t0 >= 300, "F_SETLKW blocked, then got [0,1) (%s)", b);
  strcat(b, "\n");
  put_text(P("lk/h2"), b);
  t0 = now_ms();
  r = flock(ffd, LOCK_EX);
  CK(r == 0 && now_ms() - t0 >= 200, "blocking flock(LOCK_EX) got it after %lld ms",
     (long long)(now_ms() - t0));
  put_text(P("lk/h3"), "h3\n");
done:
  ;
  char rc[32];
  snprintf(rc, sizeof rc, "%d\n", fails);
  put_text(P("lk/hrc"), rc);
  row("[LP] helper done rc=%d (exiting WITH [0,1), [100,101) and the flock held)\n", fails);
  _exit(0); /* no unlock, no close: the kernel must release on exit */
}

/* ===================================================================== */
/* exec markers                                                          */
/* ===================================================================== */

static void sec_exec(void) {
  const char *tags[2] = {"static", "dyn"};
  for (int i = 0; i < 2; i++) {
    char m[512], b[400] = "";
    snprintf(m, sizeof m, "%s/out/%s.txt", X, tags[i]);
    int ok = wait_for(m, 60000) && get_text(m, b, sizeof b) > 0;
    char *nl = strchr(b, '\n');
    if (nl) *nl = 0;
    if (i == 0 && !ok) {
      /* Open item (docs/cp-ext2b.md section 5): the kernel's compat loader
         refuses a glibc static-pie on EVERY filesystem (its 24
         R_X86_64_IRELATIVE relocations are ELF_ERR_RELOC), so this one is
         reported, not graded; the gate checks that its bytes were read from
         ext2.  The dynamic glibc program below is graded. */
      row("[LP] info glibc static-pie from %s/bin: no marker (loader refuses "
          "glibc static-pie on any filesystem: open item)\n", X);
      continue;
    }
    CK(ok && strncmp(b, "ok ", 3) == 0 && strstr(b, "libc=2.31"),
       "glibc %s ELF ran from %s/bin: [%s]", tags[i], X, b);
    if (i == 1)
      CK(ok && strstr(b, "/lpx/glibc/libc.so.6"),
         "dynamic: libc.so.6 was mapped from the ext2 volume");
  }
}

int main(int argc, char **argv) {
  snprintf(R, sizeof R, "%s", argc > 1 ? argv[1] : "/EXT");
  is_helper = argc > 2 && strcmp(argv[2], "helper") == 0;
  if (argc > 3) FAT = argv[3];
  snprintf(X, sizeof X, "%s/lpx", R);
  if (is_helper) {
    helper_main();
    return 0;
  }
  mkdir(X, 0755);
  sec_mmap();
  sec_attr();
  sec_path();
  sec_locks_main();
  sec_exec();
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
