/* src/user/linux_probe_cow.c - Linux-ABI probe: MAP_PRIVATE copy-on-write of
 * file-backed objects (lane C2-wine2, docs/cyberpunk.md §6 P2).
 *
 * A plain musl x86_64 program: the same source is the reference on a Linux
 * host and the measurement inside HobbyOS.  Rows start "[LP] "; the last row is
 * "[LP] done rc=<failures>" (tools/gate/t3_linux_probe.sh accepts rc=0).
 *
 * The Linux contract Wine depends on (dlls/ntdll/unix/virtual.c
 * map_file_into_view maps every read-only section view MAP_PRIVATE because
 * file changes are visible through it on Linux; server/mapping.c
 * check_current_dir_for_exec maps a 1-byte temp file PROT_READ|PROT_EXEC
 * MAP_PRIVATE to decide where its section files live):
 *   - a private page reads through to the object until it is written;
 *   - a write copies; the write never reaches the object, a MAP_SHARED view,
 *     another private view, or another process;
 *   - PROT_EXEC private views execute the object's bytes; mprotect on them
 *     (RW for a patch, back to RX) copies on the first write only.
 *
 *   C1 memfd MAP_PRIVATE maps        C2 read-through (write(), shared store)
 *   C3 write fault copies            C4 untouched pages keep reading through
 *   C5 PROT_EXEC private + mprotect  C6 Wine's tmpfs exec check (/tmp, 1 byte)
 *   C7 tmpfs file private COW        C8 kernel-side write (read() into it)
 *   C9 MADV_DONTNEED reverts         C10 fork: copies isolated, read-through
 *   C11 F_SEAL_WRITE: shared RW refused, private RW allowed
 *   C12 a 2nd process's MAP_SHARED store reaches an untouched private page
 *   C13 munmap of a private view keeps the object's pages; churn
 *   C14 ext2 file (argv[1], default /EXT): COW isolation + read-through
 *       (rows skipped when the directory is absent, e.g. on a Linux host)
 *   C15 ftruncate shrink under live mappings: a touch past the new end is a
 *       fatal signal (Linux SIGBUS), never the freed frame; shrink + regrow
 *       reads zeros through shared and private views, COW copies past the
 *       cut are discarded (Linux truncate_pagecache), copies below it stay
 *   C16 mremap(MAYMOVE|FIXED) of a private view carries its COW copies;
 *       untouched pages keep reading through at the new address
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_SEAL_WRITE 0x0008
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 2
#endif

#define PG 4096

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
static int mfd(const char *name, unsigned flags) {
  return (int)syscall(SYS_memfd_create, name, flags);
}
static char obj_byte(int fd, off_t off) {
  char c = 0;
  if (pread(fd, &c, 1, off) != 1) return (char)0xEE;
  return c;
}
/* An object of `pages` pages, page i filled with 'a'+i. */
static int fill_obj(int fd, int pages) {
  char b[PG];
  for (int i = 0; i < pages; i++) {
    memset(b, 'a' + i, sizeof b);
    if (pwrite(fd, b, sizeof b, (off_t)i * PG) != (ssize_t)sizeof b) return -1;
  }
  return 0;
}

