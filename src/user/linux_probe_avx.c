/*
 * src/user/linux_probe_avx.c — cp P1 (docs/cyberpunk.md AD-4, F2): AVX register
 * state under preemption and through a Linux-shaped XSAVE signal frame.
 *
 * ORDINARY musl Linux program: no HobbyOS header, so the same static-PIE binary
 * runs on a Linux box (the reference) and inside the intel guest.  Every row is
 * "[LP] ..." and the last is "[LP] done rc=<fails>"; tools/gate/t3_linux_probe.sh
 * avx only accepts rc=0.  The AVX work lives in target("avx") functions and
 * inline asm so the rest builds with the probes' plain flags.
 *
 *   C0  CPUID/XGETBV: when the CPU has XSAVE+AVX, the kernel must have set
 *       CR4.OSXSAVE (CPUID.1:ECX[27]) and XCR0 & 6 == 6.  A CPU without XSAVE
 *       (QEMU qemu64) runs the SSE-only variant of every case below, which is
 *       the FXSAVE fallback's check.
 *   C1  auxv: AT_HWCAP == CPUID.1:EDX (Linux's ELF_HWCAP), AT_HWCAP2 reported,
 *       AT_MINSIGSTKSZ present and large enough for the FP image.
 *   C2  2 x ncpu threads (12..32; more threads than CPUs, so the timer must
 *       preempt)
 *       each hold a distinct pattern in ymm0..15 across sched_yield loops and
 *       across busy spins with the registers live; rdtsc gaps inside the spin
 *       count how often the thread was actually interrupted mid-hold.
 *   C3  a SIGUSR1 handler that clobbers every ymm register must not leak into
 *       the interrupted code; the frame must carry the interrupted state
 *       (UC_FP_XSTATE, FP_XSTATE_MAGIC1/2, the AVX area holding the upper
 *       halves), fpregs 64-byte aligned.
 *   C4  a handler that EDITS the saved upper halves in the frame's XSAVE area
 *       (found via the magic words + CPUID.(0xD,2).EBX) and one legacy xmm
 *       lower half must see both edits in the registers after rt_sigreturn.
 *   C5  the same delivery on a sigaltstack of max(AT_MINSIGSTKSZ, MINSIGSTKSZ)
 *       bytes: the frame must fit and the state must survive.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif
#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif
#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u
#define UC_FP_XSTATE 0x1

static int fails;

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
  uint32_t a = leaf, b, c = sub, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}

static uint64_t xgetbv0(void) {
  uint32_t lo, hi;
  __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
  return ((uint64_t)hi << 32) | lo;
}

static uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* ---- mode ---------------------------------------------------------------- */

static int g_avx;          /* 1: run the ymm variants; 0: xmm (FXSAVE) only */
static uint32_t g_avx_off; /* CPUID.(0xD,2).EBX: AVX component offset       */
#define REGB (g_avx ? 32 : 16) /* bytes per vector register in our images */

/* ---- register hold primitives -------------------------------------------
 * Each loads 16 vector registers from `in`, does something that may switch
 * the thread out or deliver a signal while they are live, and stores them to
 * `out`.  Images are 16 x 32 bytes (ymm) or 16 x 16 bytes (xmm). */

#define YLOAD(r, o) "vmovdqu " #o "(%[in]), %%ymm" #r "\n"
#define YSTORE(r, o) "vmovdqu %%ymm" #r ", " #o "(%[out])\n"
#define YLOAD_ALL                                                          \
  YLOAD(0, 0) YLOAD(1, 32) YLOAD(2, 64) YLOAD(3, 96) YLOAD(4, 128)          \
  YLOAD(5, 160) YLOAD(6, 192) YLOAD(7, 224) YLOAD(8, 256) YLOAD(9, 288)     \
  YLOAD(10, 320) YLOAD(11, 352) YLOAD(12, 384) YLOAD(13, 416)               \
  YLOAD(14, 448) YLOAD(15, 480)
