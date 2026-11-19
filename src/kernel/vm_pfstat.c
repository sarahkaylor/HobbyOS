/*
 * src/kernel/vm_pfstat.c -- H7-fault: what demand faults cost while the game
 * streams (docs/cp-fault.md §2).
 *
 * H4-kglitch's [AWAY] rows showed the game's audio-clock threads spending
 * 16-25 % of their absences "in the kernel, not in a syscall" (T).  T is a
 * fault OR an interrupt, and a sample cannot say which, nor what the fault
 * was doing.  These rows can:
 *
 *   [PFK] t= dt= n= us= kmode= rep= <kind>=n/us/max_us[/io=us/rpc=n] ...
 *        faults that ended in the interval, by the path that resolved them
 *        (vm_pfstat.h enum pf_kind); us is the time inside vm_handle_fault /
 *        vm_touch; io the part spent fetching the page's source (page cache
 *        or device); rpc the faults that put an NFS READ on the wire;
 *        kmode the #PFs taken in the kernel (a syscall touching user memory);
 *        rep a fault on the page the same thread faulted on < 10 ms before.
 *   [PFH] t= all=<17 buckets> file=<17 buckets>
 *        latency histogram, buckets < 1, 2, 4, ... 32768 us, then the rest.
 *   [PFL] t= lw=sum/max lh=sum/max vml=n/w=sum/max@line/h=sum/max@line
 *        zero=n/us/max fresh=n/us shoot=n/us/max/cpus= warm=n zp=hit/miss/made
 *        vm_lock wait (lw) and hold (lh) inside faults; vml every vm_lock
 *        pair in vm.c with the source line of the longest wait and hold;
 *        frame zeroing (fresh: the first frame handed out from a 2 MiB guest
 *        physical chunk this boot, i.e. likely guest RAM the host has never
 *        backed); TLB shootdowns (IPIs to other CPUs, synchronous).
 *   [PFI] t= pf=n/us/max tmr=n/us/max ipi=n/us/max oth=n/us/max
 *        every trap from user mode that returned to the same thread, by
 *        class: #PF (the whole trap), the timer (32, 0x84), IPIs (0x81
 *        resched, 0x82 TLB), anything else (devices).
 *   [PFT] t= top=tid:pf_us:irq_us:name,...
 *        the four threads with the most #PF + interrupt time in the interval.
 *
 * One set per PF_EMIT_MS while faults happen, PF_SETS sets per boot.  The
 * per-thread totals also feed [AWAY]'s pf=/irq= fields (linux_compat_away.c).
 * Per-CPU cumulative counters live in one cache-line record per CPU (the
 * C1-k7 rule); the emitter diffs them against its own snapshot, so no CPU
 * resets another's counters (only the interval maxima, which are racy on
 * purpose: a lost maximum is a diagnostic blur, not a wrong count).
 */
#include <stdint.h>

#include "kline.h"
#include "process.h"
#include "vm_pfstat.h"

#if defined(__x86_64__)

extern uint32_t get_cpuid(void);
extern uint64_t timer_get_ms(void);
extern uint64_t timer_tsc_per_ms(void);

#define PF_EMIT_MS 10000
#define PF_SETS 400
#define PF_REP_US 10000
#define PF_CHUNK_SHIFT 21                 /* 2 MiB */
#define PF_CHUNK_WORDS 512                /* 64 GiB of guest physical */

enum { PFT_PF = 0, PFT_TMR, PFT_IPI, PFT_OTH, PFT_N };

