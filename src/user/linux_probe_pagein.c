/* src/user/linux_probe_pagein.c - Linux-ABI probe: a page-in that finds no
 * free frame is NOT an I/O error (lane H10-pagein, finding H9-12,
 * docs/cp-pagein.md).
 *
 * h4-l01/l02 (lavapipe at 12 GiB): the guest ran out of frames, Wine's
 * SIGTRAP handler needed a not-yet-read page of Wine's text, the page source
 * returned the same 0 for "no frame" as for "read failed", and the kernel
 * marked the executable mapping VM_MAP_IOERR for good: SIGBUS, then SIGBUS on
 * every later fault in it, and the game died.  On Linux a page-in that cannot
 * get memory is VM_FAULT_OOM (the OOM killer's SIGKILL), never SIGBUS, and it
 * never poisons the mapping.
 *
 * A plain musl x86_64 program; rows "[LP] ...", last row "[LP] done rc=<n>".
 * The file lives in argv[1] (default "/", the FAT16 boot volume in HobbyOS).
 * PGIN.BIN holds NPAGES executable pages: page i is `mov eax, 0x100+i; ret`
 * padded with int3; page ZPAGE is all zeros (the exec gate refuses an
 * all-zero text page with EIO: M6/L63, the "real read error" here).
 *
 *   P1 child A maps PGIN.BIN PROT_READ|PROT_EXEC MAP_PRIVATE, takes every
 *      free frame in the guest (memfd writes, then 1-byte preads into fresh
 *      anonymous pages until the kernel refuses), then calls page 0:
 *        - SIGBUS here (the frame shortage reported as EIO) is the bug;
 *        - returning 0x100 (the kernel reclaimed a frame) is fine: take the
 *          rest again and try the next page (bounded);
 *        - SIGKILL (an out-of-memory kill) is fine.
 *      If it is still alive it releases everything and calls page 5 of the
 *      SAME mapping: it must run (base: SIGBUS, the region was marked).
 *   P3 with memory back, a new mapping of the same file runs every code page.
 *   P4 child C calls the zero page: SIGBUS (BUS_ADRERR), and then page 1 of
 *      the same mapping is refused too (the M6/L69 mark is unchanged).
 * The kernel side lives in the console: tools/gate/lp_check_pagein.py grades
 * that no [EIO] row belongs to child A and that child C's do.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096
#define NPAGES 8
#define ZPAGE 7
#define NMEMFD 12
#define MEMFD_CAP (3ull << 30) /* below the kernel's 4 GiB object cap */
#define CHUNK (1u << 20)
#define ANON_SPAN (2ull << 30)

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

/* ---- calling a page of the mapping, with SIGBUS caught ---- */
static sigjmp_buf jb;
static volatile long bus_code, bus_addr;
static void on_bus(int s, siginfo_t *si, void *u) {
  (void)s;
  (void)u;
  bus_code = si->si_code;
  bus_addr = (long)si->si_addr;
  siglongjmp(jb, 1);
}
static char altstk[64 * 1024];
static void catch_bus(void) {
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = altstk;
  ss.ss_size = sizeof altstk;
  memset(altstk, 0x5A, sizeof altstk); /* resident before any shortage */
  (void)sigaltstack(&ss, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_bus;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGBUS, &sa, 0);
}
/* 0x100+i on success, -1 after SIGBUS */
static long call_page(volatile uint8_t *base, int i) {
  if (sigsetjmp(jb, 1))
    return -1;
  long (*fn)(void) = (long (*)(void))(uintptr_t)(base + (uintptr_t)i * PG);
  return fn();
}