#define YSTORE_ALL                                                         \
  YSTORE(0, 0) YSTORE(1, 32) YSTORE(2, 64) YSTORE(3, 96) YSTORE(4, 128)     \
  YSTORE(5, 160) YSTORE(6, 192) YSTORE(7, 224) YSTORE(8, 256)               \
  YSTORE(9, 288) YSTORE(10, 320) YSTORE(11, 352) YSTORE(12, 384)            \
  YSTORE(13, 416) YSTORE(14, 448) YSTORE(15, 480)
#define XLOAD(r, o) "movdqu " #o "(%[in]), %%xmm" #r "\n"
#define XSTORE(r, o) "movdqu %%xmm" #r ", " #o "(%[out])\n"
#define XLOAD_ALL                                                          \
  XLOAD(0, 0) XLOAD(1, 16) XLOAD(2, 32) XLOAD(3, 48) XLOAD(4, 64)           \
  XLOAD(5, 80) XLOAD(6, 96) XLOAD(7, 112) XLOAD(8, 128) XLOAD(9, 144)       \
  XLOAD(10, 160) XLOAD(11, 176) XLOAD(12, 192) XLOAD(13, 208)               \
  XLOAD(14, 224) XLOAD(15, 240)
#define XSTORE_ALL                                                         \
  XSTORE(0, 0) XSTORE(1, 16) XSTORE(2, 32) XSTORE(3, 48) XSTORE(4, 64)      \
  XSTORE(5, 80) XSTORE(6, 96) XSTORE(7, 112) XSTORE(8, 128) XSTORE(9, 144)  \
  XSTORE(10, 160) XSTORE(11, 176) XSTORE(12, 192) XSTORE(13, 208)           \
  XSTORE(14, 224) XSTORE(15, 240)
#define VCLOB                                                               \
  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",    \
      "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"

/* spin: `n` pause iterations with the registers live; returns the number of
   rdtsc gaps > thr (the thread was off the CPU or in an interrupt). */
#define SPIN_BODY                                                           \
  "rdtsc\n shl $32, %%rdx\n or %%rdx, %%rax\n mov %%rax, %%r9\n"            \
  "xor %%r10d, %%r10d\n mov %[n], %%r8\n"                                   \
  "1: pause\n rdtsc\n shl $32, %%rdx\n or %%rdx, %%rax\n"                   \
  "mov %%rax, %%rcx\n sub %%r9, %%rcx\n mov %%rax, %%r9\n"                  \
  "cmp %[thr], %%rcx\n jb 2f\n inc %%r10\n"                                 \
  "2: dec %%r8\n jnz 1b\n mov %%r10, %[gaps]\n"

__attribute__((target("avx"))) static uint64_t
ymm_spin(const uint8_t *in, uint8_t *out, uint64_t n, uint64_t thr) {
  uint64_t gaps = 0;
  __asm__ volatile(YLOAD_ALL SPIN_BODY YSTORE_ALL
                   : [gaps] "=m"(gaps)
                   : [in] "r"(in), [out] "r"(out), [n] "r"(n), [thr] "r"(thr)
                   : "rax", "rcx", "rdx", "r8", "r9", "r10", "memory", "cc",
                     VCLOB);
  return gaps;
}

static uint64_t xmm_spin(const uint8_t *in, uint8_t *out, uint64_t n,
                         uint64_t thr) {
  uint64_t gaps = 0;
  __asm__ volatile(XLOAD_ALL SPIN_BODY XSTORE_ALL
                   : [gaps] "=m"(gaps)
                   : [in] "r"(in), [out] "r"(out), [n] "r"(n), [thr] "r"(thr)
                   : "rax", "rcx", "rdx", "r8", "r9", "r10", "memory", "cc",
                     VCLOB);
  return gaps;
}

#define YIELD_BODY                                                          \
  "mov %[m], %%r8\n"                                                        \
  "1: mov $24, %%eax\n syscall\n dec %%r8\n jnz 1b\n"

__attribute__((target("avx"))) static void
ymm_yield(const uint8_t *in, uint8_t *out, uint64_t m) {
  __asm__ volatile(YLOAD_ALL YIELD_BODY YSTORE_ALL
                   :
                   : [in] "r"(in), [out] "r"(out), [m] "r"(m)
                   : "rax", "rcx", "r11", "r8", "memory", "cc", VCLOB);
}