struct pf_cpu {
  /* vm_lock hold in progress on this CPU (irqsave: never preempted) */
  uint64_t lk_t0;
  int lk_site;
  int lk_slot;          /* thread slot + 1 whose open fault takes the hold */
  int fresh;            /* frame_alloc's last frame opened a fresh chunk */
  /* cumulative counters */
  uint32_t n[PFK_N];
  uint64_t tsc[PFK_N], iot[PFK_N];
  uint32_t ion[PFK_N];
  uint32_t hist[PF_HB], histf[PF_HB];
  uint32_t kmode_n, rep;
  uint64_t lw_sum, lh_sum;
  uint32_t gl_n;
  uint64_t glw_sum, glh_sum;
  uint32_t z_n, zf_n;
  uint64_t z_tsc, zf_tsc;
  uint32_t sh_n, sh_tg;
  uint64_t sh_tsc;
  uint32_t tr_n[PFT_N];
  uint64_t tr_tsc[PFT_N];
  /* round 2: shootdowns by site */
  int sh_site;
  uint32_t ss_n[PFS_N], ss_tg[PFS_N], ss_run[PFS_N], ss_slow[PFS_N];
  uint64_t ss_tsc[PFS_N];
  uint64_t ss_mx[PFS_N];
  uint32_t slow_cpu[MAX_CPUS]; /* slow acks by target CPU */
  uint32_t slow_infault;        /* ... whose thread was inside a fault (IRQs off) */
  uint32_t pvflush;             /* targets the host flushed (preempted vCPUs) */
  uint32_t tg_n[PFR_N];         /* round 2: timer-interrupt segments */
  uint64_t tg_tsc[PFR_N], tg_mx[PFR_N];
  /* interval maxima (the emitter zeroes them) */
  uint64_t mx[PFK_N];
  uint64_t lw_mx, lh_mx, glw_mx, glh_mx, z_mx, sh_mx;
  int glw_site, glh_site;
  uint64_t tr_mx[PFT_N];
} __attribute__((aligned(64)));
static struct pf_cpu pf_pc[MAX_CPUS];

/* The fault a thread has open (a fault can park in NFS I/O and resume on
   another CPU, so this is the thread's, not the CPU's). */
struct pf_open {
  uint64_t t0, va, io, lw, lh, zt, sh;
  int depth, kmode, kind;
  uint32_t rpc;
};
static struct pf_open pf_op[MAX_PROCESSES];
static uint64_t pf_last_va[MAX_PROCESSES], pf_last_t[MAX_PROCESSES];
/* per-thread totals (tsc): #PF traps and interrupts from user mode */
static uint64_t pf_thr_pf_t[MAX_PROCESSES], pf_thr_irq_t[MAX_PROCESSES];
static uint64_t pf_chunk_seen[PF_CHUNK_WORDS];

static inline struct pf_cpu *pf_me(void) {
  uint32_t c = get_cpuid();
  return &pf_pc[c < MAX_CPUS ? c : 0];
}
static inline int pf_slot(void) {
  struct process *p = current_process();
  return (p && p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
}

uint64_t pfstat_tsc_to_us(uint64_t tsc) {
  uint64_t tpm = timer_tsc_per_ms();
  return tpm ? tsc * 1000u / tpm : 0;
}
static int pf_bucket(uint64_t tsc) {
  uint64_t us = pfstat_tsc_to_us(tsc);
  if (us == 0) return 0;
  int b = 64 - __builtin_clzll(us); /* us in [2^(b-1), 2^b) -> bucket b */
  return b < PF_HB ? b : PF_HB - 1;
}

/* [PFX]: one row per single fault or user-mode interrupt of PF_LONG_US or
 * more, so a long event can be matched (tid, at=) to an [AWAY] absence or an
 * [NFSPIPE] stall:
 *   [PFX] F tid= at= us= kind= va= io= lw= lh= zero= shoot= rpc= km=
 *   [PFX] I tid= at= us= vec=
 * at= is the uptime (ms) the event ended. */
#define PF_LONG_US 2000
#define PF_LONG_ROWS 400
static int pf_long_rows;
static const char *const pf_kname[PFK_N];
static uint64_t pf_long_tsc(void) { return (uint64_t)PF_LONG_US * (timer_tsc_per_ms() / 1000u); }
static void pf_long_row(char what, int k, uint64_t dt, const struct pf_open *o) {
  if (pf_long_rows >= PF_LONG_ROWS) return;
  pf_long_rows++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[PFX] ");
  kl_c(&kl, what);
  kl_s(&kl, " tid=");
  kl_i(&kl, pf_slot());
  kl_s(&kl, " at=");
  kl_i(&kl, (long)timer_get_ms());
  kl_s(&kl, " us=");
  kl_i(&kl, (long)pfstat_tsc_to_us(dt));
  if (o) {
    kl_s(&kl, " kind=");
    kl_s(&kl, pf_kname[k]);
    kl_s(&kl, " va=");
    kl_hex(&kl, (unsigned long)o->va);
    kl_s(&kl, " io=");
    kl_i(&kl, (long)pfstat_tsc_to_us(o->io));
    kl_s(&kl, " lw=");
    kl_i(&kl, (long)pfstat_tsc_to_us(o->lw));
    kl_s(&kl, " lh=");
    kl_i(&kl, (long)pfstat_tsc_to_us(o->lh));
    kl_s(&kl, " zero=");
    kl_i(&kl, (long)pfstat_tsc_to_us(o->zt));
    kl_s(&kl, " shoot=");
    kl_i(&kl, (long)pfstat_tsc_to_us(o->sh));
    kl_s(&kl, " rpc=");
    kl_i(&kl, (long)o->rpc);
    kl_s(&kl, " km=");
    kl_i(&kl, o->kmode);
  } else {
    kl_s(&kl, " vec=");
    kl_i(&kl, k);
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

void pfstat_begin(int kmode, uint64_t va) {
  int s = pf_slot();
  struct pf_open *o = &pf_op[s];
  if (o->depth++ > 0) return;
  uint64_t now = pf_tsc();
  o->t0 = now;
  o->va = va & ~0xFFFull;
  o->io = o->lw = o->lh = o->zt = o->sh = 0;
  o->kmode = kmode;
  o->kind = PFK_ERR;
  o->rpc = 0;
  if (s && pf_last_va[s] == o->va && now - pf_last_t[s] < PF_REP_US * (timer_tsc_per_ms() / 1000u))
    pf_me()->rep++;
  pf_last_va[s] = o->va;
  pf_last_t[s] = now;
}

void pfstat_kind(int kind) {
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth > 0 && kind >= 0 && kind < PFK_N) o->kind = kind;
}

void pfstat_io(uint64_t tsc) {
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth > 0) o->io += tsc;
}

void pfstat_rpc(void) {
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth > 0) o->rpc++;
}