/* C1..C4: the core contract on a memfd. */
static void c1_c4_memfd(void) {
  int fd = mfd("cow", 0);
  ck(fd >= 0 && fill_obj(fd, 4) == 0, "C1 memfd 4 pages", fd < 0 ? errno : 0);
  if (fd < 0) return;
  volatile char *p = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "C1 mmap memfd MAP_PRIVATE RW", p == MAP_FAILED ? errno : 0);
  if (p == MAP_FAILED) { close(fd); return; }
  volatile char *s = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(s != MAP_FAILED, "C1 mmap memfd MAP_SHARED alongside", s == MAP_FAILED ? errno : 0);
  volatile char *q = mmap(0, 4 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
  ck(q != MAP_FAILED, "C1 second MAP_PRIVATE view (RO)", q == MAP_FAILED ? errno : 0);
  if (s == MAP_FAILED || q == MAP_FAILED) { close(fd); return; }

  ck(p[0] == 'a' && p[PG] == 'b' && p[3 * PG + 7] == 'd', "C2 private reads object bytes", p[0]);
  /* read-through: page 0 is now resident read-only; the object changes */
  (void)!pwrite(fd, "W", 1, 5);
  ck(p[5] == 'W' && q[5] == 'W', "C2 write() visible through untouched private views", p[5]);
  s[6] = 'S';
  ck(p[6] == 'S' && q[6] == 'S', "C2 MAP_SHARED store visible through private views", p[6]);

  /* C3: the first write copies */
  p[0] = 'P';
  ck(p[0] == 'P', "C3 private write lands", p[0]);
  ck(obj_byte(fd, 0) == 'a', "C3 object unchanged (pread)", obj_byte(fd, 0));
  ck(s[0] == 'a' && q[0] == 'a', "C3 shared + 2nd private view unchanged", s[0]);
  ck(p[5] == 'W' && p[6] == 'S', "C3 copy carries the page's earlier bytes", p[5]);
  /* after the copy the page is private: object writes no longer show */
  (void)!pwrite(fd, "X", 1, 7);
  ck(p[7] == 'a' && q[7] == 'X', "C3 copied page detached; RO view still reads through", p[7]);
  /* write fault on a page never read */
  p[2 * PG + 1] = 'Q';
  ck(p[2 * PG + 1] == 'Q' && p[2 * PG] == 'c' && obj_byte(fd, 2 * PG + 1) == 'c',
     "C3 write to untouched page: copy of object page", p[2 * PG]);

  /* C4: other pages still read through */
  (void)!pwrite(fd, "Y", 1, PG + 3);
  s[3 * PG] = 'Z';
  ck(p[PG + 3] == 'Y' && p[3 * PG] == 'Z', "C4 unwritten pages keep reading through", p[PG + 3]);

  munmap((void *)p, 4 * PG);
  munmap((void *)q, 4 * PG);
  munmap((void *)s, 4 * PG);
  close(fd);
}

/* mov eax, imm32 ; ret */
static void put_code(unsigned char *b, int v) {
  b[0] = 0xB8;
  b[1] = (unsigned char)v; b[2] = (unsigned char)(v >> 8);
  b[3] = (unsigned char)(v >> 16); b[4] = (unsigned char)(v >> 24);
  b[5] = 0xC3;
}