static void xmm_yield(const uint8_t *in, uint8_t *out, uint64_t m) {
  __asm__ volatile(XLOAD_ALL YIELD_BODY XSTORE_ALL
                   :
                   : [in] "r"(in), [out] "r"(out), [m] "r"(m)
                   : "rax", "rcx", "r11", "r8", "memory", "cc", VCLOB);
}

/* tgkill(self) INSIDE the hold: the signal is delivered on that syscall's
   return, while the registers carry the pattern. */
#define KILL_BODY "syscall\n"

__attribute__((target("avx"))) static long
ymm_selfkill(const uint8_t *in, uint8_t *out, long tgid, long tid, long sig) {
  long rax = SYS_tgkill;
  __asm__ volatile(YLOAD_ALL KILL_BODY YSTORE_ALL
                   : "+a"(rax)
                   : [in] "r"(in), [out] "r"(out), "D"(tgid), "S"(tid), "d"(sig)
                   : "rcx", "r11", "memory", "cc", VCLOB);
  return rax;
}

static long xmm_selfkill(const uint8_t *in, uint8_t *out, long tgid, long tid,
                         long sig) {
  long rax = SYS_tgkill;
  __asm__ volatile(XLOAD_ALL KILL_BODY XSTORE_ALL
                   : "+a"(rax)
                   : [in] "r"(in), [out] "r"(out), "D"(tgid), "S"(tid), "d"(sig)
                   : "rcx", "r11", "memory", "cc", VCLOB);
  return rax;
}

/* Clobber every vector register from inside a handler. */
__attribute__((target("avx"))) static void ymm_clobber(void) {
  __asm__ volatile("vcmpps $15, %%ymm0, %%ymm0, %%ymm0\n"
                   "vmovdqa %%ymm0, %%ymm1\n vmovdqa %%ymm0, %%ymm2\n"
                   "vmovdqa %%ymm0, %%ymm3\n vmovdqa %%ymm0, %%ymm4\n"
                   "vmovdqa %%ymm0, %%ymm5\n vmovdqa %%ymm0, %%ymm6\n"
                   "vmovdqa %%ymm0, %%ymm7\n vmovdqa %%ymm0, %%ymm8\n"
                   "vmovdqa %%ymm0, %%ymm9\n vmovdqa %%ymm0, %%ymm10\n"
                   "vmovdqa %%ymm0, %%ymm11\n vmovdqa %%ymm0, %%ymm12\n"
                   "vmovdqa %%ymm0, %%ymm13\n vmovdqa %%ymm0, %%ymm14\n"
                   "vmovdqa %%ymm0, %%ymm15\n" ::: VCLOB);
}

static void xmm_clobber(void) {
  __asm__ volatile("pcmpeqb %%xmm0, %%xmm0\n"
                   "movdqa %%xmm0, %%xmm1\n movdqa %%xmm0, %%xmm2\n"
                   "movdqa %%xmm0, %%xmm3\n movdqa %%xmm0, %%xmm4\n"
                   "movdqa %%xmm0, %%xmm5\n movdqa %%xmm0, %%xmm6\n"
                   "movdqa %%xmm0, %%xmm7\n movdqa %%xmm0, %%xmm8\n"
                   "movdqa %%xmm0, %%xmm9\n movdqa %%xmm0, %%xmm10\n"
                   "movdqa %%xmm0, %%xmm11\n movdqa %%xmm0, %%xmm12\n"
                   "movdqa %%xmm0, %%xmm13\n movdqa %%xmm0, %%xmm14\n"
                   "movdqa %%xmm0, %%xmm15\n" ::: VCLOB);
}

static void fill(uint8_t *img, int seed) {
  for (int i = 0; i < 16 * 32; i++)
    img[i] = (uint8_t)(seed * 37 + i * 11 + 1 + ((i & 16) ? 0x55 : 0));
}

static void hold_spin(const uint8_t *in, uint8_t *out, uint64_t n,
                      uint64_t thr, uint64_t *gaps) {
  *gaps = g_avx ? ymm_spin(in, out, n, thr) : xmm_spin(in, out, n, thr);
}

static void hold_yield(const uint8_t *in, uint8_t *out, uint64_t m) {
  if (g_avx) ymm_yield(in, out, m); else xmm_yield(in, out, m);
}

