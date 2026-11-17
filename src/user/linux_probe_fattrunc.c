/* src/user/linux_probe_fattrunc.c - Linux-ABI probe: ftruncate / O_TRUNC of
 * a FAT16 file that is mapped (lane C3-ext2w, docs/cp-ext2w.md section 4;
 * docs/cyberpunk.md §0.2 item 18, "open: ftruncate/O_TRUNC of a mapped FAT
 * file").
 *
 * Linux semantics (one page cache): after a shrink, a mapped page wholly
 * past the new end is SIGBUS when touched (shared views and private
 * read-through pages; private COW copies past the cut are discarded too),
 * the page holding the new end reads zeros after it, the pages before it
 * keep their bytes, and an extension reads zeros.  A plain musl program: on
 * a Linux host it runs against any directory (argv[1]); in HobbyOS against
 * the FAT boot volume ("/").  Touches that must die run in a forked child.
 *
 * Rows "[LP] PASS|FAIL ...", last row "[LP] done rc=<fails>".
 *   T1 MAP_SHARED view, shrink to 5000: page 0 kept, page 1 zero past 5000,
 *      page 2 (resident before) SIGBUS
 *   T2 MAP_PRIVATE read-only view: same; a page never touched before the
 *      shrink and past the end is SIGBUS too
 *   T3 private COW copy below the cut kept, past the cut discarded (SIGBUS)
 *   T4 regrow to 3 pages: the cut range reads zeros in both views
 *   T5 open(O_TRUNC) while mapped: every page SIGBUS
 *   T6 the file's own bytes agree (read() after the shrink)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096
#define FSIZE (3 * PG)

static int fails;
static char path[200];

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

static int mkfile(void) {
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  char b[FSIZE];
  for (int i = 0; i < FSIZE; i++) b[i] = (char)('a' + i / PG);
  if (write(fd, b, FSIZE) != FSIZE) { close(fd); return -1; }
  return fd;
}

/* Fork: the child reads p[off]; returns the signal that ended it, or
   1000 + the byte it read. */
static int touch_child(volatile char *p, long off) {
  pid_t k = fork();
  if (k == 0) {
    char c = p[off];
    _exit((unsigned char)c == 0 ? 200 : (unsigned char)c & 0x7F);
  }
  int st = 0;
  if (k < 0 || waitpid(k, &st, 0) != k) return -1;
  if (WIFSIGNALED(st)) return WTERMSIG(st);
  return 1000 + WEXITSTATUS(st);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "/";
  snprintf(path, sizeof path, "%s%sFATTRUNC.BIN", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
  out("[LP] fattrunc probe (C3-ext2w): file %s\n", path);

  /* T1-T4 on one file with three views */
  int fd = mkfile();
  ck(fd >= 0, "setup 12288-byte file (pages a/b/c)", fd < 0 ? errno : 0);
  if (fd < 0) { out("[LP] done rc=%d\n", fails); return 1; }
  volatile char *s = mmap(0, FSIZE, PROT_READ, MAP_SHARED, fd, 0);
  volatile char *r = mmap(0, FSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  volatile char *w = mmap(0, FSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(s != MAP_FAILED && r != MAP_FAILED && w != MAP_FAILED, "setup three mappings", errno);
  if (s == MAP_FAILED || r == MAP_FAILED || w == MAP_FAILED) { out("[LP] done rc=%d\n", fails); return 1; }
  /* resident before the shrink: shared pages 0,1,2; private RO pages 0,1
     (page 2 never touched); private RW page 0 and 2 COW-written */
  char pre = (char)(s[0] + s[PG] + s[2 * PG] + r[0] + r[PG] + w[PG]);
  (void)pre;
  w[0] = 'X';
  w[2 * PG] = 'Y';
  ck(s[2 * PG] == 'c' && r[PG] == 'b' && w[0] == 'X', "setup views resident", s[2 * PG]);

  ck(ftruncate(fd, 5000) == 0, "T1 ftruncate 12288 -> 5000", errno);
  ck(s[0] == 'a' && s[4999 - PG + PG] == 'b', "T1 shared: bytes before the new end kept", s[0]);
  ck(s[5000] == 0 && s[PG + 4095] == 0, "T1 shared: page 1 reads zeros after 5000", s[5000]);
  int sig = touch_child(s, 2 * PG);
  ck(sig == SIGBUS, "T1 shared: resident page 2 is SIGBUS", sig);
  ck(r[0] == 'a' && r[4999] == 'b' && r[5000] == 0, "T2 private RO: page 1 truncated content", r[5000]);
  sig = touch_child(r, 2 * PG);
  ck(sig == SIGBUS, "T2 private RO: untouched page 2 is SIGBUS", sig);
  ck(w[0] == 'X' && w[PG] == 'b' && w[5000] == 0, "T3 private RW: COW page 0 kept, page 1 reads through", w[0]);
  sig = touch_child(w, 2 * PG);
  ck(sig == SIGBUS, "T3 private RW: COW copy past the cut discarded (SIGBUS)", sig);

  char rb[16];
  ck(pread(fd, rb, 16, 4990) == 10 && rb[9] == 'b', "T6 read() ends at 5000", rb[9]);

  ck(ftruncate(fd, FSIZE) == 0, "T4 regrow to 12288", errno);
  ck(s[5000] == 0 && s[2 * PG] == 0 && s[FSIZE - 1] == 0, "T4 shared: regrown range reads zeros", s[2 * PG]);
  ck(r[2 * PG] == 0 && w[2 * PG] == 0, "T4 private views: regrown page 2 reads zeros", r[2 * PG]);
  (void)!pwrite(fd, "N", 1, 2 * PG + 7);
  ck(s[2 * PG + 7] == 'N', "T4 shared: a write() into the regrown page is seen", s[2 * PG + 7]);
  munmap((void *)s, FSIZE);
  munmap((void *)r, FSIZE);
  munmap((void *)w, FSIZE);
  close(fd);

  /* T5: O_TRUNC while mapped */
  fd = mkfile();
  s = fd >= 0 ? mmap(0, FSIZE, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
  ck(s != MAP_FAILED && s[0] == 'a' && s[PG] == 'b', "T5 setup shared view resident", errno);
  if (s != MAP_FAILED) {
    int fd2 = open(path, O_RDWR | O_TRUNC);
    ck(fd2 >= 0, "T5 open(O_TRUNC) of the mapped file", errno);
    sig = touch_child(s, 0);
    ck(sig == SIGBUS, "T5 page 0 is SIGBUS after O_TRUNC", sig);
    sig = touch_child(s, PG);
    ck(sig == SIGBUS, "T5 page 1 is SIGBUS after O_TRUNC", sig);
    struct stat st;
    long sz = (fd2 >= 0 && fstat(fd2, &st) == 0) ? (long)st.st_size : -1;
    ck(sz == 0, "T5 size 0", sz);
    if (fd2 >= 0) close(fd2);
    munmap((void *)s, FSIZE);
  }
  if (fd >= 0) close(fd);
  unlink(path);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
