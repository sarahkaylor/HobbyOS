/* gfx_stats.c — pure burst-per-flush stats for GFXBENCH (GB-045). */
#include "gfx_stats.h"

/* Insertion sort of `a[0..n)` ascending. */
static void sort_u64(uint64_t *a, int n) {
  for (int i = 1; i < n; i++) {
    uint64_t v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = v;
  }
}

void gfx_stats_of(uint64_t *s, int n, struct gfx_stats *out) {
  uint64_t sum = 0;
  int i;
  for (i = 0; i < n; i++) sum += s[i];
  sort_u64(s, n);
  out->min = s[0];
  out->med = s[n / 2];
  out->mean = sum / (uint64_t)n;
}

uint64_t gfx_per_flush(uint64_t burst_us, int burst) {
  return burst_us / (uint64_t)burst;
}

void gfx_u64_to_str(uint64_t v, char *out, int max) {
  int i = 0;
  char tmp[24];
  do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < max - 1);
  int j = 0;
  while (i > 0) out[j++] = tmp[--i];
  out[j] = '\0';
}