static long hold_selfkill(const uint8_t *in, uint8_t *out, int sig) {
  long tgid = getpid(), tid = syscall(SYS_gettid);
  return g_avx ? ymm_selfkill(in, out, tgid, tid, sig)
               : xmm_selfkill(in, out, tgid, tid, sig);
}

/* ---- C0/C1 -------------------------------------------------------------- */

static void c0_cpu(void) {
  uint32_t r[4], m[4];
  cpuid(0, 0, m);
  cpuid(1, 0, r);
  int xsave = (r[2] >> 26) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
  int avx2 = 0;
  if (m[0] >= 7) {
    uint32_t s[4];
    cpuid(7, 0, s);
    avx2 = (s[1] >> 5) & 1;
  }
  uint64_t xcr0 = osxsave ? xgetbv0() : 0;
  uint32_t xsz = 0;
  if (osxsave && m[0] >= 0xD) {
    uint32_t d0[4], d2[4];
    cpuid(0xD, 0, d0);
    cpuid(0xD, 2, d2);
    xsz = d0[1];
    g_avx_off = d2[1];
  }
  row("[LP] C0 cpu xsave=%d osxsave=%d avx=%d avx2=%d xcr0=0x%llx "
      "xsave_size=%u avx_off=%u\n",
      xsave, osxsave, avx, avx2, (unsigned long long)xcr0, xsz, g_avx_off);
  if (xsave && avx) {
    if (!osxsave || (xcr0 & 6) != 6) {
      row("[LP] C0 FAIL CPU has XSAVE+AVX but the OS did not enable it "
          "(OSXSAVE=%d XCR0=0x%llx)\n", osxsave, (unsigned long long)xcr0);
      fails++;
      return;
    }
    g_avx = 1;
    row("[LP] C0 PASS OSXSAVE=1, XCR0&6==6; ymm variants (XCR0 beyond AVX: "
        "0x%llx)\n", (unsigned long long)(xcr0 & ~7ull));
  } else {
    if (osxsave) {
      row("[LP] C0 FAIL OSXSAVE set on a CPU without XSAVE+AVX\n");
      fails++;
    }
    row("[LP] C0 PASS no XSAVE/AVX on this CPU: xmm (FXSAVE fallback) "
        "variants\n");
  }
}

static unsigned long g_minsig;

static void c1_auxv(void) {
  uint32_t r[4];
  cpuid(1, 0, r);
  errno = 0;
  unsigned long hw = getauxval(AT_HWCAP);
  unsigned long hw2 = getauxval(AT_HWCAP2);
  int e2 = errno;
  errno = 0;
  unsigned long ms = getauxval(AT_MINSIGSTKSZ);
  int em = errno;
  g_minsig = ms;
  row("[LP] C1 auxv AT_HWCAP=0x%lx (cpuid1.edx=0x%x) AT_HWCAP2=0x%lx%s "
      "AT_MINSIGSTKSZ=%lu%s MINSIGSTKSZ=%ld\n",
      hw, r[3], hw2, e2 ? "(absent)" : "", ms, em ? "(absent)" : "",
      (long)MINSIGSTKSZ);
  int bad = 0;
  if (hw != (unsigned long)r[3]) {
    row("[LP] C1 FAIL AT_HWCAP is not CPUID.1:EDX\n");
    bad = 1;
  }
  /* the frame: header + the FP image; 512 without XSAVE, ~836+ with AVX */
  unsigned long need = g_avx ? 832 + 4 + 560 : 512 + 560;
  if (em || ms < need) {
    row("[LP] C1 FAIL AT_MINSIGSTKSZ %lu < %lu (frame + FP image)\n", ms, need);
    bad = 1;
  }
  if (!bad) row("[LP] C1 PASS\n");
  fails += bad;
}

/* ---- C2: threads -------------------------------------------------------- */

#define NTHR_MAX 32
static uint64_t g_thr_tsc; /* tsc ticks in ~200 us */

struct targ {
  int id;
  int bad_yield, bad_spin;
  uint64_t gaps;
  int cpus_seen;
};