void pfstat_end(void) {
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth <= 0) { o->depth = 0; return; }
  if (--o->depth > 0) return;
  uint64_t dt = pf_tsc() - o->t0;
  struct pf_cpu *c = pf_me();
  int k = o->kind;
  c->n[k]++;
  c->tsc[k] += dt;
  c->iot[k] += o->io;
  if (o->rpc) c->ion[k]++;
  if (dt > c->mx[k]) c->mx[k] = dt;
  int b = pf_bucket(dt);
  c->hist[b]++;
  if (k == PFK_NFS || k == PFK_EXT2 || k == PFK_FAT) c->histf[b]++;
  if (o->kmode) c->kmode_n++;
  c->lw_sum += o->lw;
  c->lh_sum += o->lh;
  if (o->lw > c->lw_mx) c->lw_mx = o->lw;
  if (o->lh > c->lh_mx) c->lh_mx = o->lh;
  if (dt >= pf_long_tsc()) pf_long_row('F', k, dt, o);
}

void pfstat_lock_acquired(uint64_t t_before, int site) {
  uint64_t now = pf_tsc();
  struct pf_cpu *c = pf_me();
  uint64_t w = now - t_before;
  c->gl_n++;
  c->glw_sum += w;
  if (w > c->glw_mx) { c->glw_mx = w; c->glw_site = site; }
  c->lk_t0 = now;
  c->lk_site = site;
  int s = pf_slot();
  if (pf_op[s].depth > 0) {
    pf_op[s].lw += w;
    c->lk_slot = s + 1;
  } else {
    c->lk_slot = 0;
  }
}

void pfstat_lock_releasing(void) {
  struct pf_cpu *c = pf_me();
  if (!c->lk_t0) return;
  uint64_t h = pf_tsc() - c->lk_t0;
  c->lk_t0 = 0;
  c->glh_sum += h;
  if (h > c->glh_mx) { c->glh_mx = h; c->glh_site = c->lk_site; }
  if (c->lk_slot) {
    struct pf_open *o = &pf_op[c->lk_slot - 1];
    if (o->depth > 0) o->lh += h;
    c->lk_slot = 0;
  }
}

