/* rwread1.c — L34: prove (or disprove) the short-read hypothesis end to end.
 *
 * The wave-14 bug was an NFS read that returned 1024 bytes for a 2915-byte
 * request.  Every layer above it was blamed before that.  This probe asks the
 * question the game's first scene asks, with no caller loop to hide behind:
 * open each of RimWorld's real asset files, then issue ONE read(fd, buf, size)
 * for the whole file and compare the returned count and a SHA-256 against the
 * bytes on the host.
 *
 * A short count for a regular file below EOF is a kernel bug (POSIX: read()
 * returns the full count unless EOF/error).  A correct count with a wrong
 * digest is a fill/offset bug.  Both would make Unity reject a scene file and
 * fall into its teardown path.
 *
 * Runs as a normal native app (desktop Apps menu) and mounts the NFS export
 * itself, so it does not depend on rwlaunch.  Rows go to SYS_WRITE_CONSOLE:
 * a desktop-launched child's fd 1 is the window protocol pipe, not a log.
 *
 * Build: tools/rw/run-guest-read1.sh
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
  c->total -= 8;
  sha256_update(c, lenbuf, 8);
  for (int i = 0; i < 8; i++) {
    out[i*4]   = (uint8_t)(c->h[i] >> 24);
    out[i*4+1] = (uint8_t)(c->h[i] >> 16);
    out[i*4+2] = (uint8_t)(c->h[i] >> 8);
    out[i*4+3] = (uint8_t)(c->h[i]);
  }
}

/* ------------------------------------------------------------------- guest */
#include "libc.h"
#include "rwnfs_lane.h" /* M6/L63: /NFSEXPORT */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "rwread_files.h"   /* generated: RW_READ_FILES[], RW_NREADS */

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

static void hex32(const uint8_t *d, char o[65]) {
  static const char *H = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { o[i*2] = H[d[i] >> 4]; o[i*2+1] = H[d[i] & 15]; }
  o[64] = 0;
}

static int same_hex(const char *a, const char *b) {
  for (int i = 0; i < 64; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* ONE read() for the whole file.  That is the contract Unity's FileStream
 * relies on: a regular-file read returns the requested count unless it hits
 * EOF or a real error. */
static void one_shot(const char *path, const char *name,
                     unsigned long host_size, const char *host_sha) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    pc("[R1] MISS "); pc(name); pc("\n");
    return;
  }
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  struct stat sb;
  long fsz = (fstat(fd, &sb) == 0) ? (long)sb.st_size : -1;
  long end = (long)lseek(fd, 0, 2);
  lseek(fd, 0, 0);

  unsigned long want = host_size;
  if (fsz > 0 && (unsigned long)fsz > want) want = (unsigned long)fsz;
  if (want == 0) { close(fd); return; }

  void *buf = mmap(0, (size_t)want, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buf == MAP_FAILED) {
    pc("[R1] NOMAP "); pc(name); pc(" want="); pd((long)want); pc("\n");
    close(fd);
    return;
  }
  long got = (long)read(fd, buf, (size_t)want);
  close(fd);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 +
            (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;

  uint8_t d[32]; char hex[65];
  if (got > 0) {
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, buf, (size_t)got);
    sha256_final(&c, d); hex32(d, hex);
  } else {
    for (int i = 0; i < 64; i++) hex[i] = '0';
    hex[64] = 0;
  }
  munmap(buf, (size_t)want);

  int count_ok = (got == (long)host_size);
  int sha_ok = same_hex(hex, host_sha);
  pc("[R1] "); pc(name);
  pc(" want="); pd((long)host_size);
  pc(" fstat="); pd(fsz);
  pc(" seekend="); pd(end);
  pc(" got="); pd(got);
  pc(" ms="); pd(ms);
  pc(" sha="); pc(hex);
  pc(count_ok ? " count=OK" : " count=SHORT");
  pc(sha_ok ? " sha=OK" : " sha=DIFF");
  pc("\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  pc("[R1] begin pid="); pd(getpid()); pc(" files="); pd(RW_NREADS); pc("\n");

  /* The game's own launcher mounts this; a probe launched from the Apps menu
   * has to do it itself.  DHCP may still be settling at first call. */
  int mrc = -1;
  for (int i = 0; i < 100; i++) {
    /* M6/L63: per-lane export from /NFSEXPORT (default if absent). */
    static char rbuf[128];
    int rl = 0;
    const char *re = rw_nfs_export_path("/srv/nfs/rimworld");
    for (int k = 0; "10.0.2.2"[k] && rl < 100; k++) rbuf[rl++] = "10.0.2.2"[k];
    rbuf[rl++] = ':';
    for (int k = 0; re[k] && rl < 126; k++) rbuf[rl++] = re[k];
    rbuf[rl] = 0;
    mrc = mount(rbuf, "/NFS");
    if (mrc == 0) break;
    usleep(200000);
  }
  pc("[R1] mount /NFS rc="); pd(mrc); pc("\n");
  if (mrc != 0) {
    pc("[R1] NO-NFS: cannot test the asset path\n");
    exit(1);
  }

  int ok = 0;
  for (int i = 0; i < RW_NREADS; i++) {
    one_shot(RW_READ_FILES[i].path, RW_READ_FILES[i].name,
             RW_READ_FILES[i].size, RW_READ_FILES[i].sha);
    ok++;
  }
  pc("[R1] done files="); pd(ok); pc("\n");
  exit(0);
}