static void *c2_thread(void *vp) {
  struct targ *a = vp;
  uint8_t in[512] __attribute__((aligned(32)));
  uint8_t out[512] __attribute__((aligned(32)));
  uint64_t cpumask = 0;
  fill(in, 100 + a->id);
  for (int it = 0; it < 40; it++) {
    memset(out, 0, sizeof out);
    hold_yield(in, out, 25);
    if (memcmp(in, out, 16 * (size_t)REGB) != 0) a->bad_yield++;
    memset(out, 0, sizeof out);
    uint64_t g = 0;
    hold_spin(in, out, 200000, g_thr_tsc, &g);
    a->gaps += g;
    if (memcmp(in, out, 16 * (size_t)REGB) != 0) a->bad_spin++;
    int c = sched_getcpu();
    if (c >= 0 && c < 64) cpumask |= 1ull << c;
  }
  a->cpus_seen = __builtin_popcountll(cpumask);
  return NULL;
}

static void c2_threads(void) {
  long t0 = now_ns();
  uint64_t s0 = rdtsc();
  while (now_ns() - t0 < 20000000L) {
  }
  uint64_t per_us = (rdtsc() - s0) / 20000;
  g_thr_tsc = per_us ? per_us * 200 : 200000;

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  int NTHR = (int)(2 * (ncpu > 0 ? ncpu : 6));
  if (NTHR < 12) NTHR = 12;
  if (NTHR > NTHR_MAX) NTHR = NTHR_MAX;
  pthread_t th[NTHR_MAX];
  struct targ a[NTHR_MAX];
  long w0 = now_ns();
  for (int i = 0; i < NTHR; i++) {
    memset(&a[i], 0, sizeof a[i]);
    a[i].id = i;
    if (pthread_create(&th[i], NULL, c2_thread, &a[i]) != 0) {
      row("[LP] C2 FAIL pthread_create %d\n", i);
      fails++;
      return;
    }
  }
  int bad = 0;
  uint64_t gaps = 0;
  int maxcpus = 0;
  for (int i = 0; i < NTHR; i++) {
    pthread_join(th[i], NULL);
    bad += a[i].bad_yield + a[i].bad_spin;
    gaps += a[i].gaps;
    if (a[i].cpus_seen > maxcpus) maxcpus = a[i].cpus_seen;
    if (a[i].bad_yield || a[i].bad_spin)
      row("[LP] C2 thread %d CORRUPT yield=%d spin=%d\n", i, a[i].bad_yield,
          a[i].bad_spin);
  }
  long ms = (now_ns() - w0) / 1000000L;
  row("[LP] C2 %s threads=%d ncpu=%ld iters=40 reg=%s tsc/us=%llu "
      "interrupted_holds=%llu max_cpus_per_thread=%d wall_ms=%ld\n",
      bad ? "FAIL" : "PASS", NTHR, ncpu, g_avx ? "ymm" : "xmm",
      (unsigned long long)per_us, (unsigned long long)gaps, maxcpus, ms);
  if (bad) fails++;
  else if (gaps == 0) {
    row("[LP] C2 FAIL no hold was ever interrupted (preemption not exercised)\n");
    fails++;
  }
}

/* ---- C3/C4/C5: signals --------------------------------------------------- */

static volatile int h_mode;  /* 1 clobber, 2 edit */
static volatile int h_ran;
static volatile unsigned long h_flags, h_fpal, h_m1, h_ext, h_xsz, h_m2,
    h_bv, h_xf;
