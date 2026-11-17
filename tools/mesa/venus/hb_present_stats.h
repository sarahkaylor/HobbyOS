/*
 * hb_present_stats.h -- the HobbyOS present-cost instrument (Cyberpunk P7,
 * lane C6-p7; docs/cp-p7.md §3).  Copied into Mesa's src/vulkan/wsi/ by
 * tools/mesa/venus/build.sh and included by wsi_common.c (which defines the
 * one global: HB_PSTATS_DEFINE) and wsi_common_x11.c.
 *
 * The RimWorld [HBT] idea (docs/l88-fps-report.md) for Vulkan: every present
 * is split into the stages that can cost time, measured with the TSC (the
 * guest's CLOCK_MONOTONIC ticks at 1 ms), and one row per window
 * (HB_PRESENT_STATS_MS, default 2000; 0 = off) goes to stderr:
 *
 *   [HBP] pid=P n=N fps=F path=scanout|putimage|mixed
 *         acq=avg/max sub=avg/max wait=avg/max flip=avg/max geom=avg/max
 *         set=avg flush=avg (kernel round trips of a scanout flip) us
 *         scanout=S putimage=U refused=R
 *
 *   acq    vkAcquireNextImageKHR (wsi_common_acquire_next_image2), app thread
 *   sub    vkQueuePresentKHR minus `wait`: the blit / semaphore submit and
 *          the hand-off to the WSI queue thread
 *   wait   the software WSI's CPU wait for the frame's fence (the GPU
 *          finishing the frame + its blit), inside vkQueuePresentKHR
 *   flip   the WSI queue thread's present: GPU_IOC_SCANOUT_BLOB, or
 *          xcb_put_image of the whole image
 *   geom   the xcb_get_geometry round trip after it (OUT_OF_DATE check)
 *   set/flush  what the kernel measured inside the scanout ioctl
 *
 * Every number is a per-window average and maximum in microseconds.  Cost
 * of the instrument: a few rdtsc and atomic adds per present and one write(2)
 * per window.
 */
#ifndef HB_PRESENT_STATS_H
#define HB_PRESENT_STATS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "util/os_time.h"

enum {
   HB_PS_ACQ,
   HB_PS_SUB,
   HB_PS_WAIT,
   HB_PS_FLIP,
   HB_PS_GEOM,
   HB_PS_SET,
   HB_PS_FLUSH,
   HB_PS_N
};

struct hb_pstats {
   int state;                  /* 0 unread env, 1 on, -1 off */
   uint64_t window_ms;
   uint64_t tsc0, ns0;         /* calibration origin */
   uint64_t win_tsc, win_ns;   /* current window start */
   uint64_t sum[HB_PS_N], max[HB_PS_N], cnt[HB_PS_N];  /* TSC cycles */
   uint64_t presents, scanout, putimage, refused;
   uint64_t total_presents;
};

#ifdef HB_PSTATS_DEFINE
struct hb_pstats hb_pstats;
#else
extern struct hb_pstats hb_pstats;
#endif

static inline uint64_t
hb_tsc(void)
{
#if defined(__x86_64__)
   uint32_t lo, hi;
   __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
   return ((uint64_t)hi << 32) | lo;
#else
   return (uint64_t)os_time_get_nano();
#endif
}

static inline int
hb_pstats_on(void)
{
   struct hb_pstats *s = &hb_pstats;
   int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
   if (st)
      return st > 0;
   const char *e = getenv("HB_PRESENT_STATS_MS");
   long ms = e ? strtol(e, NULL, 10) : 2000;
   s->window_ms = ms > 0 ? (uint64_t)ms : 0;
   s->tsc0 = s->win_tsc = hb_tsc();
   s->ns0 = s->win_ns = (uint64_t)os_time_get_nano();
   __atomic_store_n(&s->state, ms > 0 ? 1 : -1, __ATOMIC_RELEASE);
   return ms > 0;
}

/* One stage sample, in TSC cycles. */
static inline void
hb_pstats_add(int k, uint64_t cyc)
{
   struct hb_pstats *s = &hb_pstats;
   if (!hb_pstats_on())
      return;
   __atomic_fetch_add(&s->sum[k], cyc, __ATOMIC_RELAXED);
   __atomic_fetch_add(&s->cnt[k], 1, __ATOMIC_RELAXED);
   uint64_t m = __atomic_load_n(&s->max[k], __ATOMIC_RELAXED);
   while (cyc > m && !__atomic_compare_exchange_n(&s->max[k], &m, cyc, 1, __ATOMIC_RELAXED,
                                                  __ATOMIC_RELAXED))
      ;
}

