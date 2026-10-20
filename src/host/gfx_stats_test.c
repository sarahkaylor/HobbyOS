/* gfx_stats_test.c — T1 host golden for GFXBENCH burst-per-flush stats
 * (GB-045).  Locks gfx_stats_of()'s min/median/mean on known samples and
 * gfx_per_flush()'s burst division, so the numbers GFXBENCH prints are the
 * arithmetic the report claims, independent of the guest clock.
 *
 * The regression this guards: gfxbench previously timed a SINGLE sub-ms
 * flush against the guest's millisecond clock, so samples read 0us or 1000us
 * at the mercy of the tick boundary (`min=0 med=1000 mean=800` — GB-045-V
 * RED).  The fix measures a burst and divides; this test locks the division
 * and the summary stats.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../user_include/libc.h"
#include "../user/gfx_stats.h"

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
  checks++;
  if (cond) printf("  PASS: %s\n", msg);
  else { printf("  FAIL: %s\n", msg); fails++; }
}

int main(void) {
  printf("[TEST] gfxbench burst-per-flush stats (GB-045)\n");

  /* 1) gfx_per_flush: burst division is per-flush cost. */
  check(gfx_per_flush(32000, 32) == 1000, "burst 32000us/32 -> 1000us per flush");
  check(gfx_per_flush(1000, 64) == 15, "burst 1000us/64 -> 15us per flush (floor div)");
  check(gfx_per_flush(0, 32) == 0, "zero-duration burst -> 0 per flush");

  /* 2) gfx_stats_of: min/med/mean over a known (unsorted) sample set. */
  {
    uint64_t s[] = { 800, 1200, 900, 1000, 1100 };       /* mean=1000, med(2)=1000, min=800 */
    struct gfx_stats st;
    gfx_stats_of(s, 5, &st);
    check(st.min == 800, "min over {800,1200,900,1000,1100} == 800");
    check(st.med == 1000, "median (odd n=5) == 1000");
    check(st.mean == 1000, "mean (odd n=5) == 1000");
  }

  /* 3) Even count: median is the upper-middle element (s[n/2]). */
  {
    uint64_t s[] = { 40, 20, 30, 10 };                   /* sorted 10,20,30,40 */
    struct gfx_stats st;
    gfx_stats_of(s, 4, &st);
    check(st.min == 10, "min over {40,20,30,10} == 10");
    check(st.med == s[4 / 2] && s[2] == 30, "even n: median = s[n/2] = 30");
    check(st.mean == 25, "mean (even n=4) == 25");
  }

  /* 4) Mixed-burst realism: mostly ~1000us flushes with a couple of fast
   *    outliers is a measurable (not junk 0/1000-only) distribution once
   *    burst-averaged. */
  {
    uint64_t s[10];
    int i;
    for (i = 0; i < 8; i++) s[i] = 980;
    s[8] = 1010; s[9] = 990;
    struct gfx_stats st;
    gfx_stats_of(s, 10, &st);
    check(st.min == 980 && st.med == 980 && st.mean == 984,
          "burst-averaged spread: min=980 med=980 mean=984 (no 0us/1000us junk)");
  }

  /* 5) gfx_u64_to_str: numbers reach serial as decimal (not swallowed by
   *    the window's stdout when GFXBENCH runs as a desktop app). */
  {
    char o[24];
    gfx_u64_to_str(0, o, sizeof o);    check(strcmp(o, "0") == 0, "0 -> \"0\"");
    gfx_u64_to_str(1000, o, sizeof o); check(strcmp(o, "1000") == 0, "1000 -> \"1000\"");
    gfx_u64_to_str(984, o, sizeof o);  check(strcmp(o, "984") == 0, "984 -> \"984\"");
    gfx_u64_to_str(18446744073709551615ULL, o, sizeof o);  /* UINT64_MAX */
    check(strcmp(o, "18446744073709551615") == 0,
          "UINT64_MAX renders fully (us precision never truncated)");
  }

  printf("[TEST] gfx_stats: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