static volatile int h_frame_ok;  /* saved image == interrupted pattern */
static volatile uintptr_t h_local;
static uint8_t g_pat[512] __attribute__((aligned(32)));
static uint8_t g_edit_hi[16 * 16];
static uint8_t g_edit_lo2[16];

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void handler(int sig, siginfo_t *si, void *ucv) {
  (void)sig; (void)si;
  volatile int here = 0;
  h_local = (uintptr_t)&here;
  ucontext_t *uc = ucv;
  uint8_t *fp = (uint8_t *)uc->uc_mcontext.fpregs;
  h_flags = uc->uc_flags;
  h_fpal = (uintptr_t)fp & 63;
  h_m1 = h_ext = h_xsz = h_m2 = h_bv = h_xf = 0;
  int ok = 1;
  if (fp) {
    h_m1 = rd32(fp + 464);
    h_ext = rd32(fp + 468);
    h_xf = rd64(fp + 472);
    h_xsz = rd32(fp + 480);
    if (h_m1 == FP_XSTATE_MAGIC1 && h_xsz >= 576 && h_xsz < 65536) {
      h_m2 = rd32(fp + h_xsz);
      h_bv = rd64(fp + 512);
    }
    /* the legacy region must hold the interrupted xmm lower halves */
    for (int r = 0; r < 16; r++)
      if (memcmp(fp + 160 + 16 * r, g_pat + (size_t)REGB * r, 16) != 0) ok = 0;
    /* and, with AVX, the XSAVE AVX area the upper halves */
    if (g_avx) {
      if (h_m1 != FP_XSTATE_MAGIC1 || h_m2 != FP_XSTATE_MAGIC2 || !(h_bv & 4))
        ok = 0;
      else
        for (int r = 0; r < 16; r++)
          if (memcmp(fp + g_avx_off + 16 * r, g_pat + 32 * r + 16, 16) != 0)
            ok = 0;
    }
  } else {
    ok = 0;
  }
  h_frame_ok = ok;
  if (h_mode == 1 || h_mode == 3) {
    if (g_avx) ymm_clobber(); else xmm_clobber();
  } else if (h_mode == 2 && fp) {
    if (g_avx && h_m1 == FP_XSTATE_MAGIC1 && h_m2 == FP_XSTATE_MAGIC2) {
      for (int r = 0; r < 16; r++)
        memcpy(fp + g_avx_off + 16 * r, g_edit_hi + 16 * r, 16);
      uint64_t bv = rd64(fp + 512) | 4;
      memcpy(fp + 512, &bv, 8);
    }
    memcpy(fp + 160 + 16 * 2, g_edit_lo2, 16); /* xmm2 lower half */
    if (g_avx) ymm_clobber(); else xmm_clobber();
  }
  h_ran++;
}

static int install(int onstack) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | (onstack ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  return sigaction(SIGUSR1, &sa, NULL);
}

static void frame_row(const char *tag) {
  row("[LP] %s frame uc_flags=0x%lx fpregs%%64=%lu magic1=0x%lx ext=%lu "
      "xfeatures=0x%lx xstate_size=%lu magic2=0x%lx xstate_bv=0x%lx "
      "saved_state_ok=%d\n",
      tag, h_flags, h_fpal, h_m1, h_ext, h_xf, h_xsz, h_m2, h_bv, h_frame_ok);
}

/* Shared frame-shape assertions for C3..C5. */
static int frame_shape_bad(const char *tag) {
  int bad = 0;
  if (h_fpal != 0) { row("[LP] %s FAIL fpregs not 64-byte aligned\n", tag); bad = 1; }
  if (g_avx) {
    if (!(h_flags & UC_FP_XSTATE)) { row("[LP] %s FAIL UC_FP_XSTATE clear\n", tag); bad = 1; }
    if (h_m1 != FP_XSTATE_MAGIC1 || h_m2 != FP_XSTATE_MAGIC2) {
      row("[LP] %s FAIL magic words\n", tag); bad = 1;
    }
    if (h_ext != h_xsz + 4) { row("[LP] %s FAIL extended_size != xstate_size+4\n", tag); bad = 1; }
    if ((h_xf & 7) != 7) { row("[LP] %s FAIL xfeatures lacks x87|SSE|AVX\n", tag); bad = 1; }
  } else if (h_m1 == FP_XSTATE_MAGIC1) {
    row("[LP] %s FAIL FP_XSTATE_MAGIC1 on a CPU without XSAVE\n", tag); bad = 1;
  }
  if (!h_frame_ok) { row("[LP] %s FAIL frame does not hold the interrupted state\n", tag); bad = 1; }
  return bad;
}

