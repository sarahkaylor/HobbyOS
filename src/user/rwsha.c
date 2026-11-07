/* rwsha.c — M6/L31 diagnostics: read every file under a directory through the
 * guest's own VFS and print a SHA-256 per file, hash a large file through an
 * mmap (the path Mono uses for assemblies), exercise MAP_PRIVATE copy-on-write,
 * and dump /proc/self/maps exactly as the guest renders it.
 *
 * Why this exists: the managed layer dies while resolving its own types
 * ("Unable to find method ProcessFrame in [UnityEngine.CoreModule.dll]"), and
 * the death site moves run to run.  Before any kernel instrumentation, the
 * cheapest hypothesis to retire is "the bytes Mono mmapped are not the bytes
 * on the host".  Staging (mcopy) can be perfect and the guest's FAT16 read or
 * page-cache fill can still return the wrong cluster.
 *
 * Runs as a normal native app (desktop Apps menu, or a wave process).  Every
 * row goes to SYS_WRITE_CONSOLE because a desktop-launched child's fd 1 is the
 * window protocol pipe, not a log.
 *
 * Build (guest):
 *   clang <USER_CFLAGS> -DRWSHA_GUEST -c src/user/rwsha.c
 *   ld.lld -T src/user/linker.ld -e _start ... obj/intel/libc.a
 * Build (host, to validate the hash core):
 *   gcc -DRWSHA_HOST -o /tmp/rwsha_host src/user/rwsha.c
 */
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ SHA-256 */
typedef struct {
  uint32_t h[8];
  uint64_t total;
  uint8_t buf[64];
  unsigned n;
} sha256_ctx;

static const uint32_t K256[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
  0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
  0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
  0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
  0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
  0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
  0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
  0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
  0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
           ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
  e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
    uint32_t ch = (e & f) ^ (~e & g);
    t1 = h + S1 + ch + K256[i] + w[i];
    uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    t2 = S0 + maj;
    h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
  }
  c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
  c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c) {
  c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u;
  c->h[3]=0xa54ff53au; c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu;
  c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
  c->total = 0; c->n = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->total += len;
  while (len) {
    unsigned take = 64 - c->n;
    if (take > len) take = (unsigned)len;
    for (unsigned i = 0; i < take; i++) c->buf[c->n + i] = p[i];
    c->n += take; p += take; len -= take;
    if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
  }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->total * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t z = 0;
  while (c->n != 56) sha256_update(c, &z, 1);
  uint8_t lenbuf[8];
  for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - 8*i));
  c->total -= 8;                 /* the length field is not message data */
  sha256_update(c, lenbuf, 8);
  for (int i = 0; i < 8; i++) {
    out[i*4]   = (uint8_t)(c->h[i] >> 24);
    out[i*4+1] = (uint8_t)(c->h[i] >> 16);
    out[i*4+2] = (uint8_t)(c->h[i] >> 8);
    out[i*4+3] = (uint8_t)(c->h[i]);
  }
}

/* ---------------------------------------------------------------- host test */
#ifdef RWSHA_HOST
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static void hex(const uint8_t d[32], char o[65]) {
  static const char *H = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { o[i*2] = H[d[i] >> 4]; o[i*2+1] = H[d[i] & 15]; }
  o[64] = 0;
}

int main(int argc, char **argv) {
  static uint8_t buf[1 << 16];
  for (int a = 1; a < argc; a++) {
    int fd = open(argv[a], O_RDONLY);
    if (fd < 0) { printf("MISS %s\n", argv[a]); return 1; }
    sha256_ctx c; sha256_init(&c);
    long n, tot = 0;
    while ((n = (long)read(fd, buf, sizeof buf)) > 0) { sha256_update(&c, buf, (size_t)n); tot += n; }
    close(fd);
    uint8_t d[32]; char h[65];
    sha256_final(&c, d); hex(d, h);
    printf("%s  %s  %ld\n", h, argv[a], tot);
  }
  return 0;
}
#else
/* ------------------------------------------------------------------- guest */
#include "libc.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "rwsha_files.h"   /* generated: RW_FILES[], RW_NFILES */

static void pc(const char *s) { print_console(s); }

/* print_dec() writes fd 1, which is the window protocol pipe for a
 * desktop-launched child, so every number here must go to the console too. */
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

static void hex32(const uint8_t *d, char o[65]) {
  static const char *H = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { o[i*2] = H[d[i] >> 4]; o[i*2+1] = H[d[i] & 15]; }
  o[64] = 0;
}