/* A kernel-measured stage, in microseconds (converted with the current
 * calibration; before the first window it is stored as cycles at 1 GHz and
 * the first row is approximate). */
static inline void
hb_pstats_add_us(int k, uint32_t us)
{
   struct hb_pstats *s = &hb_pstats;
   if (!hb_pstats_on())
      return;
   uint64_t dt = hb_tsc() - s->tsc0, dn = (uint64_t)os_time_get_nano() - s->ns0;
   double cpn = dn > 100000000ull ? (double)dt / (double)dn : 1.0;
   hb_pstats_add(k, (uint64_t)((double)us * 1000.0 * cpn));
}

/* After each flip (WSI queue thread): count it and, once per window, print
 * the row and start a new window.  path: 1 scanout, 2 putimage; refused:
 * the kernel said not now (fell back to putimage). */
static inline void
hb_pstats_present(int path, int refused)
{
   struct hb_pstats *s = &hb_pstats;
   if (!hb_pstats_on())
      return;
   __atomic_fetch_add(&s->presents, 1, __ATOMIC_RELAXED);
   __atomic_fetch_add(path == 1 ? &s->scanout : &s->putimage, 1, __ATOMIC_RELAXED);
   if (refused)
      __atomic_fetch_add(&s->refused, 1, __ATOMIC_RELAXED);
   uint64_t now_ns = (uint64_t)os_time_get_nano();
   if (now_ns - s->win_ns < s->window_ms * 1000000ull)
      return;
   uint64_t now_tsc = hb_tsc();
   double cpn = (double)(now_tsc - s->tsc0) / (double)(now_ns - s->ns0 ? now_ns - s->ns0 : 1);
   double win_s = (double)(now_tsc - s->win_tsc) / cpn / 1e9;
   uint64_t n = __atomic_exchange_n(&s->presents, 0, __ATOMIC_RELAXED);
   uint64_t so = __atomic_exchange_n(&s->scanout, 0, __ATOMIC_RELAXED);
   uint64_t pu = __atomic_exchange_n(&s->putimage, 0, __ATOMIC_RELAXED);
   uint64_t rf = __atomic_exchange_n(&s->refused, 0, __ATOMIC_RELAXED);
   double avg[HB_PS_N], mx[HB_PS_N];
   for (int k = 0; k < HB_PS_N; k++) {
      uint64_t c = __atomic_exchange_n(&s->cnt[k], 0, __ATOMIC_RELAXED);
      uint64_t su = __atomic_exchange_n(&s->sum[k], 0, __ATOMIC_RELAXED);
      uint64_t m = __atomic_exchange_n(&s->max[k], 0, __ATOMIC_RELAXED);
      avg[k] = c ? (double)su / (double)c / cpn / 1000.0 : 0.0;
      mx[k] = (double)m / cpn / 1000.0;
   }
   s->total_presents += n;
   s->win_tsc = now_tsc;
   s->win_ns = now_ns;
   char row[512];
   int len = snprintf(row, sizeof row,
                      "[HBP] pid=%d n=%llu fps=%.1f path=%s acq=%.0f/%.0f sub=%.0f/%.0f "
                      "wait=%.0f/%.0f flip=%.0f/%.0f geom=%.0f/%.0f set=%.0f flush=%.0f us "
                      "scanout=%llu putimage=%llu refused=%llu total=%llu\n",
                      (int)getpid(), (unsigned long long)n, win_s > 0 ? n / win_s : 0.0,
                      so && pu ? "mixed" : so ? "scanout" : "putimage", avg[HB_PS_ACQ],
                      mx[HB_PS_ACQ], avg[HB_PS_SUB], mx[HB_PS_SUB], avg[HB_PS_WAIT],
                      mx[HB_PS_WAIT], avg[HB_PS_FLIP], mx[HB_PS_FLIP], avg[HB_PS_GEOM],
                      mx[HB_PS_GEOM], avg[HB_PS_SET], avg[HB_PS_FLUSH], (unsigned long long)so,
                      (unsigned long long)pu, (unsigned long long)rf,
                      (unsigned long long)s->total_presents);
   if (len > 0)
      (void)!write(2, row, (size_t)(len < (int)sizeof row ? len : (int)sizeof row - 1));
}

#endif /* HB_PRESENT_STATS_H */
