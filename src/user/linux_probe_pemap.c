/*
 * src/user/linux_probe_pemap.c - C1-pe: what Wine's PE loader does to a
 * mapped image, across threads and processes, with every byte checked.
 *
 * ORDINARY musl Linux program; the same static-PIE ELF runs on the reference
 * kernel (strix, Linux) and in the HobbyOS guest.  Every row is ONE write(2);
 * the last row is "[LP] done rc=<fails>" (tools/gate/t3_linux_probe.sh).
 *
 * Wine maps a PE image (dlls/ntdll/unix/virtual.c map_image_into_view) as:
 * a PROT_NONE reservation; the header page and every section mapped
 * MAP_FIXED|MAP_PRIVATE PROT_READ|PROT_WRITE at its file offset (Wine's own
 * DLLs are 4 KiB file-aligned, so every section is a file mapping); section
 * tails memset and relocations written (copy-on-write); then mprotect per
 * section.  Unaligned images go through pread() into anonymous memory.  A
 * view is released by mapping PROT_NONE anonymous memory MAP_FIXED over it,
 * and the next image often lands at the same address.  Other threads of the
 * process run on other CPUs all the while.
 *
 *  V1 COW, memfd       thread A reads a MAP_PRIVATE page (the object's frame,
 *                      read-only leaf); thread B writes it (copy-on-write);
 *                      A must see B's value at once, as on Linux
 *  V2 COW, file        the same over a regular file (/tmp, or argv's first)
 *  V3 COW by read()    B fills the page with read(2) from a pipe (the kernel
 *                      writes the private page: vm_touch's copy)
 *  V4 MAP_FIXED anon   B replaces A's anonymous page with MAP_FIXED; A must
 *                      see the new page
 *  V5 MAP_FIXED file   B replaces it with a MAP_FIXED file page
 *  S1 storm            -p children at a time, -c in all, each running -i
 *                      Wine-style image loads of every data file: map, COW
 *                      tails + "relocations", mprotect per section, pread
 *                      copies, release by MAP_FIXED PROT_NONE; a sibling
 *                      thread reads each published view and churns its own
 *                      anonymous pages (decommit/recommit).  Every byte is
 *                      compared with the generator (splitmix64 of the file's
 *                      seed and word index).
 *
 * Data files: "pemapNN.bin" made by `linux_probe_pemap --gen DIR N SIZE`
 * (run on the host for a read-only NFS export, or in the guest for /EXT and
 * /tmp).  Without file arguments the probe makes its own in /tmp.
 * Arguments: [-p par] [-c children] [-i iters] [-s] [-n] [file-or-dir ...]
 *   -s  storm only (skip V1-V5)
 *   -n  the sibling does not judge what it reads (so a stale-TLB read cannot
 *       end a child early, and the owner's own checks run to the end)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PG 4096UL
#define ITERS 12
#define SLACK 2000 /* A's loop count allowed past "B's call returned" */
#define MAXF 16
#define VIEW_MAX (4UL << 20)  /* largest image the storm maps */
#define SLOTS 4               /* view addresses, reused round-robin */
#define SIB_PAGES 64

