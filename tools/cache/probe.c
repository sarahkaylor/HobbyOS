/* probe.c — standalone GB-006 measurement probe: a self-contained translation
 * unit that compiles with the host cc (and the HobbyOS cross clang), used by
 * cold_vs_cached.sh to demonstrate a real cold-vs-cached compile timing. */
#include <stdint.h>

static const uint32_t g_probe_rows = 4096;
static uint32_t g_probe[4096];

uint32_t probe_fill(void) {
  uint32_t acc = 0;
  for (uint32_t i = 0; i < g_probe_rows; i++) {
    g_probe[i] = (i * 2654435761u) ^ (i >> 13);
    acc ^= g_probe[i];
  }
  return acc;
}