static uint8_t iobuf[1 << 16];

/* Hash an open fd from its current offset to EOF.  Returns bytes hashed. */
static long hash_fd(int fd, uint8_t out[32]) {
  sha256_ctx c; sha256_init(&c);
  long n, tot = 0;
  while ((n = (long)read(fd, iobuf, sizeof iobuf)) > 0) {
    sha256_update(&c, iobuf, (size_t)n);
    tot += n;
  }
  sha256_final(&c, out);
  return tot;
}

static void put_path(const char *dir, const char *name) {
  pc(dir); pc(name);
}

/* One row per file: SHA <name> <hex> <bytes>. */
static int hash_one(const char *dir, const char *name) {
  char path[160];
  int k = 0;
  for (; dir[k] && k < 150; k++) path[k] = dir[k];
  for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
  path[k] = 0;

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    pc("[SHA] MISS "); pc(name); pc("\n");
    return -1;
  }
  uint8_t d[32]; char h[65];
  long n = hash_fd(fd, d);
  close(fd);
  hex32(d, h);
  pc("[SHA] "); pc(name); pc(" "); pc(h); pc(" "); pd(n); pc("\n");
  return 0;
}

/* stat() vs fstat() vs lseek(SEEK_END): Mono maps an assembly with the size
 * it gets from fstat(), so a wrong size here is a truncated image even when
 * the bytes on disk are perfect. */
static void stat_pass(const char *dir, const char *name, unsigned long expected) {
  char path[160];
  int k = 0;
  for (; dir[k] && k < 150; k++) path[k] = dir[k];
  for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
  path[k] = 0;

  struct stat sb;
  long ss = (stat(path, &sb) == 0) ? (long)sb.st_size : -1;
  int fd = open(path, O_RDONLY);
  long fs = -1, end = -1;
  if (fd >= 0) {
    struct stat fb;
    if (fstat(fd, &fb) == 0) fs = (long)fb.st_size;
    end = (long)lseek(fd, 0, 2 /* SEEK_END */);
    close(fd);
  }
  pc("[STAT] "); pc(name);
  pc(" stat="); pd(ss);
  pc(" fstat="); pd(fs);
  pc(" seekend="); pd(end);
  pc(" expected="); pd((long)expected);
  pc((ss == fs && fs == end && (unsigned long)fs == expected) ? " OK\n" : " MISMATCH\n");
}

/* A big read split into 64 KiB chunks with an lseek before every chunk: the
 * assembly mmap path re-reads a page at an arbitrary file offset, so a
 * sequential read that is correct can still hide a broken seek/fill. */
static void seek_pass(const char *dir, const char *name, unsigned long size) {
  char path[160];
  int k = 0;
  for (; dir[k] && k < 150; k++) path[k] = dir[k];
  for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
  path[k] = 0;

  int fd = open(path, O_RDONLY);
  if (fd < 0) { pc("[SEEK] MISS "); pc(name); pc("\n"); return; }
  sha256_ctx c; sha256_init(&c);
  unsigned long off = 0, total = 0;
  while (off < size) {
    unsigned long want = size - off;
    if (want > sizeof iobuf) want = sizeof iobuf;
    if (lseek(fd, (off_t)off, 0) != (off_t)off) { pc("[SEEK] lseek-fail\n"); break; }
    long n = (long)read(fd, iobuf, want);
    if (n <= 0) break;
    sha256_update(&c, iobuf, (size_t)n);
    total += (unsigned long)n;
    off += (unsigned long)n;
  }
  close(fd);
  uint8_t d[32]; char h[65];
  sha256_final(&c, d); hex32(d, h);
  pc("[SEEK] "); pc(name); pc(" "); pc(h); pc(" "); pd((long)total); pc("\n");
}