static int fails;
static int sib_judge = 1; /* -n: the sibling reads the views but does not judge them */
static void out(const char *fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- the data generator (host and guest use the same function) ---------- */
static uint64_t sm64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
static uint64_t gen_word(uint64_t seed, uint64_t i) { return sm64((seed << 32) ^ i ^ 0x5045u); }
/* byte `off` of a file of `size` bytes with `seed` (0 past EOF) */
static uint8_t gen_byte(uint64_t seed, uint64_t size, uint64_t off) {
  if (off >= size) return 0;
  return (uint8_t)(gen_word(seed, off / 8) >> (8 * (off % 8)));
}
static void gen_fill(uint64_t seed, uint64_t size, uint64_t off, uint8_t *dst, uint64_t n) {
  for (uint64_t k = 0; k < n; k++) dst[k] = gen_byte(seed, size, off + k);
}
static int gen_files(const char *dir, int n, uint64_t size) {
  static uint8_t buf[65536];
  for (int k = 0; k < n; k++) {
    char path[512];
    snprintf(path, sizeof path, "%s/pemap%02d.bin", dir, k);
    /* sizes differ per file and are never page multiples: the tail page is
       part file, part zeros (Wine memsets it) */
    uint64_t sz = size + (uint64_t)k * 12345u + 1000u;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { out("[LP] gen open %s errno=%d\n", path, errno); return -1; }
    for (uint64_t off = 0; off < sz;) {
      uint64_t c = sz - off < sizeof buf ? sz - off : sizeof buf;
      gen_fill((uint64_t)k + 1, sz, off, buf, c);
      if (write(fd, buf, c) != (ssize_t)c) { out("[LP] gen write errno=%d\n", errno); close(fd); return -1; }
      off += c;
    }
    fsync(fd);
    close(fd);
  }
  return 0;
}

struct dfile {
  char path[256];
  uint64_t seed, size;
};
static struct dfile df[MAXF];
static int ndf;

static int add_file(const char *path) {
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  int k;
  if (sscanf(b, "pemap%d.bin", &k) != 1 || ndf >= MAXF) return -1;
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  snprintf(df[ndf].path, sizeof df[ndf].path, "%s", path);
  df[ndf].seed = (uint64_t)k + 1;
  df[ndf].size = (uint64_t)st.st_size;
  ndf++;
  return 0;
}
static void add_arg(const char *a) {
  struct stat st;
  if (stat(a, &st) != 0) { out("[LP] note: %s absent (errno=%d), skipped\n", a, errno); return; }
  if (!S_ISDIR(st.st_mode)) { add_file(a); return; }
  DIR *d = opendir(a);
  if (!d) return;
  char names[MAXF][256];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < MAXF)
    if (!strncmp(e->d_name, "pemap", 5)) snprintf(names[n++], 256, "%s", e->d_name);
  closedir(d);
  for (int i = 0; i < n; i++) { /* directory order is not sorted on NFS */
    char p[600];
    snprintf(p, sizeof p, "%.300s/%.255s", a, names[i]);
    add_file(p);
  }
}

/* ---- V rows: one thread changes a mapping, another must see it ---------- */
static volatile uint64_t *va_page;
static volatile uint64_t va_old;
static volatile unsigned long va_count, va_seen_at;
static volatile uint64_t va_seen_val;
static volatile uint64_t va_until; /* V3: wait for this value (a kernel copy may tear) */
static volatile int va_until_set;
static volatile int va_go, va_done, va_hot;

static void *vis_a(void *v) {
  (void)v;
  while (!va_go) {}
  volatile uint64_t *p = va_page;
  for (;;) {
    uint64_t x = p[0];
    va_count++;
    if (va_count == 1000) va_hot = 1;
    if (x != va_old && (!va_until_set || x == va_until)) {
      va_seen_val = x;
      va_seen_at = va_count;
      va_done = 1;
      return 0;
    }
    if (va_done) return 0; /* B gave up */
  }
}

static int memfd(const char *name) { return (int)syscall(SYS_memfd_create, name, 0); }

enum { V_COW_MEMFD = 1, V_COW_FILE, V_COW_READ, V_FIX_ANON, V_FIX_FILE };

