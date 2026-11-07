/* rwwalk.c — L58: the in-guest directory walker.
 *
 * Question: are the def files really enumerable from inside the guest?  The
 * def pass reports 577 unresolved SoundDefs and a missing
 * `MusicalInstrumentBase` parent while `[NFSLS]` says every directory of
 * /Data/Core/Defs matched the host.  A count of *directories* says nothing
 * about *files*, so this probe prints every directory and every file with
 * its stat() size, fully recursive, over the raw getdents64 syscall (the
 * same kernel path the game's glibc readdir uses — NOT the native
 * SYS_READ_DIR, which has its own 31-char cap).
 *
 * It also re-lists one directory with four different buffer sizes: a
 * resume-offset or reclen bug shows up as a lost/duplicated entry when the
 * buffer boundary falls inside an entry, and a name cap shows up as a
 * `nlen` shorter than the host's own name length.
 *
 * Rows (serial console, one per entry so the host can diff them):
 *   [W] d <path>
 *   [W] f <path> <size-from-stat>
 *   [W] e <path> nlen=<n>          stat() failed on an entry we enumerated
 *   [W] r <dir> idx=.. reclen=.. type=.. nlen=.. name=<name>
 *   [W] S <path> n=.. dirs=.. files=.. used=.. bsz=..
 *   [W] TOT dirs=.. files=.. bytes=..
 *
 * Build/run: tools/rw/run-guest-walk.sh
 */
#include <stdint.h>
#include <stddef.h>
#include "libc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

#define SYS_GETDENTS64_NATIVE 95
#define MAXDEPTH 10
#define PATHCAP 512

static long g_dirs, g_files, g_bytes, g_bad;

static void pc(const char *s) { print_console(s); }

static void pd(long v) {
  char tmp[24], out[24];
  int i = 0, k = 0;
  unsigned long u;
  if (v < 0) { pc("-"); u = (unsigned long)(-v); } else u = (unsigned long)v;
  if (!u) { pc("0"); return; }
  while (u) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
  while (i) out[k++] = tmp[--i];
  out[k] = 0;
  pc(out);
}

static long raw_getdents64(int fd, void *buf, long n) {
  long ret;
  register long rdi __asm__("rdi") = fd;
  register long rsi __asm__("rsi") = (long)buf;
  register long rdx __asm__("rdx") = n;
  register long rax __asm__("rax") = SYS_GETDENTS64_NATIVE;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "r"(rdi), "r"(rsi), "r"(rdx), "r"(rax)
                   : "rcx", "r11", "memory");
  return ret;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* dst = dir + "/" + name, or dir + name when dir already ends in '/'. */
static void join(char *dst, int cap, const char *dir, const char *name) {
  int n = 0;
  while (dir[n] && n < cap - 1) { dst[n] = dir[n]; n++; }
  if (n > 0 && dst[n - 1] != '/' && n < cap - 1) dst[n++] = '/';
  int i = 0;
  while (name[i] && n < cap - 1) dst[n++] = name[i++];
  dst[n] = 0;
}


/* One console record per line, in ONE syscall.  Building a row from several
 * print_console() calls let a kernel row ([RDP]/[GDT]/[XO]) interleave in the
 * middle of a path and shredded four lines in an earlier run — the guest's
 * list was complete and the log said otherwise. */
static void emit_row(const char *tag, const char *path, long num, int with_num) {
  char b[PATHCAP + 40];
  int n = 0;
  while (*tag && n < (int)sizeof b - 1) b[n++] = *tag++;
  int i = 0;
  while (path[i] && n < (int)sizeof b - 1) b[n++] = path[i++];
  if (with_num) {
    char t[24];
    int k = 0;
    unsigned long u = (num < 0) ? (unsigned long)(-num) : (unsigned long)num;
    if (num < 0 && n < (int)sizeof b - 1) b[n++] = '-';
    if (!u) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + (u % 10)); u /= 10; }
    if (n < (int)sizeof b - 1) b[n++] = ' ';
    while (k && n < (int)sizeof b - 1) b[n++] = t[--k];
  }
  if (n < (int)sizeof b - 1) b[n++] = '\n';
  b[n] = 0;
  pc(b);
}