void pfstat_frame_alloc(uint64_t phys) {
  uint64_t ch = phys >> PF_CHUNK_SHIFT;
  int fresh = 0;
  if (ch < (uint64_t)PF_CHUNK_WORDS * 64u) {
    uint64_t bit = 1ull << (ch & 63u);
    fresh = (__atomic_fetch_or(&pf_chunk_seen[ch >> 6], bit, __ATOMIC_RELAXED) & bit) == 0;
  }
  pf_me()->fresh = fresh;
}

void pfstat_zeroed(uint64_t phys, uint64_t tsc) {
  (void)phys;
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth > 0) o->zt += tsc;
  struct pf_cpu *c = pf_me();
  c->z_n++;
  c->z_tsc += tsc;
  if (tsc > c->z_mx) c->z_mx = tsc;
  if (c->fresh) {
    c->zf_n++;
    c->zf_tsc += tsc;
    c->fresh = 0;
  }
}

void pfstat_tseg(int seg, uint64_t tsc) {
  if (seg < 0 || seg >= PFR_N) return;
  struct pf_cpu *c = pf_me();
  c->tg_n[seg]++;
  c->tg_tsc[seg] += tsc;
  if (tsc > c->tg_mx[seg]) c->tg_mx[seg] = tsc;
}

int pfstat_pid_in_fault(int pid) {
  return pid > 0 && pid < MAX_PROCESSES && pf_op[pid].depth > 0;
}

void pfstat_shoot_site(int site) {
  if (site > 0 && site < PFS_N) pf_me()->sh_site = site;
}

void pfstat_shoot(uint64_t tsc, int targets, int running, int slow, uint32_t slowmask) {
  struct pf_open *o = &pf_op[pf_slot()];
  if (o->depth > 0) o->sh += tsc;
  struct pf_cpu *c = pf_me();
  int st = c->sh_site;
  c->sh_site = 0;
  if (st < 0 || st >= PFS_N) st = 0;
  if (!targets) return; /* nobody else ran the AS: no IPI, nothing to count */
  c->sh_n++;
  c->sh_tsc += tsc;
  c->sh_tg += (uint32_t)targets;
  if (tsc > c->sh_mx) c->sh_mx = tsc;
  c->ss_n[st]++;
  c->ss_tsc[st] += tsc;
  c->ss_tg[st] += (uint32_t)targets;
  c->ss_run[st] += (uint32_t)running;
  c->ss_slow[st] += (uint32_t)slow;
  if (tsc > c->ss_mx[st]) c->ss_mx[st] = tsc;
  for (int k = 0; k < MAX_CPUS; k++)
    if (slowmask & (1u << k)) c->slow_cpu[k]++;
}
void pfstat_shoot_infault(int n) { pf_me()->slow_infault += (uint32_t)n; }
void pfstat_shoot_pv(int n) { pf_me()->pvflush += (uint32_t)n; }

uint64_t pfstat_thr_pf(int slot) {
  return (slot > 0 && slot < MAX_PROCESSES) ? pf_thr_pf_t[slot] : 0;
}
uint64_t pfstat_thr_irq(int slot) {
  return (slot > 0 && slot < MAX_PROCESSES) ? pf_thr_irq_t[slot] : 0;
}

/* ---- the row set ---------------------------------------------------- */

static volatile int pf_emitting;
static uint64_t pf_last_emit_ms;
static int pf_sets;
static const char *const pf_kname[PFK_N] = {
  "anon", "shobj", "nfs", "ext2", "fat", "img", "cow", "priv", "retry", "err"};

/* the emitter's snapshot of the summed counters */
static struct {
  uint32_t n[PFK_N];
  uint64_t tsc[PFK_N], iot[PFK_N];
  uint32_t ion[PFK_N];
  uint32_t hist[PF_HB], histf[PF_HB];
  uint32_t kmode_n, rep;
  uint64_t lw_sum, lh_sum;
  uint32_t gl_n;
  uint64_t glw_sum, glh_sum;
  uint32_t z_n, zf_n;
  uint64_t z_tsc, zf_tsc;
  uint32_t sh_n, sh_tg;
  uint64_t sh_tsc;
  uint32_t tr_n[PFT_N];
  uint64_t tr_tsc[PFT_N];
  uint32_t ss_n[PFS_N], ss_tg[PFS_N], ss_run[PFS_N], ss_slow[PFS_N];
  uint64_t ss_tsc[PFS_N];
  uint32_t slow_cpu[MAX_CPUS];
  uint32_t slow_infault;
  uint32_t pvflush;
  uint32_t tg_n[PFR_N];
  uint64_t tg_tsc[PFR_N];
} pf_prev, pf_sum;
static uint64_t pf_thr_prev[MAX_PROCESSES], pf_thr_prev_irq[MAX_PROCESSES];

