/* gfx_stats.h — pure burst-per-flush stats for GFXBENCH (GB-045).
 *
 * gfxbench measures framebuffer present cost by timing a BURST of flush
 * calls end-to-end against the guest's millisecond clock and dividing by the
 * burst count, so a sub-ms per-flush cost is resolvable.  This module holds
 * the pure arithmetic (median/min/mean + burst division) so it can be locked
 * by a host golden test without any guest dependency.
 */
#ifndef GFX_STATS_H
#define GFX_STATS_H

#include <stdint.h>

struct gfx_stats {
  uint64_t min;   /* fastest per-flush sample (intrinsic-cost estimator) */
  uint64_t med;   /* median per-flush sample */
  uint64_t mean;  /* mean per-flush sample */
};

/* Stats over `n` per-flush sample values.  `s` is sorted in place. */
void gfx_stats_of(uint64_t *s, int n, struct gfx_stats *out);

/* Per-flush microseconds for a burst: total burst time / burst count. */
uint64_t gfx_per_flush(uint64_t burst_us, int burst);

/* Decimal-format `v` into `out` (NUL-terminated, at most `max` incl. NUL).
 * GFXBENCH prints its numbers through print_console (serial) so they are
 * capturable when the app runs as a desktop window (print_dec -> stdout
 * would vanish into the window pipe). */
void gfx_u64_to_str(uint64_t v, char *out, int max);

#endif
