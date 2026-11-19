/* src/user/linux_probe_lowva.c — Linux-ABI probe: the compat address space
 * (docs/cp-address-space.md F1, docs/cyberpunk.md P1).
 *
 * What Wine needs from the address space, row by row (Wine 11.0
 * dlls/ntdll/unix/virtual.c and loader/preloader.c, docs/cyberpunk-evidence.md):
 *   - fixed mappings at 0x10000 (DOS area) and 0x7ffe0000 (KUSER_SHARED_DATA);
 *   - KUSER is a MAP_SHARED memfd page the wineserver keeps writing, so two
 *     processes mapping the same memfd at 0x7ffe0000 must stay coherent;
 *   - MAP_FIXED_NOREPLACE over a mapping or over the kernel hole is EEXIST
 *     (never EINVAL: Wine's probe loop reads EINVAL as "address space ends"),
 *     MAP_FIXED into the hole is ENOMEM;
 *   - PROT_NONE reservations of 64 GiB singly and >= 141 GiB in total that
 *     commit nothing until mprotect + touch (the game's measured footprint);
 *   - the last page below 0x7ffffffff000 is mappable, that page is not.
 * Plus the C4-x row: a syscall that copies from / into a never-touched
 * FILE-backed page (writev from, readv into) must demand-fault it in the
 * kernel, not hang.
 * An ordinary musl static-PIE: the same source runs on a Linux host.  Rows
 * are "[LP] ...", the last one "[LP] done rc=<fails>". */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif

#define PAGE 4096UL
#define GiB (1024UL * 1024UL * 1024UL)
#define KHOLE_LO 0x70000000UL /* F1 kernel hole [0x70000000, 0x78000000) */
#define KUSER 0x7ffe0000UL
#define VA_HI 0x7ffffffff000UL

static int fails;