/* ---- taking every free frame ---- */
static char wbuf[CHUNK];
static int mfd[NMEMFD];
static uint64_t mfd_len[NMEMFD];
static uint8_t *anon;
static uint64_t anon_used;
static int srcfd = -1;
static void touch_stack(void) { /* the stack this child uses, made resident */
  volatile char big[96 * 1024];
  for (unsigned i = 0; i < sizeof big; i += 512) big[i] = (char)i;
}
static int prep_exhaust(void) {
  for (unsigned i = 0; i < sizeof wbuf; i++) wbuf[i] = (char)(i * 13 + 1);
  for (int k = 0; k < NMEMFD; k++) {
    mfd[k] = (int)syscall(SYS_memfd_create, "pgin", 0);
    if (mfd[k] < 0) return -1;
    mfd_len[k] = 0;
  }
  srcfd = (int)syscall(SYS_memfd_create, "pgsrc", 0);
  if (srcfd < 0 || write(srcfd, wbuf, PG) != PG) return -1;
  anon = mmap(0, ANON_SPAN, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (anon == MAP_FAILED) return -1;
  touch_stack();
  return 0;
}
/* Returns MiB taken.  Phase A: memfd writes (the bulk).  Phase B: one byte
   into each fresh anonymous page, through pread() -- a refused copy-out is a
   syscall error here, never a fault -- until the kernel says no.  That also
   drains the zeroed frames parked for faults (the anon path takes those). */
static long exhaust(void) {
  long mib = 0;
  /* a write that stops short moves on to the next memfd (a big object's
     page array can be what ran out); a memfd that takes nothing at all
     means the pool is dry */
  for (int k = 0; k < NMEMFD; k++) {
    int took = 0;
    while (mfd_len[k] + CHUNK <= MEMFD_CAP) {
      ssize_t w = write(mfd[k], wbuf, CHUNK);
      if (w > 0) {
        mfd_len[k] += (uint64_t)w;
        took = 1;
      }
      if (w != (ssize_t)CHUNK) break;
      mib++;
    }
    if (!took && mfd_len[k] + CHUNK <= MEMFD_CAP) break;
  }
  for (int miss = 0; anon_used + PG <= ANON_SPAN;) {
    if (pread(srcfd, anon + anon_used, 1, 0) == 1) {
      anon_used += PG;
      miss = 0;
    } else if (++miss >= 3) {
      break;
    }
  }
  return mib + (long)(anon_used >> 20);
}
static void release(void) {
  for (int k = 0; k < NMEMFD; k++) {
    if (mfd[k] >= 0) close(mfd[k]);
    mfd[k] = -1;
  }
  if (anon && anon != MAP_FAILED) munmap(anon, ANON_SPAN);
  anon = 0;
}

struct report {
  long taken_mib;
  int recovered;   /* page-ins that ran under the shortage (reclaim) */
  int bus_page;    /* the page that took SIGBUS under the shortage, or -1 */
  long bus_code;
  long after_back; /* page 5 of the same mapping once memory is back */
};

static volatile uint8_t *map_file(int fd) {
  void *m = mmap(0, NPAGES * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  return m == MAP_FAILED ? 0 : (volatile uint8_t *)m;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "/";
  char path[160];
  snprintf(path, sizeof path, "%s%sPGIN.BIN", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
  out("[LP] pagein probe (H10-pagein, H9-12): file %s\n", path);
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0, "setup create", fd < 0 ? errno : 0);
  if (fd < 0) { out("[LP] done rc=%d\n", fails); return 1; }
  static uint8_t page[PG];
  int wok = 1;
  for (int i = 0; i < NPAGES; i++) {
    memset(page, i == ZPAGE ? 0x00 : 0xCC, PG);
    if (i != ZPAGE) {
      uint32_t v = 0x100u + (uint32_t)i;
      page[0] = 0xB8; /* mov eax, imm32 */
      memcpy(page + 1, &v, 4);
      page[5] = 0xC3; /* ret */
    }
    if (write(fd, page, PG) != PG) wok = 0;
  }
  ck(wok, "setup write 8 pages (7 code, 1 zero)", errno);
  fsync(fd);

  /* ---- P1 ---- */
  int pp[2];
  ck(pipe(pp) == 0, "P1 pipe", errno);
  pid_t a = fork();
  if (a == 0) {
    close(pp[0]);
    struct report r;
    memset(&r, 0, sizeof r);
    r.bus_page = -1;
    r.after_back = -2;
    volatile uint8_t *m = map_file(fd);
    if (!m || prep_exhaust() != 0) _exit(90);
    catch_bus();
    r.taken_mib = exhaust();
    /* no output and no new stack below here until release(): there is no
       frame to put it in */
    for (int k = 0; k < 4; k++) {
      long v = call_page(m, k);
      if (v == 0x100 + k) {
        r.recovered++;
        r.taken_mib += exhaust(); /* the kernel found a frame: take it again */
        continue;
      }
      r.bus_page = k;
      r.bus_code = bus_code;
      break;
    }
    release();
    r.after_back = call_page(m, 5);
    (void)!write(pp[1], &r, sizeof r);
    _exit(0);
  }
  close(pp[1]);
  out("[LP] P1 child pid=%d (exhausts the guest, then executes unread file pages)\n", (int)a);
  int st = 0;
  (void)waitpid(a, &st, 0);
  struct report r;
  memset(&r, 0, sizeof r);
  ssize_t got = read(pp[0], &r, sizeof r);
  close(pp[0]);
  if (WIFSIGNALED(st)) {
    out("[LP] P1 child killed by signal %d (no report)\n", WTERMSIG(st));
    ck(WTERMSIG(st) != SIGBUS, "P1 no SIGBUS for a frame shortage", WTERMSIG(st));
    ck(WTERMSIG(st) == SIGKILL, "P1 the shortage ended in an out-of-memory kill (SIGKILL)", WTERMSIG(st));
  } else if (got == (ssize_t)sizeof r) {
    out("[LP] P1 report taken_mib=%ld recovered=%d bus_page=%d bus_code=%ld after_back=0x%lx\n",
        r.taken_mib, r.recovered, r.bus_page, r.bus_code, (unsigned long)r.after_back);
    ck(r.taken_mib > 1024, "P1 the child took the guest's memory (MiB)", r.taken_mib);
    ck(r.bus_page < 0, "P1 no SIGBUS for a frame shortage (page that took it)", r.bus_page);
    ck(r.after_back == 0x105, "P1 with memory back, the same mapping runs page 5 (not marked)",
       r.after_back);
  } else {
    ck(0, "P1 child report", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
  }

  /* ---- P3: memory back, a new mapping runs every code page ---- */
  {
    volatile uint8_t *m = map_file(fd);
    int okp = 0;
    catch_bus();
    for (int i = 0; m && i < NPAGES; i++)
      if (i != ZPAGE && call_page(m, i) == 0x100 + i) okp++;
    ck(okp == NPAGES - 1, "P3 memory back: a new mapping runs all 7 code pages", okp);
    if (m) munmap((void *)m, NPAGES * PG);
  }

  /* ---- P4: a real read failure is still SIGBUS, and still marks ---- */
  int pq[2];
  ck(pipe(pq) == 0, "P4 pipe", errno);
  pid_t c = fork();
  if (c == 0) {
    close(pq[0]);
    long res[3] = {0, 0, 0};
    volatile uint8_t *m = map_file(fd);
    if (!m) _exit(91);
    catch_bus();
    res[0] = call_page(m, ZPAGE);
    res[1] = bus_code;
    res[2] = call_page(m, 1);
    (void)!write(pq[1], res, sizeof res);
    _exit(0);
  }
  close(pq[1]);
  out("[LP] P4 child pid=%d (executes the all-zero page: the exec gate's EIO)\n", (int)c);
  long res[3] = {0, 0, 0};
  st = 0;
  (void)waitpid(c, &st, 0);
  got = read(pq[0], res, sizeof res);
  close(pq[0]);
  ck(got == (ssize_t)sizeof res && res[0] == -1 && res[1] == 2,
     "P4 the zero text page is SIGBUS/BUS_ADRERR (EIO unchanged)", res[1]);
  ck(got == (ssize_t)sizeof res && res[2] == -1,
     "P4 ... and the mapping stays refused (page 1 too: M6/L69's mark)", res[2]);

  close(fd);
  unlink(path);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
