/* src/user/linux_probe_fatcoh.c - Linux-ABI probe: a MAP_PRIVATE page that the
 * mapper has not written reflects later write()s to the file (lane C3-pfx,
 * docs/cyberpunk.md §0.2 item 18 "FAT private mappings miss later write()s").
 *
 * A plain musl x86_64 program: the reference on a Linux host and the
 * measurement in HobbyOS.  The file lives in argv[1] (default "/", which is
 * the FAT16 boot volume for a compat process in HobbyOS).  Rows "[LP] ...",
 * last row "[LP] done rc=<fails>".
 *
 *   F1 a private page already faulted in (read) sees a later pwrite()/write()
 *   F2 a private page never touched sees it too
 *   F3 a page the mapper wrote is private: later writes do not show, the file
 *      has them (pread)
 *   F4 a read-only MAP_SHARED view sees every later write
 *   F5 a write that extends the file inside the mapped last page shows
 *   F6 a mapping made before the write and one made after agree
 *   F7 a fork child's untouched private page sees the parent's later write
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096
#define FSIZE 10000 /* 3 pages, the last one partial */

static int fails;

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
static char fbyte(int fd, off_t off) {
  char c = 0;
  if (pread(fd, &c, 1, off) != 1) return (char)0xEE;
  return c;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "/";
  char path[160];
  snprintf(path, sizeof path, "%s%sFATCOH.BIN", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
  out("[LP] fatcoh probe (C3-pfx): file %s\n", path);
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0, "setup create", fd < 0 ? errno : 0);
  if (fd < 0) { out("[LP] done rc=%d\n", fails); return 1; }
  char b[FSIZE];
  for (int i = 0; i < FSIZE; i++) b[i] = (char)('a' + i / PG);
  ck(write(fd, b, FSIZE) == FSIZE, "setup write 10000 bytes (a/b/c pages)", errno);

  volatile char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "setup mmap MAP_PRIVATE RW 3 pages", p == MAP_FAILED ? errno : 0);
  volatile char *s = mmap(0, 3 * PG, PROT_READ, MAP_SHARED, fd, 0);
  ck(s != MAP_FAILED, "setup mmap MAP_SHARED RO 3 pages", s == MAP_FAILED ? errno : 0);
  if (p == MAP_FAILED || s == MAP_FAILED) { out("[LP] done rc=%d\n", fails); return 1; }

  /* fault pages 0 and 2 in (read only); page 1 stays untouched */
  ck(p[0] == 'a' && p[2 * PG] == 'c' && s[0] == 'a', "setup read faults pages 0 and 2", p[0]);

  /* F1 */
  (void)!pwrite(fd, "PPPP", 4, 100);
  ck(p[100] == 'P' && p[103] == 'P', "F1 faulted private page sees a later pwrite()", p[100]);
  (void)!lseek(fd, 2 * PG + 50, SEEK_SET);
  (void)!write(fd, "WW", 2);
  ck(p[2 * PG + 50] == 'W', "F1 faulted private page sees a later write()", p[2 * PG + 50]);

  /* F2 */
  (void)!pwrite(fd, "QQ", 2, PG + 7);
  ck(p[PG + 7] == 'Q' && p[PG] == 'b', "F2 untouched private page sees a later pwrite()", p[PG + 7]);

  /* F3 */
  p[200] = 'M';
  (void)!pwrite(fd, "RR", 2, 300);
  ck(p[200] == 'M' && p[300] == 'a' && p[100] == 'P', "F3 written page is private: later write not shown",
     p[300]);
  ck(fbyte(fd, 200) == 'a' && fbyte(fd, 300) == 'R', "F3 the file has the write, not the private store",
     fbyte(fd, 300));

  /* F4 */
  ck(s[100] == 'P' && s[300] == 'R' && s[PG + 7] == 'Q' && s[2 * PG + 50] == 'W' && s[200] == 'a',
     "F4 MAP_SHARED RO view sees every write", s[300]);

  /* F5: extend the file inside the mapped last page */
  ck(p[FSIZE + 5] == 0, "F5 before: past-EOF bytes of the last page read 0", p[FSIZE + 5]);
  (void)!pwrite(fd, "EXT", 3, FSIZE + 5);
  ck(p[FSIZE + 5] == 'E' && p[FSIZE + 7] == 'T' && s[FSIZE + 5] == 'E',
     "F5 an extending write shows in the mapped last page", p[FSIZE + 5]);

  /* F6: a mapping made now agrees with the old one */
  volatile char *n = mmap(0, 3 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
  ck(n != MAP_FAILED && n[100] == 'P' && n[300] == 'R' && n[PG + 7] == 'Q', "F6 new mapping has the writes",
     n == MAP_FAILED ? errno : n[300]);
  if (n != MAP_FAILED) {
    (void)!pwrite(fd, "ZZ", 2, 400);
    ck(n[400] == 'Z' && p[400] == 'a' && s[400] == 'Z', "F6 old and new views agree after one more write",
       n[400]);
    munmap((void *)n, 3 * PG);
  }

  /* F7: fork child, parent writes, child's untouched page shows it */
  int pp[2];
  ck(pipe(pp) == 0, "F7 pipe", errno);
  pid_t c = fork();
  if (c == 0) {
    char go;
    (void)!read(pp[0], &go, 1);
    _exit(p[PG + 100] == 'K' ? 0 : 1);
  }
  (void)!pwrite(fd, "K", 1, PG + 100);
  (void)!write(pp[1], "g", 1);
  int st = 0;
  waitpid(c, &st, 0);
  ck(c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "F7 fork child's private page sees the parent's write",
     st);

  munmap((void *)p, 3 * PG);
  munmap((void *)s, 3 * PG);
  close(fd);
  unlink(path);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
