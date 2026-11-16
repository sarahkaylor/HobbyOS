/* src/user/linux_probe_bigobj.c - Linux-ABI probe: VM objects far larger than
 * the old 64 MiB cap (lane C3-pfx, docs/cyberpunk.md §6 P3, docs/cp-pfx.md).
 *
 * A plain musl x86_64 program: the reference on a Linux host and the
 * measurement in HobbyOS.  Rows "[LP] ...", last row "[LP] done rc=<fails>".
 *
 * Sizes come from P0 (docs/cp-p0-host.md §5 row 5/6): Wine's largest memfd is
 * exactly 64.0 MiB, the largest MAP_SHARED mapping a 192 MiB GPU BO, and the
 * game's largest mapped FILE is Cyberpunk2077.exe (66,704,848 bytes) mapped
 * MAP_PRIVATE RW at offset 0x397a000 for 3,166,208 bytes (.pdata).  Every big
 * row here uses 200 MiB (> 192 MiB, > 3 x 64 MiB).
 *
 *   B1 memfd 200 MiB: ftruncate, MAP_SHARED RW touch of every page, a 2nd
 *      shared view and pread() see every store, a MAP_PRIVATE view reads
 *      through and its writes stay private
 *   B2 MAP_SHARED|MAP_ANONYMOUS 200 MiB: every page touched, a fork child
 *      reads every page and its stores reach the parent
 *   B3 ext2 file 200 MiB (argv[1], default /EXT; rows skipped when absent):
 *      written with write(), MAP_SHARED read of every page, MAP_PRIVATE RW
 *      copy-on-write, MAP_SHARED RW stores reach read() after msync
 *   B4 the Cyberpunk2077.exe shape on ext2: a 66,704,848-byte file, a
 *      MAP_PRIVATE RW view at 0x397a000 + 3,166,208 bytes placed MAP_FIXED
 *      over a PROT_NONE reservation (Wine's map_file_into_view), every page
 *   B5 64 MiB + 1 page memfd (one page past the old cap) maps and touches
 *   M  sysinfo() freeram around the 200 MiB memfd (info rows, not graded:
 *      HobbyOS counts free 32 MiB blocks; the frame-exact cost is the unit
 *      test test_vm_big_object_cost in src/kernel/vm_test.c)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096UL
#define BIG (200UL << 20)        /* 200 MiB */
#define NPG (BIG / PG)           /* 51,200 pages */
#define EXE_SIZE 66704848UL      /* Cyberpunk2077.exe (docs/cp-p0-host.md): 63.6 MiB */
#define EXE_OFF 0x397a000UL      /* its .pdata mapping */
#define EXE_LEN 3166208UL

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
static int mfd(const char *name) { return (int)syscall(SYS_memfd_create, name, 0); }

/* The word stored at the start of page i (and checked everywhere). */
static uint64_t tag(uint64_t seed, uint64_t i) { return (seed << 40) ^ (i * 0x9E3779B97F4A7C15ULL); }

static long freeram_kib(void) {
  struct sysinfo si;
  if (sysinfo(&si) != 0) return -1;
  return (long)((uint64_t)si.freeram * si.mem_unit / 1024);
}

/* Count pages of [p, p+n pages) whose first word != tag(seed, first+i). */
static long bad_pages(volatile uint64_t *p, uint64_t n, uint64_t seed, uint64_t first) {
  long bad = 0;
  for (uint64_t i = 0; i < n; i++)
    if (p[i * (PG / 8)] != tag(seed, first + i)) bad++;
  return bad;
}
static void stamp(volatile uint64_t *p, uint64_t n, uint64_t seed, uint64_t first) {
  for (uint64_t i = 0; i < n; i++) p[i * (PG / 8)] = tag(seed, first + i);
}