/* One iteration; 0 = ok, else a reason in why. */
static int vis_one(int row, const char *file, char *why, size_t whyn) {
  int fd = -1, pfd[2] = {-1, -1};
  uint64_t *pg = 0;
  uint64_t want = 0;
  if (row == V_COW_MEMFD) {
    fd = memfd("pemap");
    uint64_t w[512];
    for (int i = 0; i < 512; i++) w[i] = 0xA11CE000ULL + (uint64_t)i;
    if (fd < 0 || write(fd, w, sizeof w) != (ssize_t)sizeof w) { snprintf(why, whyn, "memfd errno=%d", errno); return 9; }
  } else if (row == V_COW_FILE || row == V_COW_READ || row == V_FIX_FILE) {
    fd = open(file, O_RDONLY);
    if (fd < 0) { snprintf(why, whyn, "open %s errno=%d", file, errno); return 9; }
  }
  if (row == V_FIX_ANON || row == V_FIX_FILE) {
    void *m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { snprintf(why, whyn, "mmap errno=%d", errno); return 9; }
    pg = (uint64_t *)m;
    pg[0] = 0xFEEDFACECAFEF00DULL;
  } else {
    void *m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { snprintf(why, whyn, "mmap fd errno=%d", errno); close(fd); return 9; }
    pg = (uint64_t *)m;
  }
  va_page = pg;
  va_old = pg[0]; /* a read fault: the object's frame, read-only leaf */
  va_count = va_seen_at = 0;
  va_seen_val = 0;
  va_go = va_done = va_hot = 0;
  va_until_set = row == V_COW_READ;
  va_until = ~va_old;
  pthread_t t;
  pthread_create(&t, 0, vis_a, 0);
  va_go = 1;
  while (!va_hot) {}
  while (va_count < 300000) {} /* A is hot: its CPU's TLB holds the page */
  int rc = 0;
  if (row == V_COW_MEMFD || row == V_COW_FILE) {
    want = ~va_old;
    pg[0] = want; /* write fault: copy-on-write */
  } else if (row == V_COW_READ) {
    want = ~va_old;
    if (pipe(pfd) != 0 || write(pfd[1], &want, 8) != 8 || read(pfd[0], pg, 8) != 8) rc = -1;
  } else if (row == V_FIX_ANON) {
    rc = mmap(pg, PG, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED;
    want = 0x0123456789ABCDEFULL;
    if (!rc) pg[0] = want;
  } else {
    rc = mmap(pg, PG, PROT_READ, MAP_FIXED | MAP_PRIVATE, fd, 0) == MAP_FAILED;
    want = pg[0];
    if (want == va_old) { snprintf(why, whyn, "file word equals the magic"); rc = -2; }
  }
  unsigned long c0 = va_count;
  if (rc) {
    va_done = 1;
    pthread_join(t, 0);
    if (rc != -2) snprintf(why, whyn, "call failed errno=%d", errno);
    return 9;
  }
  long t0 = now_ms();
  while (!va_done && now_ms() - t0 < 3000) {}
  int ret = 0;
  if (!va_done) {
    va_done = 1;
    snprintf(why, whyn, "A still read the old value 0x%llx 3 s after B's change (count %lu -> %lu)",
             (unsigned long long)va_old, c0, va_count);
    ret = 1;
  } else if (va_seen_val != want && !(row == V_FIX_ANON && va_seen_val == 0)) {
    /* V4: the fresh page reads 0 until B's store lands */
    snprintf(why, whyn, "A read 0x%llx, want 0x%llx", (unsigned long long)va_seen_val,
             (unsigned long long)want);
    ret = 3;
  } else if (va_seen_at > c0 + SLACK) {
    snprintf(why, whyn, "A read the old value %lu more times after B's change returned (c0=%lu)",
             va_seen_at - c0, c0);
    ret = 2;
  }
  pthread_join(t, 0);
  munmap(pg, PG);
  if (fd >= 0) close(fd);
  if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
  return ret;
}

/* ---- S1: the storm ------------------------------------------------------ */
struct sib {
  volatile int stop;
  /* touch request: read one word of every page of [tview, tview+tlen) */
  volatile uint64_t tview, tlen;
  volatile unsigned tgen, tack;
  /* check request: the published view, relocated by delta */
  volatile uint64_t view, vlen, delta;
  volatile int vfile;
  volatile unsigned gen, ack;
  volatile unsigned long bad, reads, churn;
  uint64_t *area;            /* sibling-owned anonymous pages */
  char msg[200];
};

/* relocation sites: one word per 4 KiB page at a page-dependent offset */
static uint64_t rel_off(uint64_t page, uint64_t delta) { return ((sm64(page ^ delta) % 500u) * 8u); }

static void *sib_main(void *v) {
  struct sib *s = (struct sib *)v;
  uint64_t tag = (uint64_t)getpid() << 32;
  unsigned round = 0;
  volatile uint64_t sink = 0;
  while (!s->stop) {
    /* (a) the loader is still writing the view: just read it, so this CPU's
       TLB holds its pages (Linux makes no promise yet) */
    unsigned tg = s->tgen;
    if (tg != s->tack) {
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      for (uint64_t o = 0; o < s->tlen; o += PG) sink += *(volatile uint64_t *)(s->tview + o);
      __atomic_thread_fence(__ATOMIC_RELEASE);
      s->tack = tg;
    }
    /* (b) the view is published: every relocated word must read relocated */
    unsigned g = s->gen;
    if (g != s->ack) {
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      uint64_t base = s->view, len = s->vlen, delta = s->delta;
      int fi = s->vfile;
      for (uint64_t pgo = 0; pgo + 8 <= len && !s->bad; pgo += PG) {
        uint64_t o = pgo + rel_off(pgo / PG, delta);
        if (o + 8 > df[fi].size || o + 8 > len) continue;
        uint64_t got = *(volatile uint64_t *)(base + o);
        uint64_t want = gen_word(df[fi].seed, o / 8) + delta;
        s->reads++;
        if (got != want && sib_judge) {
          s->bad++;
          snprintf(s->msg, sizeof s->msg, "sibling read %s+0x%llx = 0x%llx, want 0x%llx (unrelocated 0x%llx)",
                   df[fi].path, (unsigned long long)o, (unsigned long long)got,
                   (unsigned long long)want, (unsigned long long)(want - delta));
        }
      }
      __atomic_thread_fence(__ATOMIC_RELEASE);
      s->ack = g;
    }

    /* (c) churn our own pages: write, check, decommit, recommit, zero */
    round++;
    uint64_t *a = s->area;
    for (int p = 0; p < SIB_PAGES; p++) a[p * 512] = tag | ((uint64_t)round << 8) | (uint64_t)p;
    for (int p = 0; p < SIB_PAGES; p++)
      if (a[p * 512] != (tag | ((uint64_t)round << 8) | (uint64_t)p) && !s->bad) {
        s->bad++;
        snprintf(s->msg, sizeof s->msg, "sibling page %d lost its store (0x%llx)", p,
                 (unsigned long long)a[p * 512]);
      }
    if ((round & 7) == 0) {
      int half = (round & 8) ? SIB_PAGES / 2 : 0;
      void *d = (char *)a + (uint64_t)half * PG;
      mmap(d, (SIB_PAGES / 2) * PG, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      mmap(d, (SIB_PAGES / 2) * PG, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      for (int p = half; p < half + SIB_PAGES / 2; p++)
        if (a[p * 512] != 0 && !s->bad) {
          s->bad++;
          snprintf(s->msg, sizeof s->msg, "sibling recommitted page %d reads 0x%llx, want 0", p,
                   (unsigned long long)a[p * 512]);
        }
      s->churn++;
    }
  }
  return 0;
}

/* compare [base, base+len) of a view mapped from file offset `foff`, with
   relocations `delta` applied at rel_off (0 = none) and the bytes past
   `zero_from` (view-relative) expected zero; returns mismatching bytes and
   describes the first */
static uint64_t check_view(const char *tag, int fi, uint64_t base, uint64_t foff, uint64_t len,
                           uint64_t delta, uint64_t zero_from, char *why, size_t whyn) {
  static uint8_t exp[PG];
  uint64_t bad = 0;
  for (uint64_t pgo = 0; pgo < len; pgo += PG) {
    gen_fill(df[fi].seed, df[fi].size, foff + pgo, exp, PG);
    for (uint64_t k = 0; k < PG; k++)
      if (pgo + k >= zero_from) exp[k] = 0;
    if (delta) {
      uint64_t o = rel_off((foff + pgo) / PG, delta);
      if (foff + pgo + o + 8 <= df[fi].size && pgo + o + 8 <= zero_from) {
        uint64_t w;
        memcpy(&w, exp + o, 8);
        w += delta;
        memcpy(exp + o, &w, 8);
      }
    }
    const uint8_t *got = (const uint8_t *)(base + pgo);
    if (memcmp(got, exp, PG) == 0) continue;
    uint64_t first = PG, n = 0;
    for (uint64_t k = 0; k < PG; k++)
      if (got[k] != exp[k]) { if (first == PG) first = k; n++; }
    if (!bad) {
      uint64_t zeros = 0;
      for (uint64_t k = 0; k < PG; k++) zeros += got[k] == 0;
      uint64_t gw = 0, ew = 0;
      memcpy(&gw, got + (first & ~7UL), 8);
      memcpy(&ew, exp + (first & ~7UL), 8);
      snprintf(why, whyn, "%s %s off=0x%llx +0x%llx: %llu bytes differ, got 0x%016llx want 0x%016llx, zeros=%llu va=0x%llx",
               tag, df[fi].path, (unsigned long long)(foff + pgo), (unsigned long long)first,
               (unsigned long long)n, (unsigned long long)gw, (unsigned long long)ew,
               (unsigned long long)zeros, (unsigned long long)(base + pgo));
    }
    bad += n;
  }
  return bad;
}

static int wait_ack(volatile unsigned *ack, unsigned want) {
  long t0 = now_ms();
  while (*ack != want)
    if (now_ms() - t0 > 10000) return 1;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return 0;
}

static int child_main(int idx, int iters) {
  char why[400] = "";
  uint64_t bad = 0;
  int fds[MAXF];
  for (int f = 0; f < ndf; f++) {
    fds[f] = open(df[f].path, O_RDONLY);
    if (fds[f] < 0) { out("[LP] S1 child %d open %s errno=%d\n", idx, df[f].path, errno); return 2; }
  }
  /* Wine reserves its address space up front; views are carved out of it */
  uint64_t rsv_len = (uint64_t)(SLOTS + 1) * VIEW_MAX + SIB_PAGES * PG;
  char *rsv = mmap(0, rsv_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (rsv == MAP_FAILED) { out("[LP] S1 child %d reserve errno=%d\n", idx, errno); return 2; }
  struct sib s;
  memset(&s, 0, sizeof s);
  s.area = (uint64_t *)(rsv + (uint64_t)(SLOTS + 1) * VIEW_MAX);
  mmap(s.area, SIB_PAGES * PG, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  pthread_t st;
  pthread_create(&st, 0, sib_main, &s);
  uint64_t rng = sm64((uint64_t)getpid() * 7919u + (uint64_t)idx);
  unsigned long maps = 0, preads = 0;
  for (int it = 0; it < iters && !bad && !s.bad; it++) {
    for (int f = 0; f < ndf && !bad && !s.bad; f++) {
      rng = sm64(rng);
      uint64_t size = df[f].size < VIEW_MAX ? df[f].size : VIEW_MAX;
      uint64_t len = (size + PG - 1) & ~(PG - 1);
      char *view = rsv + (rng % SLOTS) * VIEW_MAX;
      uint64_t delta = (sm64(rng ^ 0xD1) & 0xFFFFF000ULL) + PG;
      int use_pread = (rng >> 20) % 4 == 0;
      if (!use_pread) {
        /* header page, then "sections" of 1..64 pages, each its own
           MAP_FIXED|MAP_PRIVATE RW file mapping at the matching offset */
        uint64_t o = 0;
        while (o < len) {
          uint64_t n = o == 0 ? PG : (1 + sm64(rng + o) % 64) * PG;
          if (n > len - o) n = len - o;
          if (mmap(view + o, n, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fds[f], (off_t)o) == MAP_FAILED) {
            snprintf(why, sizeof why, "mmap %s off=0x%llx errno=%d", df[f].path, (unsigned long long)o, errno);
            bad++;
            break;
          }
          maps++;
          o += n;
        }
        if (bad) break;
      } else {
        /* the unaligned-image path: RW anonymous memory, pread into it */
        mmap(view, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t o = 0;
        while (o < size) {
          uint64_t n = (1 + sm64(rng ^ o) % 48) * PG + (sm64(o) % 512);
          if (n > size - o) n = size - o;
          ssize_t r = pread(fds[f], view + o, n, (off_t)o);
          if (r != (ssize_t)n) {
            snprintf(why, sizeof why, "pread %s off=0x%llx n=%llu got %zd errno=%d", df[f].path,
                     (unsigned long long)o, (unsigned long long)n, r, errno);
            bad++;
            break;
          }
          preads++;
          o += n;
        }
        if (bad) break;
      }
      /* Wine reads the header right away (find_existing_module) */
      uint64_t b0 = check_view(use_pread ? "pread-hdr" : "map-hdr", f, (uint64_t)view, 0, PG, 0,
                               size, why, sizeof why);
      if (b0) { bad += b0; break; }
      /* let the sibling read the view before it is written */
      s.tview = (uint64_t)view;
      s.tlen = len;
      __atomic_thread_fence(__ATOMIC_RELEASE);
      s.tgen++;
      if (wait_ack(&s.tack, s.tgen)) { snprintf(why, sizeof why, "sibling never touched the view"); bad++; break; }
      /* tail memset + relocations of the read-only "sections" (copy-on-write
         before the final protections) */
      if (size & (PG - 1)) memset(view + size, 0, len - size);
      for (uint64_t pgo = 0; pgo < len; pgo += PG) {
        uint64_t o = pgo + rel_off(pgo / PG, delta);
        if ((pgo / PG) % 3 != 2 && o + 8 <= size) *(uint64_t *)(view + o) += delta;
      }
      /* the final protections: header R, then RX / R / RW pages */
      for (uint64_t pgo = 0; pgo < len; pgo += PG) {
        uint64_t k = pgo / PG;
        int pr = k == 0 ? PROT_READ : k % 3 == 0 ? PROT_READ | PROT_EXEC : k % 3 == 1 ? PROT_READ
                                                                        : PROT_READ | PROT_WRITE;
        mprotect(view + pgo, PG, pr);
      }
      /* the sibling reads the view again, then the RW "data" pages take their
         first write (a DLL's first store to a global: copy-on-write with no
         mprotect after it) */
      s.tgen++;
      if (wait_ack(&s.tack, s.tgen)) { snprintf(why, sizeof why, "sibling never touched the view"); bad++; break; }
      for (uint64_t pgo = 2 * PG; pgo < len; pgo += 3 * PG) {
        uint64_t o = pgo + rel_off(pgo / PG, delta);
        if (o + 8 <= size) *(uint64_t *)(view + o) += delta;
      }
      /* publish to the sibling, then check every byte ourselves */
      s.view = (uint64_t)view;
      s.vlen = size;
      s.vfile = f;
      s.delta = delta;
      __atomic_thread_fence(__ATOMIC_RELEASE);
      s.gen++;
      bad += check_view(use_pread ? "pread" : "map", f, (uint64_t)view, 0, len, delta, size, why, sizeof why);
      sched_yield();
      bad += bad ? 0 : check_view(use_pread ? "pread-2" : "map-2", f, (uint64_t)view, 0, len, delta, size, why, sizeof why);
      if (wait_ack(&s.ack, s.gen)) { if (!bad) snprintf(why, sizeof why, "sibling never checked the view"); bad++; }
      /* a plain pread of a random range into a fresh anonymous buffer */
      {
        static uint8_t rb[64 * 1024];
        uint64_t off = sm64(rng ^ 0xBEEF) % size;
        uint64_t n = 1 + sm64(rng ^ 0xF00D) % sizeof rb;
        if (n > size - off) n = size - off;
        ssize_t r = pread(fds[f], rb, n, (off_t)off);
        if (r != (ssize_t)n) {
          if (!bad) snprintf(why, sizeof why, "pread64k %s off=0x%llx n=%llu got %zd", df[f].path,
                             (unsigned long long)off, (unsigned long long)n, r);
          bad++;
        } else {
          for (uint64_t k = 0; k < n; k++)
            if (rb[k] != gen_byte(df[f].seed, df[f].size, off + k)) {
              if (!bad) snprintf(why, sizeof why, "pread64k %s off=0x%llx: byte 0x%02x want 0x%02x",
                                 df[f].path, (unsigned long long)(off + k), rb[k],
                                 gen_byte(df[f].seed, df[f].size, off + k));
              bad++;
              break;
            }
        }
      }
      /* release the view the way Wine does: PROT_NONE over it */
      mmap(view, len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
  }
  s.stop = 1;
  pthread_join(st, 0);
  if (bad || s.bad) {
    out("[LP] S1 child %d pid %d BAD: %s%s%s\n", idx, getpid(), bad ? why : "", bad && s.bad ? " | " : "",
        s.bad ? s.msg : "");
    return 1;
  }
  if (idx < 2)
    out("[LP] S1 child %d pid %d ok maps=%lu preads=%lu sib_reads=%lu sib_churn=%lu\n", idx, getpid(),
        maps, preads, s.reads, s.churn);
  return 0;
}

static int storm(int par, int total, int iters) {
  if (ndf == 0) { out("[LP] FAIL S1: no data files\n"); return 1; }
  long t0 = now_ms();
  int started = 0, running = 0, bad = 0, done = 0;
  while (done < total) {
    while (running < par && started < total) {
      pid_t pid = fork();
      if (pid == 0) _exit(child_main(started, iters));
      if (pid < 0) { out("[LP] S1 fork errno=%d\n", errno); bad++; break; }
      started++;
      running++;
    }
    int st = 0;
    pid_t w = wait(&st);
    if (w < 0) break;
    running--;
    done++;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      bad++;
      if (!WIFEXITED(st)) out("[LP] S1 child pid %d died, signal %d\n", w, WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    }
  }
  out("[LP] %s S1 storm: %d children (%d at a time) x %d iters x %d files, %d bad, %ld ms\n",
      bad ? "FAIL" : "PASS", done, par, iters, ndf, bad, now_ms() - t0);
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  int par = 8, total = 16, iters = 4, storm_only = 0;
  if (argc >= 5 && !strcmp(argv[1], "--gen"))
    return gen_files(argv[2], atoi(argv[3]), strtoull(argv[4], 0, 0)) ? 1 : 0;
  int ai = 1;
  for (; ai < argc && argv[ai][0] == '-'; ai++) {
    if (!strcmp(argv[ai], "-s")) storm_only = 1;
    else if (!strcmp(argv[ai], "-n")) sib_judge = 0;
    else if (ai + 1 < argc && !strcmp(argv[ai], "-p")) par = atoi(argv[++ai]);
    else if (ai + 1 < argc && !strcmp(argv[ai], "-c")) total = atoi(argv[++ai]);
    else if (ai + 1 < argc && !strcmp(argv[ai], "-i")) iters = atoi(argv[++ai]);
  }
  for (; ai < argc; ai++) add_arg(argv[ai]);
  if (ndf == 0) {
    mkdir("/tmp/pemap", 0755);
    if (gen_files("/tmp/pemap", 3, 600 * 1024) == 0) add_arg("/tmp/pemap");
  }
  out("[LP] pemap: %d data files (first %s, %llu bytes), par=%d children=%d iters=%d\n", ndf,
      ndf ? df[0].path : "-", ndf ? (unsigned long long)df[0].size : 0ULL, par, total, iters);
  if (!storm_only) {
    static const char *names[] = {"", "V1 COW memfd", "V2 COW file", "V3 COW by read()",
                                  "V4 MAP_FIXED anon", "V5 MAP_FIXED file"};
    for (int row = 1; row <= 5; row++) {
      if (row != V_COW_MEMFD && row != V_FIX_ANON && ndf == 0) continue;
      int bad = 0, nbad = 0;
      char why[240] = "", first[240] = "";
      for (int it = 0; it < ITERS; it++) {
        int r = vis_one(row, ndf ? df[0].path : "", why, sizeof why);
        if (r == 9) { out("[LP] FAIL %s iter %d: %s\n", names[row], it, why); bad = 1; break; }
        if (r) { if (!nbad) snprintf(first, sizeof first, "iter %d: %s", it, why); nbad++; }
      }
      if (nbad) { out("[LP] FAIL %s: %d of %d iterations stale; first %s\n", names[row], nbad, ITERS, first); bad = 1; }
      else if (!bad) out("[LP] PASS %s (%d iterations, slack %d)\n", names[row], ITERS, SLACK);
      fails += bad;
    }
  }
  fails += storm(par, total, iters);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