/* C5: PE images are mapped this way: RX private, RW for a fixup, back to RX. */
static void c5_exec(void) {
  int fd = mfd("cowx", 0);
  unsigned char code[PG];
  memset(code, 0xCC, sizeof code);
  put_code(code, 42);
  ck(fd >= 0 && pwrite(fd, code, PG, 0) == PG, "C5 memfd with code", fd < 0 ? errno : 0);
  if (fd < 0) return;
  void *x = mmap(0, PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  ck(x != MAP_FAILED, "C5 mmap PROT_READ|PROT_EXEC MAP_PRIVATE", x == MAP_FAILED ? errno : 0);
  if (x == MAP_FAILED) { close(fd); return; }
  int (*fn)(void) = (int (*)(void))x;
  ck(fn() == 42, "C5 executes the object's bytes", fn());
  ck(mprotect(x, PG, PROT_READ | PROT_WRITE) == 0, "C5 mprotect RW", errno);
  put_code((unsigned char *)x, 43);
  ck(mprotect(x, PG, PROT_READ | PROT_EXEC) == 0, "C5 mprotect back to RX", errno);
  ck(fn() == 43, "C5 patched private copy executes", fn());
  ck(obj_byte(fd, 1) == 42, "C5 object code unpatched", obj_byte(fd, 1));
  /* a fresh RX view still sees the object's (re-written) code */
  put_code(code, 44);
  (void)!pwrite(fd, code, 6, 0);
  void *y = mmap(0, PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  ck(y != MAP_FAILED && ((int (*)(void))y)() == 44 && fn() == 43,
     "C5 fresh view runs new object code, patched view keeps its copy", 0);
  if (y != MAP_FAILED) munmap(y, PG);
  /* RWX private: a write then an exec on the same page */
  void *z = mmap(0, PG, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd, 0);
  if (z != MAP_FAILED) {
    put_code((unsigned char *)z, 45);
    ck(((int (*)(void))z)() == 45 && obj_byte(fd, 1) == 44, "C5 RWX private write+exec", 0);
    munmap(z, PG);
  } else {
    ck(0, "C5 mmap RWX private", errno);
  }
  munmap(x, PG);
  close(fd);
}

/* C6: exactly server/mapping.c check_current_dir_for_exec on the tmpfs. */
static void c6_wine_exec_check(void) {
  char d[64], f[96];
  snprintf(d, sizeof d, "/tmp/cowp-%d", (int)getpid());
  mkdir(d, 0700);
  snprintf(f, sizeof f, "%s/tmpmap-%08x", d, 0x1234u);
  int fd = open(f, O_RDWR | O_CREAT | O_EXCL, 0600);
  ck(fd >= 0, "C6 create tmpmap file", fd < 0 ? errno : 0);
  if (fd < 0) return;
  ck(pwrite(fd, "", 1, 0) == 1, "C6 grow_file(fd, 1)", errno); /* the server writes 1 byte */
  void *m = mmap(0, PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  ck(m != MAP_FAILED, "C6 1-page PROT_READ|PROT_EXEC MAP_PRIVATE of a /tmp file",
     m == MAP_FAILED ? errno : 0);
  if (m != MAP_FAILED) munmap(m, PG);
  close(fd);
  unlink(f);
}

/* C7: a tmpfs regular file, the section-file case (unlinked while open). */
static void c7_tmpfs_file(void) {
  char f[96];
  snprintf(f, sizeof f, "/tmp/cowp-%d/sect", (int)getpid());
  int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0600);
  ck(fd >= 0 && fill_obj(fd, 3) == 0, "C7 /tmp file 3 pages", fd < 0 ? errno : 0);
  if (fd < 0) return;
  unlink(f); /* Wine unlinks its section files right after creating them */
  volatile char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  volatile char *s = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(p != MAP_FAILED && s != MAP_FAILED, "C7 private + shared views of the unlinked file",
     (p == MAP_FAILED || s == MAP_FAILED) ? errno : 0);
  if (p == MAP_FAILED || s == MAP_FAILED) { close(fd); return; }
  ck(p[0] == 'a' && p[2 * PG] == 'c', "C7 private reads file bytes", p[0]);
  s[1] = 's';
  ck(p[1] == 's', "C7 shared store visible through private", p[1]);
  p[PG] = 'p';
  ck(p[PG] == 'p' && s[PG] == 'b' && obj_byte(fd, PG) == 'b', "C7 private write isolated", s[PG]);
  struct stat st;
  memset(&st, 0, sizeof st);
  int fr = fstat(fd, &st);
  ck(fr == 0 && st.st_size == 3 * PG, "C7 file size unchanged", (long)st.st_size);
  munmap((void *)p, 3 * PG);
  munmap((void *)s, 3 * PG);
  close(fd);
}

/* C8: the kernel writes into a private page (read() from a pipe). */
static void c8_kernel_write(void) {
  int fd = mfd("cowk", 0);
  int pp[2];
  if (fd < 0 || fill_obj(fd, 2) != 0 || pipe(pp) != 0) { ck(0, "C8 setup", errno); return; }
  volatile char *p = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) { ck(0, "C8 mmap", errno); close(fd); return; }
  char c = p[0]; /* page 0 resident read-only (object frame) */
  (void)c;
  (void)!write(pp[1], "PIPE", 4);
  long n = read(pp[0], (void *)(p + 8), 4);
  ck(n == 4 && memcmp((const void *)(p + 8), "PIPE", 4) == 0, "C8 read() into private page", n);
  ck(obj_byte(fd, 8) == 'a', "C8 object unchanged by kernel write", obj_byte(fd, 8));
  (void)!write(pp[1], "MORE", 4);
  n = read(pp[0], (void *)(p + PG + 8), 4); /* page 1 never touched */
  ck(n == 4 && p[PG + 8] == 'M' && obj_byte(fd, PG + 8) == 'b', "C8 read() into untouched page", n);
  munmap((void *)p, 2 * PG);
  close(pp[0]); close(pp[1]); close(fd);
}

/* C9: MADV_DONTNEED drops the private copy: the page reads the object again. */
static void c9_dontneed(void) {
  int fd = mfd("cowd", 0);
  if (fd < 0 || fill_obj(fd, 1) != 0) { ck(0, "C9 setup", errno); return; }
  volatile char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) { ck(0, "C9 mmap", errno); close(fd); return; }
  p[0] = 'D';
  ck(madvise((void *)p, PG, MADV_DONTNEED) == 0, "C9 madvise DONTNEED", errno);
  ck(p[0] == 'a', "C9 private page reverts to object bytes", p[0]);
  munmap((void *)p, PG);
  close(fd);
}