static void b1_memfd(void) {
  long f0 = freeram_kib();
  int fd = mfd("bigobj");
  ck(fd >= 0, "B1 memfd_create", fd < 0 ? errno : 0);
  if (fd < 0) return;
  int r = ftruncate(fd, (off_t)BIG);
  ck(r == 0, "B1 ftruncate 200 MiB", r ? errno : 0);
  if (r) { close(fd); return; }
  volatile uint64_t *s = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(s != MAP_FAILED, "B1 mmap 200 MiB MAP_SHARED RW", s == MAP_FAILED ? errno : 0);
  if (s == MAP_FAILED) { close(fd); return; }
  stamp(s, NPG, 1, 0);
  long f1 = freeram_kib();
  ck(bad_pages(s, NPG, 1, 0) == 0, "B1 every page of the shared view holds its store", bad_pages(s, NPG, 1, 0));
  volatile uint64_t *s2 = mmap(0, BIG, PROT_READ, MAP_SHARED, fd, 0);
  ck(s2 != MAP_FAILED && bad_pages(s2, NPG, 1, 0) == 0, "B1 a 2nd MAP_SHARED view sees every page",
     s2 == MAP_FAILED ? errno : bad_pages(s2, NPG, 1, 0));
  uint64_t w = 0;
  long pr = 0;
  for (uint64_t i = 0; i < NPG; i += 97)
    if (pread(fd, &w, 8, (off_t)(i * PG)) != 8 || w != tag(1, i)) pr++;
  pr += (pread(fd, &w, 8, (off_t)(BIG - PG)) != 8 || w != tag(1, NPG - 1));
  ck(pr == 0, "B1 pread() sees the stores (every 97th page + the last)", pr);
  volatile uint64_t *p = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "B1 mmap 200 MiB MAP_PRIVATE RW", p == MAP_FAILED ? errno : 0);
  if (p != MAP_FAILED) {
    ck(bad_pages(p, NPG, 1, 0) == 0, "B1 private view reads every page through", bad_pages(p, NPG, 1, 0));
    p[(NPG - 1) * (PG / 8)] = 0xABCDULL;
    p[(NPG / 2) * (PG / 8)] = 0x1234ULL;
    ck(s[(NPG - 1) * (PG / 8)] == tag(1, NPG - 1) && s[(NPG / 2) * (PG / 8)] == tag(1, NPG / 2) &&
       p[(NPG - 1) * (PG / 8)] == 0xABCDULL,
       "B1 private writes (last + middle page) stay private", (long)p[(NPG - 1) * (PG / 8)]);
    s[(NPG - 2) * (PG / 8)] = 0x77ULL;
    ck(p[(NPG - 2) * (PG / 8)] == 0x77ULL, "B1 untouched private page sees a later shared store",
       (long)p[(NPG - 2) * (PG / 8)]);
    munmap((void *)p, BIG);
  }
  long tail = (long)(f0 - f1) - (long)(NPG * PG / 1024);
  out("[LP] info M memfd 200 MiB: freeram before=%ld KiB after-touch=%ld KiB delta=%ld KiB, data=%lu KiB, "
      "overhead=%ld KiB (frame array + page tables)\n", f0, f1, f0 - f1, NPG * PG / 1024, tail);
  if (s2 != MAP_FAILED) munmap((void *)s2, BIG);
  munmap((void *)s, BIG);
  close(fd);
  long f2 = freeram_kib();
  /* info only: HobbyOS reports freeram in whole 32 MiB physical blocks (a
     block whose frames came back to its free list still counts as used), so
     the exact cost is measured by the unit test test_vm_big_object_cost. */
  out("[LP] info M memfd 200 MiB released: freeram=%ld KiB (before=%ld)\n", f2, f0);
}

static void b2_anon(void) {
  volatile uint64_t *s = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ck(s != MAP_FAILED, "B2 mmap 200 MiB MAP_SHARED|MAP_ANONYMOUS", s == MAP_FAILED ? errno : 0);
  if (s == MAP_FAILED) return;
  stamp(s, NPG, 2, 0);
  ck(bad_pages(s, NPG, 2, 0) == 0, "B2 every page holds its store", bad_pages(s, NPG, 2, 0));
  pid_t c = fork();
  if (c == 0) {
    long bad = bad_pages(s, NPG, 2, 0);
    stamp(s, NPG, 3, 0);
    _exit(bad ? 1 : 0);
  }
  int st = 0;
  waitpid(c, &st, 0);
  ck(c > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "B2 fork child reads every page", st);
  ck(bad_pages(s, NPG, 3, 0) == 0, "B2 the child's stores reach the parent on every page", bad_pages(s, NPG, 3, 0));
  munmap((void *)s, BIG);
}

/* Write a file of `size` bytes whose page i starts with tag(seed, i). */
static int write_file(const char *path, uint64_t size, uint64_t seed) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  static uint64_t chunk[(1UL << 20) / 8];
  for (uint64_t off = 0; off < size; off += sizeof chunk) {
    memset(chunk, 0x5A, sizeof chunk);
    for (uint64_t i = 0; i < sizeof chunk / PG; i++) chunk[i * (PG / 8)] = tag(seed, off / PG + i);
    uint64_t n = size - off < sizeof chunk ? size - off : sizeof chunk;
    if (write(fd, chunk, n) != (ssize_t)n) { close(fd); return -1; }
  }
  return fd;
}