static void walk_dir(const char *path, int bsz, int verbose, int depth) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    extern int errno;
    pc("[W] OPENFAIL "); pc(path); pc(" rc="); pd(fd);
    pc(" errno="); pd(errno); pc("\n");
    g_bad++;
    return;
  }
  emit_row("[W] d ", path, 0, 0);
  g_dirs++;

  /* NOT static: walk_dir recurses, and a shared buffer would be clobbered by
   * a child's getdents64 while the parent is still parsing its page (that
   * produced phantom BADREC rows at first — the kernel was innocent). */
  uint8_t buf[4096];
  if (bsz > (int)sizeof buf) bsz = (int)sizeof buf;
  long used = 0, calls = 0, entries = 0, nfiles = 0, ndirs = 0;
  for (;;) {
    long n = raw_getdents64(fd, buf, bsz);
    if (n < 0) {
      pc("[W] DENTSFAIL "); pc(path); pc(" rc="); pd(n);
      pc(" after="); pd(entries); pc("\n");
      g_bad++;
      break;
    }
    if (n == 0) break;
    calls++;
    used += n;
    long off = 0;
    while (off < n) {
      uint8_t *p = buf + off;
      uint16_t reclen = (uint16_t)(p[16] | (p[17] << 8));
      uint8_t dtype = p[18];
      const char *nm = (const char *)(p + 19);
      if (reclen < 20 || off + reclen > n) {
        pc("[W] BADREC "); pc(path); pc(" off="); pd(off);
        pc(" reclen="); pc(""); pd(reclen); pc(" n="); pd(n); pc("\n");
        g_bad++;
        break;
      }
      int nl = slen(nm);
      if (!(nl == 1 && nm[0] == '.') && !(nl == 2 && nm[0] == '.' && nm[1] == '.')) {
        char cur[PATHCAP];
        join(cur, sizeof cur, path, nm);
        if (verbose) {
          pc("[W] r "); pc(path); pc(" idx="); pd(entries);
          pc(" reclen="); pd(reclen); pc(" type="); pd(dtype);
          pc(" nlen="); pd(nl); pc(" name="); pc(nm); pc("\n");
        }
        entries++;
        if (dtype == 4) {
          ndirs++;
          if (depth < MAXDEPTH) {
            walk_dir(cur, 4096, 0, depth + 1);
          } else {
            pc("[W] d "); pc(cur); pc("\n");
          }
        } else {
          nfiles++;
          struct stat st;
          if (stat(cur, &st) == 0) {
            g_files++;
            g_bytes += (long)st.st_size;
            emit_row("[W] f ", cur, (long)st.st_size, 1);
          } else {
            g_bad++;
            emit_row("[W] e ", cur, nl, 1);
          }
        }
      }
      off += reclen;
    }
  }
  close(fd);
  pc("[W] S "); pc(path); pc(" n="); pd(entries);
  pc(" dirs="); pd(ndirs); pc(" files="); pd(nfiles);
  pc(" used="); pd(used); pc(" calls="); pd(calls); pc(" bsz="); pd(bsz); pc("\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  pc("[W] begin pid="); pd(getpid()); pc("\n");
  int mrc = -1;
  for (int i = 0; i < 100; i++) {
    mrc = mount("10.0.2.2:/srv/nfs/rimworld", "/NFS");
    if (mrc == 0) break;
    usleep(200000);
  }
  pc("[W] mount rc="); pd(mrc); pc("\n");
  if (mrc != 0) { pc("[W] NO-NFS\n"); exit(1); }

  /* Path/flag probe: a failed directory open is four different bugs and the
   * errno plus a FAT control separates them. */
  {
    extern int errno;
    static const char *probe[] = {
      "/GAMES", "/NFS", "/NFS/Data", "/NFS/Data/Core", "/NFS/Data/Core/Defs"
    };
    for (int i = 0; i < 5; i++) {
      errno = 0;
      int fd = open(probe[i], O_RDONLY | O_DIRECTORY);
      pc("[W] PROBE "); pc(probe[i]); pc(" fd="); pd(fd);
      pc(" errno="); pd(errno);
      if (fd >= 0) {
        static uint8_t tb[1024];
        long n = raw_getdents64(fd, tb, sizeof tb);
        pc(" dents="); pd(n);
        /* First record's name: tells a FAT mount-point directory (3 entries)
         * from the real export (28) at a glance. */
        if (n > 20) {
          pc(" first="); pc((const char *)(tb + 19));
          long off = (uint16_t)(tb[16] | (tb[17] << 8));
          if (off > 0 && off < n) { pc(" second="); pc((const char *)(tb + off + 19)); }
        }
        close(fd);
      }
      pc("\n");
    }
    /* A regular file under the same mount: separates "routing is dead" from
     * "directory opens are dead". */
    errno = 0;
    int ff = open("/NFS/Data/Place official expansions here.txt", O_RDONLY);
    pc("[W] FILE /NFS/Data/Place official expansions here.txt fd="); pd(ff);
    pc(" errno="); pd(errno);
    if (ff >= 0) {
      char b[40];
      long n = read(ff, b, 32);
      b[n > 0 ? n : 0] = 0;
      pc(" n="); pd(n); pc(" head="); pc(b);
      close(ff);
    }
    pc("\n");
    errno = 0;
    int nd = open("/NFS/Data", O_RDONLY);   /* no O_DIRECTORY */
    pc("[W] NODIRFLAG /NFS/Data fd="); pd(nd); pc(" errno="); pd(errno);
    if (nd >= 0) close(nd);
    pc("\n");
    struct stat st;
    errno = 0;
    pc("[W] STAT /NFS/Data rc="); pd(stat("/NFS/Data", &st));
    pc(" errno="); pd(errno); pc(" mode="); pd((long)st.st_mode);
    pc(" size="); pd((long)st.st_size); pc("\n");
    errno = 0;
    pc("[W] STAT /NFS/Data/Core/Defs rc="); pd(stat("/NFS/Data/Core/Defs", &st));
    pc(" errno="); pd(errno); pc(" mode="); pd((long)st.st_mode); pc("\n");
  }

  walk_dir("/NFS/Data", 4096, 0, 0);
  pc("[W] TOT dirs="); pd(g_dirs); pc(" files="); pd(g_files);
  pc(" bytes="); pd(g_bytes); pc(" bad="); pd(g_bad); pc("\n");

  /* Buffer-boundary experiment: the SoundDefs listing at four buffer sizes.
   * Every pass must yield the same names; a resume bug loses or repeats an
   * entry when a record straddles the buffer end, and a name cap shows as a
   * short nlen. */
  static const int sizes[4] = { 64, 128, 4096, 8192 };
  for (int i = 0; i < 4; i++) {
    pc("[W] BOUND bsz="); pd(sizes[i]); pc("\n");
    walk_dir("/NFS/Data/Core/Defs/SoundDefs", sizes[i], 1, 1);
  }
  pc("[W] done\n");
  exit(0);
}