/* C10: fork.  The parent's copied page is copied again for the child; a
   page still reading through keeps reading through in both. */
static void c10_fork(void) {
  int fd = mfd("cowf", 0);
  if (fd < 0 || fill_obj(fd, 3) != 0) { ck(0, "C10 setup", errno); return; }
  volatile char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) { ck(0, "C10 mmap", errno); close(fd); return; }
  p[0] = 'P';            /* page 0: parent's copy */
  char c = p[PG];        /* page 1: resident, object frame */
  (void)c;
  int pp[2];
  if (pipe(pp) != 0) { ck(0, "C10 pipe", errno); return; }
  pid_t k = fork();
  if (k == 0) {
    char r[4];
    r[0] = p[0];                      /* expect 'P' */
    p[0] = 'C';                       /* child's own copy */
    p[PG] = 'c';                      /* child copies page 1 */
    r[1] = obj_byte(fd, PG);          /* expect 'b' */
    (void)!pwrite(fd, "O", 1, 2 * PG); /* object page 2 changes */
    r[2] = p[2 * PG];                 /* expect 'O' (read-through) */
    r[3] = 0;
    (void)!write(pp[1], r, 3);
    _exit(0);
  }
  int stt = 0;
  char r[4] = {0};
  long n = read(pp[0], r, 3);
  waitpid(k, &stt, 0);
  ck(k > 0 && n == 3 && r[0] == 'P', "C10 child inherits the parent's copy", r[0]);
  ck(r[1] == 'b' && r[2] == 'O', "C10 child: object untouched, page 2 reads through", r[2]);
  ck(p[0] == 'P' && p[PG] == 'b', "C10 parent isolated from the child's writes", p[0]);
  ck(p[2 * PG] == 'O', "C10 parent page 2 reads through the child's pwrite", p[2 * PG]);
  munmap((void *)p, 3 * PG);
  close(pp[0]); close(pp[1]); close(fd);
}

/* C11: seals.  F_SEAL_WRITE refuses MAP_SHARED|PROT_WRITE, never a private view. */
static void c11_seal(void) {
  int fd = mfd("cows", MFD_ALLOW_SEALING);
  if (fd < 0 || fill_obj(fd, 1) != 0) { ck(0, "C11 setup", errno); return; }
  ck(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0, "C11 F_SEAL_WRITE", errno);
  void *s = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ck(s == MAP_FAILED && errno == EPERM, "C11 sealed MAP_SHARED RW -> EPERM", errno);
  volatile char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "C11 sealed MAP_PRIVATE RW maps", p == MAP_FAILED ? errno : 0);
  if (p != MAP_FAILED) {
    p[0] = 'z';
    ck(p[0] == 'z' && obj_byte(fd, 0) == 'a', "C11 private write on a sealed memfd", p[0]);
    munmap((void *)p, PG);
  }
  close(fd);
}

/* C12: another process's MAP_SHARED store reaches an untouched private page
   (the KUSER_SHARED_DATA shape: the server writes, clients map read-only). */