static void b3_ext2(const char *root) {
  char f[160];
  snprintf(f, sizeof f, "%s/bigobj.bin", root);
  int fd = write_file(f, BIG, 4);
  ck(fd >= 0, "B3 ext2 file 200 MiB written", fd < 0 ? errno : 0);
  if (fd < 0) return;
  volatile uint64_t *s = mmap(0, BIG, PROT_READ, MAP_SHARED, fd, 0);
  ck(s != MAP_FAILED, "B3 ext2 mmap 200 MiB MAP_SHARED RO", s == MAP_FAILED ? errno : 0);
  if (s != MAP_FAILED) {
    ck(bad_pages(s, NPG, 4, 0) == 0, "B3 every page of the shared view = the file", bad_pages(s, NPG, 4, 0));
    munmap((void *)s, BIG);
  }
  volatile uint64_t *p = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "B3 ext2 mmap 200 MiB MAP_PRIVATE RW", p == MAP_FAILED ? errno : 0);
  if (p != MAP_FAILED) {
    ck(bad_pages(p, NPG, 4, 0) == 0, "B3 every page of the private view = the file", bad_pages(p, NPG, 4, 0));
    stamp(p, NPG, 5, 0);
    uint64_t w = 0;
    ck(bad_pages(p, NPG, 5, 0) == 0 && pread(fd, &w, 8, (off_t)(BIG - PG)) == 8 && w == tag(4, NPG - 1),
       "B3 private stores on every page, the file unchanged", (long)bad_pages(p, NPG, 5, 0));
    munmap((void *)p, BIG);
  }
  volatile uint64_t *rw = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(rw != MAP_FAILED, "B3 ext2 mmap 200 MiB MAP_SHARED RW", rw == MAP_FAILED ? errno : 0);
  if (rw != MAP_FAILED) {
    rw[(NPG - 1) * (PG / 8)] = 0xFEEDULL;
    rw[(NPG / 3) * (PG / 8)] = 0xBEEFULL;
    int m = msync((void *)rw, BIG, MS_SYNC);
    uint64_t a = 0, b = 0;
    (void)!pread(fd, &a, 8, (off_t)(BIG - PG));
    (void)!pread(fd, &b, 8, (off_t)((NPG / 3) * PG));
    ck(m == 0 && a == 0xFEEDULL && b == 0xBEEFULL, "B3 shared stores reach read() after msync", (long)a);
    munmap((void *)rw, BIG);
  }
  close(fd);
  unlink(f);
}

static void b4_exe_shape(const char *root) {
  char f[160];
  snprintf(f, sizeof f, "%s/cp2077shape.bin", root);
  int fd = write_file(f, EXE_SIZE, 6);
  ck(fd >= 0, "B4 ext2 file of Cyberpunk2077.exe's size (66,704,848 B)", fd < 0 ? errno : 0);
  if (fd < 0) return;
  /* Wine: reserve, then map the section MAP_FIXED into the reservation */
  uint8_t *res = mmap(0, EXE_LEN + 2 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  ck(res != MAP_FAILED, "B4 PROT_NONE reservation", res == MAP_FAILED ? errno : 0);
  if (res == MAP_FAILED) { close(fd); unlink(f); return; }
  volatile uint64_t *v = mmap(res + PG, EXE_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)EXE_OFF);
  ck(v == (void *)(res + PG), "B4 MAP_PRIVATE|MAP_FIXED RW view at 0x397a000 + 3,166,208", v == MAP_FAILED ? errno : 0);
  if (v == (void *)(res + PG)) {
    uint64_t n = EXE_LEN / PG; /* 773 whole pages */
    ck(bad_pages(v, n, 6, EXE_OFF / PG) == 0, "B4 every page of the view = the file at its offset",
       bad_pages(v, n, 6, EXE_OFF / PG));
    stamp(v, n, 7, 0);
    uint64_t w = 0;
    ck(bad_pages(v, n, 7, 0) == 0 && pread(fd, &w, 8, (off_t)EXE_OFF) == 8 && w == tag(6, EXE_OFF / PG),
       "B4 relocation-style writes stay private", (long)n);
  }
  munmap(res, EXE_LEN + 2 * PG);
  close(fd);
  unlink(f);
}

static void b5_just_over(void) {
  uint64_t sz = (64UL << 20) + PG;
  int fd = mfd("cap+1");
  int r = fd >= 0 ? ftruncate(fd, (off_t)sz) : -1;
  ck(r == 0, "B5 memfd 64 MiB + 1 page", r ? errno : 0);
  if (r) { if (fd >= 0) close(fd); return; }
  volatile uint64_t *s = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(s != MAP_FAILED, "B5 mmap 64 MiB + 1 page MAP_SHARED", s == MAP_FAILED ? errno : 0);
  if (s != MAP_FAILED) {
    stamp(s, sz / PG, 8, 0);
    ck(bad_pages(s, sz / PG, 8, 0) == 0, "B5 every page incl. the one past 64 MiB", bad_pages(s, sz / PG, 8, 0));
    munmap((void *)s, sz);
  }
  close(fd);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : "/EXT";
  out("[LP] bigobj probe (C3-pfx): 200 MiB objects, root=%s\n", root);
  b5_just_over();
  b1_memfd();
  b2_anon();
  struct stat st;
  if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP B3/B4 ext2: %s absent\n", root);
  } else {
    b3_ext2(root);
    b4_exe_shape(root);
  }
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
