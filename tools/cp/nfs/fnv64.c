/* tools/cp/nfs/fnv64.c — host helper for nfs_bench.sh: write N bytes of a
 * deterministic xorshift stream to a file and print their FNV-1a-64 (the same
 * hash src/user/cpnfsbench.c computes in the guest).  An optional third
 * argument seeds the stream (cp/nfs2: the multi-file phase; default is the
 * original seed). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) { fprintf(stderr, "usage: fnv64 <out> <bytes> [seed]\n"); return 2; }
  FILE *f = fopen(argv[1], "wb");
  if (!f) return 1;
  uint64_t n = strtoull(argv[2], 0, 0), x = 0x9e3779b97f4a7c15ULL;
  if (argc == 4) x = strtoull(argv[3], 0, 0);
  if (!x) return 2;
  uint64_t h = 0xcbf29ce484222325ULL;
  static uint8_t b[1 << 16];
  for (uint64_t done = 0; done < n;) {
    size_t k = n - done < sizeof b ? (size_t)(n - done) : sizeof b;
    for (size_t i = 0; i < k; i++) {
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;
      b[i] = (uint8_t)(x >> 29);
      h = (h ^ b[i]) * 0x100000001b3ULL;
    }
    if (fwrite(b, 1, k, f) != k) return 1;
    done += k;
  }
  fclose(f);
  printf("%016llx\n", (unsigned long long)h);
  return 0;
}