static void row(const char *fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static void check(int ok, const char *what, long detail) {
  if (!ok) fails++;
  row("[LP] %-40s %s detail=%ld\n", what, ok ? "ok" : "FAIL", detail);
}

static void *mm(uint64_t addr, uint64_t len, int prot, int flags, int fd) {
  return mmap((void *)addr, len, prot, flags, fd, 0);
}

/* errno of a mapping that must fail, 0 if it unexpectedly succeeded. */
static int mm_err(uint64_t addr, uint64_t len, int flags) {
  void *p = mm(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1);
  if (p != MAP_FAILED) {
    munmap(p, len);
    return 0;
  }
  return errno;
}

static void fixed_rw(uint64_t addr, const char *name) {
  void *p = mm(addr, PAGE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
  int ok = (p == (void *)addr);
  if (ok) {
    volatile uint32_t *w = (volatile uint32_t *)p;
    w[0] = 0x4c4f5741u;
    w[1023] = (uint32_t)addr;
    ok = (w[0] == 0x4c4f5741u && w[1023] == (uint32_t)addr);
  }
  char what[64];
  snprintf(what, sizeof what, "map fixed rw %s", name);
  check(ok, what, ok ? 0 : (p == MAP_FAILED ? errno : -1));
}

/* KUSER_SHARED_DATA shape: a memfd page at 0x7ffe0000, mapped independently
   in a parent and a forked child, written by one and read by the other. */
static void kuser_coherent(void) {
  int fd = (int)syscall(SYS_memfd_create, "lowva-kuser", 0);
  if (fd < 0 || ftruncate(fd, PAGE) != 0) {
    check(0, "memfd for 0x7ffe0000", errno);
    return;
  }
  volatile uint32_t *k = mm(KUSER, PAGE, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, fd);
  if (k != (void *)KUSER) {
    check(0, "memfd MAP_SHARED|MAP_FIXED at 0x7ffe0000", errno);
    return;
  }
  k[0] = 1;
  int p2c[2], c2p[2];
  if (pipe(p2c) || pipe(c2p)) {
    check(0, "pipes", errno);
    return;
  }
  pid_t c = fork();
  if (c == 0) {
    /* The child maps the SAME memfd at the same VA by itself (as a Wine
       client does with the fd the server sent), replacing the inherited one. */
    munmap((void *)KUSER, PAGE);
    volatile uint32_t *ck = mm(KUSER, PAGE, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_FIXED, fd);
    int ok = (ck == (void *)KUSER);
    char b;
    for (int round = 0; ok && round < 3; round++) {
      if (read(p2c[0], &b, 1) != 1) ok = 0;
      if (ok && ck[0] != (uint32_t)(100 + round)) ok = 0; /* parent's value */
      ck[1] = (uint32_t)(200 + round);                       /* child's value */
      if (write(c2p[1], "x", 1) != 1) ok = 0;
    }
    _exit(ok ? 0 : 3);
  }
  int ok = c > 0;
  char b;
  for (int round = 0; ok && round < 3; round++) {
    k[0] = (uint32_t)(100 + round);
    if (write(p2c[1], "x", 1) != 1 || read(c2p[0], &b, 1) != 1) ok = 0;
    if (ok && k[1] != (uint32_t)(200 + round)) ok = 0;
  }
  int st = 0;
  if (c > 0) waitpid(c, &st, 0);
  ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
  check(ok, "memfd 0x7ffe0000 coherent parent<->child", (long)st);
  close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
  close(fd);
}

/* 64 GiB PROT_NONE reservations, >= 141 GiB in total, nothing committed
   until mprotect + touch. */
static void reservations(void) {
  const uint64_t sz[3] = {64 * GiB, 64 * GiB, 16 * GiB};
  void *r[3];
  uint64_t total = 0;
  for (int i = 0; i < 3; i++) {
    r[i] = mm(0, sz[i], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1);
    if (r[i] == MAP_FAILED) {
      check(0, "PROT_NONE reservation", errno);
      r[i] = 0;
      continue;
    }
    total += sz[i];
    row("[LP] reserve %3lu GiB at %p\n", (unsigned long)(sz[i] / GiB), r[i]);
  }
  check(r[0] && r[1] && r[2], "reserve 64+64+16 GiB PROT_NONE", (long)(total / GiB));
  check(total >= 141 * GiB, "reserved total >= 141 GiB", (long)(total / GiB));
  if (!r[0]) return;
  /* Commit three pages of the first: start, middle, last. */
  uint64_t base = (uint64_t)r[0];
  uint64_t offs[3] = {0, 32 * GiB, 64 * GiB - PAGE};
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    void *pg = (void *)(base + offs[i]);
    if (mprotect(pg, PAGE, PROT_READ | PROT_WRITE) != 0) {
      ok = 0;
      row("[LP] mprotect +0x%lx errno=%d\n", (unsigned long)offs[i], errno);
      continue;
    }
    volatile uint64_t *w = pg;
    if (w[0] != 0) ok = 0; /* demand-zero */
    w[0] = 0x5245534552564531ULL + i;
    w[511] = offs[i];
    if (w[0] != 0x5245534552564531ULL + (uint64_t)i || w[511] != offs[i]) ok = 0;
  }
  check(ok, "mprotect+touch 3 pages of 64 GiB", 0);
  /* Unmapping the middle of the reservation splits it, the rest stays. */
  int um = munmap((void *)(base + 16 * GiB), 8 * GiB);
  volatile uint64_t *last = (void *)(base + 64 * GiB - PAGE);
  check(um == 0 && last[511] == 64 * GiB - PAGE, "munmap 8 GiB inside, tail intact", um);
  for (int i = 0; i < 3; i++)
    if (r[i]) munmap(r[i], sz[i]);
}

/* C4-x: kernel-mode copies from/into never-touched FILE-backed pages. */
static void file_backed_iov(void) {
  char exe[256];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) {
    strcpy(exe, "/PROBE.ELF");
  } else {
    exe[n] = 0;
  }
  int fd = open(exe, O_RDONLY);
  if (fd < 0) {
    check(0, "open own image", errno);
    return;
  }
  /* writev FROM a fresh read-only private file mapping, placed low. */
  const uint64_t lo = 0x20000000UL;
  unsigned char *m = mm(lo, 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd);
  if (m == MAP_FAILED) {
    /* A host Linux may have something there; any address proves the copy. */
    m = mm(0, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd);
  }
  if (m == MAP_FAILED) {
    check(0, "mmap own image PROT_READ", errno);
    close(fd);
    return;
  }
  int pp[2];
  if (pipe(pp)) {
    check(0, "pipe", errno);
    return;
  }
  struct iovec iov[2] = {{m + 0x10, 64}, {m + PAGE + 0x20, 64}};
  ssize_t w = writev(pp[1], iov, 2);
  unsigned char got[128], want[128];
  ssize_t r = read(pp[0], got, sizeof got);
  int ok = (w == 128 && r == 128 && pread(fd, want, 64, 0x10) == 64 &&
            pread(fd, want + 64, 64, PAGE + 0x20) == 64 &&
            memcmp(got, want, 128) == 0);
  check(ok, "writev from untouched file pages", (long)w);
  /* readv INTO a fresh writable private file mapping (kernel COW). */
  unsigned char *d = mm(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd);
  if (d == MAP_FAILED) {
    check(0, "mmap own image RW private", errno);
  } else {
    unsigned char pat[96];
    for (int i = 0; i < 96; i++) pat[i] = (unsigned char)(0xA0 ^ i);
    (void)!write(pp[1], pat, sizeof pat);
    struct iovec riov[2] = {{d + 0x100, 48}, {d + PAGE + 0x200, 48}};
    ssize_t rr = readv(pp[0], riov, 2);
    unsigned char orig[16];
    int ok2 = (rr == 96 && memcmp(d + 0x100, pat, 48) == 0 &&
               memcmp(d + PAGE + 0x200, pat + 48, 48) == 0);
    /* The page cache is untouched: the file still reads its own bytes. */
    ok2 = ok2 && pread(fd, orig, 16, 0x100) == 16 && memcmp(orig, pat, 16) != 0;
    check(ok2, "readv into untouched file pages (COW)", (long)rr);
  }
  close(pp[0]);
  close(pp[1]);
  close(fd);
}