static void kl_u(struct kline *k, uint64_t v) { kl_i(k, (long)v); }
static void kl_us(struct kline *k, uint64_t tsc) { kl_i(k, (long)pfstat_tsc_to_us(tsc)); }

static void pf_emit(uint64_t now_ms) {
  /* sum the per-CPU records (racy reads of live counters: a diagnostic) */
  __builtin_memset(&pf_sum, 0, sizeof pf_sum);
  uint64_t mx[PFK_N] = {0}, lw_mx = 0, lh_mx = 0, glw_mx = 0, glh_mx = 0, z_mx = 0, sh_mx = 0;
  uint64_t tr_mx[PFT_N] = {0};
  uint64_t ss_mx[PFS_N] = {0};
  uint64_t tg_mx[PFR_N] = {0};
  int glw_site = 0, glh_site = 0;
  for (int ci = 0; ci < MAX_CPUS; ci++) {
    struct pf_cpu *c = &pf_pc[ci];
    for (int k = 0; k < PFK_N; k++) {
      pf_sum.n[k] += c->n[k];
      pf_sum.tsc[k] += c->tsc[k];
      pf_sum.iot[k] += c->iot[k];
      pf_sum.ion[k] += c->ion[k];
      if (c->mx[k] > mx[k]) mx[k] = c->mx[k];
      c->mx[k] = 0;
    }
    for (int b = 0; b < PF_HB; b++) {
      pf_sum.hist[b] += c->hist[b];
      pf_sum.histf[b] += c->histf[b];
    }
    pf_sum.kmode_n += c->kmode_n;
    pf_sum.rep += c->rep;
    pf_sum.lw_sum += c->lw_sum;
    pf_sum.lh_sum += c->lh_sum;
    pf_sum.gl_n += c->gl_n;
    pf_sum.glw_sum += c->glw_sum;
    pf_sum.glh_sum += c->glh_sum;
    pf_sum.z_n += c->z_n;
    pf_sum.zf_n += c->zf_n;
    pf_sum.z_tsc += c->z_tsc;
    pf_sum.zf_tsc += c->zf_tsc;
    pf_sum.sh_n += c->sh_n;
    pf_sum.sh_tg += c->sh_tg;
    pf_sum.sh_tsc += c->sh_tsc;
    for (int t = 0; t < PFS_N; t++) {
      pf_sum.ss_n[t] += c->ss_n[t];
      pf_sum.ss_tg[t] += c->ss_tg[t];
      pf_sum.ss_run[t] += c->ss_run[t];
      pf_sum.ss_slow[t] += c->ss_slow[t];
      pf_sum.ss_tsc[t] += c->ss_tsc[t];
      if (c->ss_mx[t] > ss_mx[t]) ss_mx[t] = c->ss_mx[t];
      c->ss_mx[t] = 0;
    }
    for (int k = 0; k < MAX_CPUS; k++) pf_sum.slow_cpu[k] += c->slow_cpu[k];
    pf_sum.slow_infault += c->slow_infault;
    pf_sum.pvflush += c->pvflush;
    for (int t = 0; t < PFR_N; t++) {
      pf_sum.tg_n[t] += c->tg_n[t];
      pf_sum.tg_tsc[t] += c->tg_tsc[t];
      if (c->tg_mx[t] > tg_mx[t]) tg_mx[t] = c->tg_mx[t];
      c->tg_mx[t] = 0;
    }
    for (int t = 0; t < PFT_N; t++) {
      pf_sum.tr_n[t] += c->tr_n[t];
      pf_sum.tr_tsc[t] += c->tr_tsc[t];
      if (c->tr_mx[t] > tr_mx[t]) tr_mx[t] = c->tr_mx[t];
      c->tr_mx[t] = 0;
    }
    if (c->lw_mx > lw_mx) lw_mx = c->lw_mx;
    if (c->lh_mx > lh_mx) lh_mx = c->lh_mx;
    if (c->glw_mx > glw_mx) { glw_mx = c->glw_mx; glw_site = c->glw_site; }
    if (c->glh_mx > glh_mx) { glh_mx = c->glh_mx; glh_site = c->glh_site; }
    if (c->z_mx > z_mx) z_mx = c->z_mx;
    if (c->sh_mx > sh_mx) sh_mx = c->sh_mx;
    c->lw_mx = c->lh_mx = c->glw_mx = c->glh_mx = c->z_mx = c->sh_mx = 0;
  }
#define D(f) (pf_sum.f - pf_prev.f)
  uint32_t nall = 0;
  uint64_t tall = 0;
  for (int k = 0; k < PFK_N; k++) {
    nall += D(n[k]);
    tall += D(tsc[k]);
  }
  uint64_t dt_ms = pf_last_emit_ms ? now_ms - pf_last_emit_ms : 0;
  struct kline kl;

  kl_init(&kl);
  kl_s(&kl, "[PFK] t=");
  kl_u(&kl, now_ms);
  kl_s(&kl, " dt=");
  kl_u(&kl, dt_ms);
  kl_s(&kl, " n=");
  kl_u(&kl, nall);
  kl_s(&kl, " us=");
  kl_us(&kl, tall);
  kl_s(&kl, " kmode=");
  kl_u(&kl, D(kmode_n));
  kl_s(&kl, " rep=");
  kl_u(&kl, D(rep));
  for (int k = 0; k < PFK_N; k++) {
    uint32_t n = D(n[k]);
    if (!n) continue;
    kl_c(&kl, ' ');
    kl_s(&kl, pf_kname[k]);
    kl_c(&kl, '=');
    kl_u(&kl, n);
    kl_c(&kl, '/');
    kl_us(&kl, D(tsc[k]));
    kl_c(&kl, '/');
    kl_us(&kl, mx[k]);
    if (D(iot[k]) || D(ion[k])) {
      kl_s(&kl, "/io=");
      kl_us(&kl, D(iot[k]));
      kl_s(&kl, "/rpc=");
      kl_u(&kl, D(ion[k]));
    }
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);

  kl_init(&kl);
  kl_s(&kl, "[PFH] t=");
  kl_u(&kl, now_ms);
  kl_s(&kl, " all=");
  for (int b = 0; b < PF_HB; b++) {
    if (b) kl_c(&kl, ',');
    kl_u(&kl, D(hist[b]));
  }
  kl_s(&kl, " file=");
  for (int b = 0; b < PF_HB; b++) {
    if (b) kl_c(&kl, ',');
    kl_u(&kl, D(histf[b]));
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);

  kl_init(&kl);
  kl_s(&kl, "[PFL] t=");
  kl_u(&kl, now_ms);
  kl_s(&kl, " lw=");
  kl_us(&kl, D(lw_sum));
  kl_c(&kl, '/');
  kl_us(&kl, lw_mx);
  kl_s(&kl, " lh=");
  kl_us(&kl, D(lh_sum));
  kl_c(&kl, '/');
  kl_us(&kl, lh_mx);
  kl_s(&kl, " vml=");
  kl_u(&kl, D(gl_n));
  kl_s(&kl, "/w=");
  kl_us(&kl, D(glw_sum));
  kl_c(&kl, '/');
  kl_us(&kl, glw_mx);
  kl_c(&kl, '@');
  kl_i(&kl, glw_site);
  kl_s(&kl, "/h=");
  kl_us(&kl, D(glh_sum));
  kl_c(&kl, '/');
  kl_us(&kl, glh_mx);
  kl_c(&kl, '@');
  kl_i(&kl, glh_site);
  kl_s(&kl, " zero=");
  kl_u(&kl, D(z_n));
  kl_c(&kl, '/');
  kl_us(&kl, D(z_tsc));
  kl_c(&kl, '/');
  kl_us(&kl, z_mx);
  kl_s(&kl, " fresh=");
  kl_u(&kl, D(zf_n));
  kl_c(&kl, '/');
  kl_us(&kl, D(zf_tsc));
  kl_s(&kl, " shoot=");
  kl_u(&kl, D(sh_n));
  kl_c(&kl, '/');
  kl_us(&kl, D(sh_tsc));
  kl_c(&kl, '/');
  kl_us(&kl, sh_mx);
  kl_s(&kl, "/cpus=");
  kl_u(&kl, D(sh_tg));
  {
    /* frame.c's recently-freed LIFO (H7-fault): allocations it served */
    extern uint32_t frame_warm_hit_count(void);
    static uint32_t warm_prev;
    uint32_t wh = frame_warm_hit_count();
    kl_s(&kl, " warm=");
    kl_u(&kl, wh - warm_prev);
    warm_prev = wh;
  }
  {
    /* vm.c's vmzero pool (round 2): frames the stash took from it, frames
       it had none for, frames the task zeroed (per interval) */
    extern volatile uint32_t vm_zp_hits, vm_zp_misses, vm_zp_made;
    static uint32_t h0, m0, z0;
    uint32_t h = vm_zp_hits, m = vm_zp_misses, z = vm_zp_made;
    kl_s(&kl, " zp=");
    kl_u(&kl, h - h0);
    kl_c(&kl, '/');
    kl_u(&kl, m - m0);
    kl_c(&kl, '/');
    kl_u(&kl, z - z0);
    h0 = h;
    m0 = m;
    z0 = z;
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);

  static const char *const tn[PFT_N] = {"pf", "tmr", "ipi", "oth"};
  kl_init(&kl);
  kl_s(&kl, "[PFI] t=");
  kl_u(&kl, now_ms);
  for (int t = 0; t < PFT_N; t++) {
    kl_c(&kl, ' ');
    kl_s(&kl, tn[t]);
    kl_c(&kl, '=');
    kl_u(&kl, D(tr_n[t]));
    kl_c(&kl, '/');
    kl_us(&kl, D(tr_tsc[t]));
    kl_c(&kl, '/');
    kl_us(&kl, tr_mx[t]);
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);

  /* [PFS] shootdowns by site: n/us/max_us/targets/running/slow, then the
     target CPUs that acknowledged >= 1 ms late */
  {
    static const char *const sn[PFS_N] = {"other", "munmap", "jit", "mmap", "mprot", "zap",
                                          "fixed", "cow", "cowshare", "retry", "fork", "trunc"};
    int any = 0;
    for (int t = 0; t < PFS_N; t++) any |= D(ss_n[t]) != 0;
    if (any) {
      kl_init(&kl);
      kl_s(&kl, "[PFS] t=");
      kl_u(&kl, now_ms);
      for (int t = 0; t < PFS_N; t++) {
        if (!D(ss_n[t])) continue;
        kl_c(&kl, ' ');
        kl_s(&kl, sn[t]);
        kl_c(&kl, '=');
        kl_u(&kl, D(ss_n[t]));
        kl_c(&kl, '/');
        kl_us(&kl, D(ss_tsc[t]));
        kl_c(&kl, '/');
        kl_us(&kl, ss_mx[t]);
        kl_c(&kl, '/');
        kl_u(&kl, D(ss_tg[t]));
        kl_c(&kl, '/');
        kl_u(&kl, D(ss_run[t]));
        kl_c(&kl, '/');
        kl_u(&kl, D(ss_slow[t]));
      }
      kl_s(&kl, " pv=");
      kl_u(&kl, D(pvflush));
      kl_s(&kl, " slow_infault=");
      kl_u(&kl, D(slow_infault));
      kl_s(&kl, " slowcpu=");
      for (int k = 0; k < MAX_CPUS; k++) {
        if (k) kl_c(&kl, ',');
        kl_u(&kl, D(slow_cpu[k]));
      }
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  /* [PFR] timer-interrupt segments (every trap, user or kernel mode):
     seg=n/us/max_us */
  {
    static const char *const gn[PFR_N] = {"hrt_flush", "hrt_fire", "hrt_ltimer", "hrt_sched",
                                          "t32_cons", "t32_sample", "t32_rest", "t32_census"};
    kl_init(&kl);
    kl_s(&kl, "[PFR] t=");
    kl_u(&kl, now_ms);
    for (int t = 0; t < PFR_N; t++) {
      if (!D(tg_n[t])) continue;
      kl_c(&kl, ' ');
      kl_s(&kl, gn[t]);
      kl_c(&kl, '=');
      kl_u(&kl, D(tg_n[t]));
      kl_c(&kl, '/');
      kl_us(&kl, D(tg_tsc[t]));
      kl_c(&kl, '/');
      kl_us(&kl, tg_mx[t]);
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
#undef D
  pf_prev = pf_sum;

  /* the four threads with the most #PF + interrupt time this interval */
  int top[4] = {0, 0, 0, 0};
  uint64_t topv[4] = {0, 0, 0, 0}, topp[4] = {0, 0, 0, 0};
  for (int s = 1; s < MAX_PROCESSES; s++) {
    uint64_t cp = pf_thr_pf_t[s], ci = pf_thr_irq_t[s];
    uint64_t dp = cp >= pf_thr_prev[s] ? cp - pf_thr_prev[s] : cp;
    uint64_t di = ci >= pf_thr_prev_irq[s] ? ci - pf_thr_prev_irq[s] : ci;
    pf_thr_prev[s] = cp;
    pf_thr_prev_irq[s] = ci;
    uint64_t d = dp + di;
    for (int j = 0; j < 4; j++) {
      if (d <= topv[j]) continue;
      for (int m = 3; m > j; m--) {
        top[m] = top[m - 1];
        topv[m] = topv[m - 1];
        topp[m] = topp[m - 1];
      }
      top[j] = s;
      topv[j] = d;
      topp[j] = dp;
      break;
    }
  }
  if (topv[0]) {
    kl_init(&kl);
    kl_s(&kl, "[PFT] t=");
    kl_u(&kl, now_ms);
    kl_s(&kl, " top=");
    for (int j = 0; j < 4 && topv[j]; j++) {
      struct process *p = process_get_pcb(top[j]);
      if (j) kl_c(&kl, ',');
      kl_i(&kl, top[j]);
      kl_c(&kl, ':');
      kl_us(&kl, topp[j]);
      kl_c(&kl, ':');
      kl_us(&kl, topv[j] - topp[j]);
      kl_c(&kl, ':');
      if (p) {
        int nn = 0;
        while (p->name[nn] && nn < 15) { kl_c(&kl, p->name[nn]); nn++; }
      }
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}

static void pf_maybe_emit(int force) {
  if (pf_sets >= PF_SETS) return;
  uint64_t now = timer_get_ms();
  if (!force && now - pf_last_emit_ms < PF_EMIT_MS) return;
  if (__sync_lock_test_and_set(&pf_emitting, 1)) return;
  if (force || now - pf_last_emit_ms >= PF_EMIT_MS) {
    pf_sets++;
    pf_emit(now);
    pf_last_emit_ms = now;
  }
  __sync_lock_release(&pf_emitting);
}

void pfstat_emit_now(void) { pf_maybe_emit(1); }

void pfstat_trap(uint64_t vector, uint64_t tsc) {
  int cls = vector == 14 ? PFT_PF
            : (vector == 32 || vector == 0x84) ? PFT_TMR
            : (vector == 0x81 || vector == 0x82) ? PFT_IPI : PFT_OTH;
  struct pf_cpu *c = pf_me();
  c->tr_n[cls]++;
  c->tr_tsc[cls] += tsc;
  if (tsc > c->tr_mx[cls]) c->tr_mx[cls] = tsc;
  int s = pf_slot();
  if (s) {
    if (cls == PFT_PF) pf_thr_pf_t[s] += tsc;
    else pf_thr_irq_t[s] += tsc;
  }
  if (cls != PFT_PF && tsc >= pf_long_tsc()) pf_long_row('I', (int)vector, tsc, 0);
  /* a row set only when there were faults to report */
  if (cls == PFT_PF) pf_maybe_emit(0);
}

#endif /* __x86_64__ */
