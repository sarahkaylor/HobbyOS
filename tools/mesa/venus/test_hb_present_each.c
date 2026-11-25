/*
 * tools/mesa/venus/test_hb_present_each.c -- driver for the per-present
 * counter (HB_PRESENT_EACH=1, docs/nv-n9v-tail.md §7).
 *
 * The instrument is a header copied into Mesa's WSI, so it cannot be built
 * without the Mesa tree.  This driver includes the real header against two
 * stubs (util/os_time.h, hb_env_defaults.h) and plays the two threads that
 * use it: the app thread's vkQueuePresentKHR (hb_pp_handoff) and the WSI
 * queue thread's present (hb_pstats_add(HB_PS_FLIP) + hb_pstats_present).
 * The assertions live in test_hb_present_each.py, which compiles and runs it.
 *
 *   test_hb_present_each clean [N]   N presents, each with its hand-off
 *   test_hb_present_each nosub       one present with no hand-off
 *   test_hb_present_each drop [N]    N hand-offs first, then one present
 *   test_hb_present_each both        EACH + a window row after 150 ms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HB_PSTATS_DEFINE /* wsi_common.c defines it; the single global lives here */
#include "hb_present_stats.h"

#define SUBCYC(i) (100000ull + 1000ull * (uint64_t)(i))
#define WAITCYC(i) (3000ull + 10ull * (uint64_t)(i))
#define FLIPCYC(i) (2500000ull + 50000ull * (uint64_t)(i))

/* One present, in the order the two threads make these calls: the app thread
 * enters vkQueuePresentKHR at t_ns, hands off at its end, and the WSI thread
 * flips later and returns from the flip at `now`.  Entries are 60 Hz apart
 * (16.67 ms), back-dated by this present's own stages, so t increases and dt
 * covers entry -> flip. */
static uint64_t hb_test_base;
static void
present(int i, int handoff, uint64_t *t_back)
{
   const uint64_t cpn = (uint64_t)(hb_pstats_cpn() + 0.5);
   const uint64_t sub = SUBCYC(i) / cpn, wait = WAITCYC(i) / cpn, flip = FLIPCYC(i) / cpn;

   const uint64_t t_ns = hb_test_base + 16666667ull * (uint64_t)i -
                         1000000ull - (sub + wait + flip);
   hb_pstats_add(HB_PS_SUB, SUBCYC(i));
   hb_pstats_add(HB_PS_WAIT, WAITCYC(i));
   if (handoff)
      hb_pp_handoff(t_ns, SUBCYC(i), WAITCYC(i));
   hb_pstats_add(HB_PS_FLIP, FLIPCYC(i));
   hb_pstats_present(1, 0, FLIPCYC(i), (uint32_t)(i % 3));
   if (t_back)
      *t_back = t_ns;
}

int
main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "clean";
   const int n = argc > 2 ? atoi(argv[2]) : 8;

   /* First call resolves the environment and starts the TSC calibration;
    * 20 ms of baseline is more than hb_pstats_cpn()'s 10 ms rule. */
   (void)hb_pstats_on();
   usleep(20000);
   hb_test_base = (uint64_t)os_time_get_nano();

   if (!strcmp(mode, "clean")) {
      for (int i = 0; i < n; i++) {
         usleep(17000); /* let the wall clock pass the entry time stamped below */
         present(i, 1, NULL);
      }
   } else if (!strcmp(mode, "nosub")) {
      present(0, 0, NULL);
   } else if (!strcmp(mode, "drop")) {
      /* 100 hand-offs and no flip: the ring (64) fills and the rest drop. */
      for (int i = 0; i < 100; i++)
         hb_pp_handoff((uint64_t)os_time_get_nano(), SUBCYC(i), WAITCYC(i));
      present(100, 0, NULL);
   } else if (!strcmp(mode, "both")) {
      present(0, 1, NULL);
      usleep(150000); /* > HB_PRESENT_STATS_MS=100: the next present prints [HBP] */
      present(1, 1, NULL);
   } else {
      fprintf(stderr, "test_hb_present_each: unknown mode %s\n", mode);
      return 2;
   }
   fprintf(stderr, "test_hb_present_each: done mode=%s\n", mode);
   return 0;
}