int main(void) {
  row("[LP] lowva probe start pid=%ld\n", (long)getpid());
  /* SIGCHLD's default action must be "ignore"; on this kernel it still kills
     (fixed in lane C1-cpu, not merged yet), so keep it blocked: the forked
     child of the KUSER row is reaped with waitpid either way. */
  {
    sigset_t cs;
    sigemptyset(&cs);
    sigaddset(&cs, SIGCHLD);
    sigprocmask(SIG_BLOCK, &cs, 0);
  }

  fixed_rw(0x10000UL, "0x10000");
  fixed_rw(KUSER, "0x7ffe0000");
  /* Wine's other preloader ranges (low memory, shared user data, top-down). */
  fixed_rw(0x110000UL, "0x110000");
  fixed_rw(0x68000000UL - PAGE, "0x67fff000");
  fixed_rw(0x78000000UL, "0x78000000 (above hole)");
  fixed_rw(0x140000000UL, "0x140000000 (exe base)");
  fixed_rw(VA_HI - PAGE, "0x7fffffffe000 (last page)");

  check(mm_err(KUSER, PAGE, MAP_FIXED_NOREPLACE) == EEXIST,
        "NOREPLACE over a mapping -> EEXIST", errno);
  check(mm_err(KHOLE_LO, PAGE, MAP_FIXED_NOREPLACE) == EEXIST,
        "NOREPLACE into the kernel hole -> EEXIST", errno);
  check(mm_err(0x74000000UL, 2 * PAGE, MAP_FIXED_NOREPLACE) == EEXIST,
        "NOREPLACE mid-hole -> EEXIST", errno);
  check(mm_err(0x6ffff000UL, 2 * PAGE, MAP_FIXED_NOREPLACE) == EEXIST,
        "NOREPLACE straddling hole start -> EEXIST", errno);
  {
    int e = mm_err(KHOLE_LO, PAGE, MAP_FIXED);
    check(e == ENOMEM, "MAP_FIXED into the kernel hole -> ENOMEM", e);
  }
  {
    int e = mm_err(VA_HI, PAGE, MAP_FIXED);
    check(e == ENOMEM, "MAP_FIXED at 0x7ffffffff000 -> ENOMEM", e);
  }
  {
    /* A free low range takes NOREPLACE like MAP_FIXED. */
    void *p = mm(0x30000000UL, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1);
    int ok = (p == (void *)0x30000000UL);
    if (ok) {
      *(volatile int *)p = 7;
      ok = *(volatile int *)p == 7;
    }
    check(ok, "NOREPLACE into free low VA", ok ? 0 : errno);
  }
  {
    /* A hint inside the hole is not honoured but the map succeeds elsewhere. */
    void *p = mm(KHOLE_LO + 0x100000, PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1);
    uint64_t a = (uint64_t)p;
    check(p != MAP_FAILED && !(a >= KHOLE_LO && a < 0x78000000UL),
          "hint in the hole lands outside it", (long)(a >> 20));
  }
  {
    /* The kernel never reads through the hole on a user's behalf. */
    ssize_t w = write(1, (void *)(KHOLE_LO + 0x1000), 8);
    check(w < 0 && errno == EFAULT, "write() from the hole -> EFAULT", (long)w);
  }

  kuser_coherent();
  reservations();
  file_backed_iov();

  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