static void c12_cross_process(void) {
  int fd = mfd("cowx2", 0);
  if (fd < 0 || fill_obj(fd, 1) != 0) { ck(0, "C12 setup", errno); return; }
  volatile char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) { ck(0, "C12 mmap", errno); close(fd); return; }
  ck(mprotect((void *)p, PG, PROT_READ) == 0, "C12 mprotect R (Wine's KUSER view)", errno);
  char c0 = p[0];
  pid_t k = fork();
  if (k == 0) {
    volatile char *s = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) _exit(3);
    for (int i = 0; i < 16; i++) s[64 + i] = (char)('A' + i);
    _exit(0);
  }
  int stt = 0;
  waitpid(k, &stt, 0);
  int seen = 1;
  for (int i = 0; i < 16; i++) if (p[64 + i] != (char)('A' + i)) seen = 0;
  ck(k > 0 && WIFEXITED(stt) && WEXITSTATUS(stt) == 0 && c0 == 'a' && seen,
     "C12 child's MAP_SHARED stores visible in parent's private view", p[64]);
  munmap((void *)p, PG);
  close(fd);
}

/* C13: unmapping private views never frees the object's pages; churn. */
static void c13_unmap_churn(void) {
  int fd = mfd("cowc", 0);
  if (fd < 0 || ftruncate(fd, 64 * PG) != 0 || fill_obj(fd, 1) != 0) {
    ck(0, "C13 setup", errno);
    return;
  }
  int ok = 1;
  for (int it = 0; it < 256 && ok; it++) {
    volatile char *p = mmap(0, 64 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { ok = 0; break; }
    for (int i = 0; i < 64; i++) {
      if (i & 1) p[(long)i * PG] = (char)it;          /* copy */
      else if (p[(long)i * PG] != (i ? 0 : 'a')) ok = 0; /* read through */
    }
    munmap((void *)p, 64 * PG);
  }
  ck(ok, "C13 256 x 64-page private map/write/unmap", ok);
  int same = obj_byte(fd, 0) == 'a' && obj_byte(fd, PG) == 0 && obj_byte(fd, 3 * PG) == 0;
  ck(same, "C13 object pages intact after the churn", obj_byte(fd, 0));
  close(fd);
}

/* C14: the same contract on an ext2 file (VM_OBJ_EXT2). */
static void c14_ext2(const char *root) {
  struct stat st;
  if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP C14 ext2: %s absent\n", root);
    return;
  }
  char f[128];
  snprintf(f, sizeof f, "%s/cowprobe.bin", root);
  int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
  ck(fd >= 0 && fill_obj(fd, 2) == 0, "C14 ext2 file 2 pages", fd < 0 ? errno : 0);
  if (fd < 0) return;
  volatile char *p = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  ck(p != MAP_FAILED, "C14 ext2 MAP_PRIVATE RW", p == MAP_FAILED ? errno : 0);
  if (p != MAP_FAILED) {
    ck(p[0] == 'a' && p[PG] == 'b', "C14 private reads file bytes", p[0]);
    (void)!pwrite(fd, "W", 1, 9);
    ck(p[9] == 'W', "C14 write() visible through untouched private page", p[9]);
    p[0] = 'P';
    ck(p[0] == 'P' && obj_byte(fd, 0) == 'a', "C14 private write isolated from the file", obj_byte(fd, 0));
    void *x = mmap(0, PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, PG);
    ck(x != MAP_FAILED, "C14 ext2 PROT_EXEC private maps", x == MAP_FAILED ? errno : 0);
    if (x != MAP_FAILED) munmap(x, PG);
    munmap((void *)p, 2 * PG);
  }
  close(fd);
  unlink(f);
}