static void c3_clobber(void) {
  uint8_t out[512] __attribute__((aligned(32)));
  fill(g_pat, 7);
  install(0);
  h_mode = 1;
  h_ran = 0;
  memset(out, 0, sizeof out);
  long rc = hold_selfkill(g_pat, out, SIGUSR1);
  frame_row("C3");
  int bad = frame_shape_bad("C3");
  if (rc != 0 || h_ran != 1) { row("[LP] C3 FAIL rc=%ld ran=%d\n", rc, h_ran); bad = 1; }
  if (memcmp(out, g_pat, 16 * (size_t)REGB) != 0) {
    row("[LP] C3 FAIL handler's register clobber leaked into the interrupted code\n");
    bad = 1;
  }
  row("[LP] C3 %s handler clobber isolated (%s)\n", bad ? "FAIL" : "PASS",
      g_avx ? "ymm0..15" : "xmm0..15");
  fails += bad;
}

static void c4_edit(void) {
  uint8_t out[512] __attribute__((aligned(32)));
  uint8_t want[512];
  fill(g_pat, 9);
  for (int i = 0; i < 16 * 16; i++) g_edit_hi[i] = (uint8_t)(0xA0 ^ (i * 7));
  for (int i = 0; i < 16; i++) g_edit_lo2[i] = (uint8_t)(0x3C + i);
  memcpy(want, g_pat, sizeof want);
  if (g_avx)
    for (int r = 0; r < 16; r++) memcpy(want + 32 * r + 16, g_edit_hi + 16 * r, 16);
  memcpy(want + (size_t)REGB * 2, g_edit_lo2, 16);
  install(0);
  h_mode = 2;
  h_ran = 0;
  memset(out, 0, sizeof out);
  long rc = hold_selfkill(g_pat, out, SIGUSR1);
  frame_row("C4");
  int bad = frame_shape_bad("C4");
  if (rc != 0 || h_ran != 1) { row("[LP] C4 FAIL rc=%ld ran=%d\n", rc, h_ran); bad = 1; }
  for (int r = 0; r < 16; r++) {
    if (memcmp(out + (size_t)REGB * r, want + (size_t)REGB * r, (size_t)REGB) != 0) {
      row("[LP] C4 FAIL reg %d after sigreturn is not the edited frame value\n", r);
      bad = 1;
    }
  }
  row("[LP] C4 %s frame edits applied at rt_sigreturn (%s)\n",
      bad ? "FAIL" : "PASS",
      g_avx ? "16 ymm upper halves via XSAVE area + xmm2 lower" : "xmm2 lower");
  fails += bad;
}

static void c5_altstack(void) {
  uint8_t out[512] __attribute__((aligned(32)));
  unsigned long sz = g_minsig > (unsigned long)MINSIGSTKSZ ? g_minsig
                                                           : (unsigned long)MINSIGSTKSZ;
  void *mem = aligned_alloc(64, sz);
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = mem;
  ss.ss_size = sz;
  if (!mem || sigaltstack(&ss, NULL) != 0) {
    row("[LP] C5 FAIL sigaltstack(%lu): errno=%d\n", sz, errno);
    fails++;
    return;
  }
  fill(g_pat, 11);
  install(1);
  h_mode = 3;
  h_ran = 0;
  h_local = 0;
  memset(out, 0, sizeof out);
  long rc = hold_selfkill(g_pat, out, SIGUSR1);
  frame_row("C5");
  int bad = frame_shape_bad("C5");
  int on = h_local >= (uintptr_t)mem && h_local < (uintptr_t)mem + sz;
  if (rc != 0 || h_ran != 1) { row("[LP] C5 FAIL rc=%ld ran=%d\n", rc, h_ran); bad = 1; }
  if (!on) { row("[LP] C5 FAIL handler did not run on the altstack\n"); bad = 1; }
  if (memcmp(out, g_pat, 16 * (size_t)REGB) != 0) {
    row("[LP] C5 FAIL state lost on the altstack delivery\n");
    bad = 1;
  }
  row("[LP] C5 %s altstack of %lu bytes carries the frame (on_altstack=%d)\n",
      bad ? "FAIL" : "PASS", sz, on);
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);
  fails += bad;
}

int main(void) {
  row("[LP] avx: start pid=%d\n", (int)getpid());
  c0_cpu();
  c1_auxv();
  c2_threads();
  c3_clobber();
  c4_edit();
  c5_altstack();
  row("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