/* The path Mono actually uses: map the file and hash the mapping. */
static void mmap_pass(const char *dir, const char *name, unsigned long size) {
  char path[160];
  int k = 0;
  for (; dir[k] && k < 150; k++) path[k] = dir[k];
  for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
  path[k] = 0;

  int fd = open(path, O_RDONLY);
  if (fd < 0) { pc("[MMAP] MISS "); pc(name); pc("\n"); return; }
  void *m = mmap(0, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (m == MAP_FAILED) { pc("[MMAP] FAIL "); pc(name); pc("\n"); close(fd); return; }
  sha256_ctx c; sha256_init(&c);
  sha256_update(&c, m, (size_t)size);
  uint8_t d[32]; char h[65];
  sha256_final(&c, d); hex32(d, h);
  pc("[MMAP] "); pc(name); pc(" "); pc(h); pc(" "); pd((long)size); pc("\n");
  munmap(m, (size_t)size);
  close(fd);
}

/* MAP_PRIVATE write: the mapping must diverge, the file must not change. */
static void cow_pass(const char *dir, const char *name, unsigned long size) {
  char path[160];
  int k = 0;
  for (; dir[k] && k < 150; k++) path[k] = dir[k];
  for (int i = 0; name[i] && k < 158; i++) path[k++] = name[i];
  path[k] = 0;
  if (size < 8192) return;

  int fd = open(path, O_RDONLY);
  if (fd < 0) { pc("[COW] MISS\n"); return; }
  uint8_t before[32]; long bn = hash_fd(fd, before);
  close(fd);

  fd = open(path, O_RDONLY);
  void *m = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (m == MAP_FAILED) { pc("[COW] MAPFAIL "); pc(name); pc("\n"); close(fd); return; }
  volatile uint8_t *b = (volatile uint8_t *)m;
  uint8_t orig0 = b[0], orig4k = b[4096];
  b[0] = (uint8_t)(orig0 ^ 0xA5);
  b[4096] = (uint8_t)(orig4k ^ 0x5A);
  int inplace = (b[0] == (uint8_t)(orig0 ^ 0xA5)) && (b[4096] == (uint8_t)(orig4k ^ 0x5A));
  munmap(m, (size_t)size);
  close(fd);

  fd = open(path, O_RDONLY);
  uint8_t after[32]; long an = hash_fd(fd, after);
  close(fd);
  int same = (bn == an) && (bn >= 0);
  int equal = 1;
  for (int i = 0; i < 32; i++) if (before[i] != after[i]) equal = 0;

  pc("[COW] "); pc(name);
  pc(" inplace="); pd(inplace ? 1 : 0);
  pc(" file_same="); pd(same ? 1 : 0);
  pc(" digests_equal="); pd(equal ? 1 : 0);
  pc(" bytes="); pd(an < 0 ? -1 : an);
  pc("\n");
}

static void dump_maps(void) {
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) { pc("[MAPS] OPEN-FAIL\n"); return; }
  char buf[4096];
  char line[600];
  int li = 0, lines = 0;
  long n;
  while ((n = (long)read(fd, buf, sizeof buf)) > 0) {
    for (long i = 0; i < n; i++) {
      char ch = buf[i];
      if (ch == '\n') {
        line[li] = 0;
        pc("[MAPS] "); pc(line); pc("\n");
        li = 0; lines++;
      } else if (li < (int)sizeof line - 1) {
        line[li++] = ch;
      }
    }
  }
  if (li) { line[li] = 0; pc("[MAPS] "); pc(line); pc("\n"); lines++; }
  close(fd);
  pc("[MAPS] lines="); pd(lines); pc("\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  pc("[SHA] begin pid="); pd(getpid()); pc(" files="); pd(RW_NFILES); pc("\n");

  int ok = 0, bad = 0;
  for (int i = 0; i < RW_NFILES; i++)
    (ok += (hash_one("/RWMANAGED/", RW_FILES[i].name) == 0));

  /* the three assemblies whose metadata resolution is failing */
  for (int i = 0; i < RW_NFILES; i++) {
    const char *n = RW_FILES[i].name;
    if (strcmp(n, "mscorlib.dll") == 0 ||
        strcmp(n, "UnityEngine.CoreModule.dll") == 0 ||
        strcmp(n, "Assembly-CSharp.dll") == 0) {
      mmap_pass("/RWMANAGED/", n, RW_FILES[i].size);
      stat_pass("/RWMANAGED/", n, RW_FILES[i].size);
      cow_pass("/RWMANAGED/", n, RW_FILES[i].size);
    }
  }
  /* the biggest assembly: the 64 KiB-with-seek pass */
  {
    int big = 0;
    for (int i = 1; i < RW_NFILES; i++)
      if (RW_FILES[i].size > RW_FILES[big].size) big = i;
    pc("[SEEK] largest="); pc(RW_FILES[big].name);
    pc(" size="); pd((long)RW_FILES[big].size); pc("\n");
    seek_pass("/RWMANAGED/", RW_FILES[big].name, RW_FILES[big].size);
  }
  dump_maps();
  pc("[SHA] done ok="); pd(ok); pc("/"); pd(RW_NFILES); pc("\n");
  exit(0);
}
#endif