/* C15: shrink a mapped object. */
static void c15_truncate(void) {
  int fd = mfd("cowt", 0);
  if (fd < 0 || fill_obj(fd, 3) != 0) { ck(0, "C15 setup", errno); return; }
  /* (a) a child touches a shared page past the new end: must die */
  pid_t k = fork();
  if (k == 0) {
    volatile char *s = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED) _exit(3);
    char c = s[2 * PG]; /* resident */
    (void)c;
    if (ftruncate(fd, PG) != 0) _exit(4);
    char d = s[2 * PG]; /* past EOF now: SIGBUS on Linux */
    _exit(d == 'c' ? 5 : 6); /* 5: read the old (freed) page */
  }
  int st = 0;
  waitpid(k, &st, 0);
  ck(k > 0 && WIFSIGNALED(st), "C15 touch past the shrunk end is a fatal signal",
     WIFSIGNALED(st) ? WTERMSIG(st) : 1000 + WEXITSTATUS(st));
  /* (b) shrink + regrow under shared and private views reads zeros */
  if (ftruncate(fd, 0) != 0 || fill_obj(fd, 3) != 0) { ck(0, "C15 refill", errno); close(fd); return; }
  volatile char *s = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  volatile char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (s == MAP_FAILED || p == MAP_FAILED) { ck(0, "C15 mmap", errno); close(fd); return; }
  char c1 = p[PG], c2 = s[2 * PG];  /* page 1: private reads through; page 2: shared */
  p[2 * PG] = 'P';                  /* page 2: private COW copy (cut off) */
  p[0] = 'Q';                       /* page 0: private COW copy (kept) */
  ck(c1 == 'b' && c2 == 'c', "C15 views resident before the shrink", c1);
  ck(ftruncate(fd, PG) == 0 && ftruncate(fd, 3 * PG) == 0, "C15 shrink to 1 page, regrow to 3", errno);
  ck(s[PG] == 0 && s[2 * PG] == 0 && obj_byte(fd, 2 * PG) == 0, "C15 shared view reads the regrown hole as zeros", s[PG]);
  ck(p[PG] == 0, "C15 private read-through page reads zeros", p[PG]);
  ck(p[2 * PG] == 0, "C15 private COW copy past the cut is discarded (Linux even_cows)", p[2 * PG]);
  ck(s[0] == 'a' && p[0] == 'Q', "C15 below the cut: object and COW copy untouched", p[0]);
  s[PG] = 'N';
  ck(p[PG] == 'N', "C15 regrown page shared again by both views", p[PG]);
  munmap((void *)s, 3 * PG);
  munmap((void *)p, 3 * PG);
  close(fd);
}

/* C16: mremap a private view to a new address. */
static void c16_mremap(void) {
  int fd = mfd("cowm", 0);
  if (fd < 0 || fill_obj(fd, 3) != 0) { ck(0, "C16 setup", errno); return; }
  volatile char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  void *tgt = mmap(0, 3 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED || tgt == MAP_FAILED) { ck(0, "C16 mmap", errno); close(fd); return; }
  p[0] = 'P';           /* COW copy */
  char c = p[PG];       /* reading through */
  (void)c;
  volatile char *q = mremap((void *)p, 3 * PG, 3 * PG, MREMAP_MAYMOVE | MREMAP_FIXED, tgt);
  ck(q == (volatile char *)tgt, "C16 mremap MAYMOVE|FIXED", q == MAP_FAILED ? errno : 0);
  if (q != (volatile char *)tgt) { close(fd); return; }
  ck(q[0] == 'P', "C16 COW copy carried to the new address", q[0]);
  (void)!pwrite(fd, "Y", 1, PG);
  ck(q[PG] == 'Y' && q[2 * PG] == 'c', "C16 other pages read through at the new address", q[PG]);
  ck(obj_byte(fd, 0) == 'a', "C16 object unchanged", obj_byte(fd, 0));
  munmap((void *)q, 3 * PG);
  close(fd);
}

int main(int argc, char **argv) {
  c1_c4_memfd();
  c5_exec();
  c6_wine_exec_check();
  c7_tmpfs_file();
  c8_kernel_write();
  c9_dontneed();
  c10_fork();
  c11_seal();
  c12_cross_process();
  c13_unmap_churn();
  c15_truncate();
  c16_mremap();
  c14_ext2(argc > 1 ? argv[1] : "/EXT");
  char d[64];
  snprintf(d, sizeof d, "/tmp/cowp-%d", (int)getpid());
  rmdir(d);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
