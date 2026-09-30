/*
 * zlib 1.3.1 host smoke test (L6 lane browser/l6-libs1).
 *
 * Deflate/inflate roundtrip of a deterministic 64 KiB buffer: fill pattern
 * (structured + LCG pseudo-random bytes), compress2(level 9) -> uncompress ->
 * byte-exact compare.  Prints size/check-value fingerprints so repeated runs
 * are comparable (checksum-stability gate).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zlib.h>

#define RAW_SIZE (64 * 1024)

static unsigned char raw[RAW_SIZE];
static unsigned char comp[RAW_SIZE + 1024];
static unsigned char back[RAW_SIZE];

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

int main(void)
{
  uint32_t s = 0x12345678u;
  size_t i;
  uLongf comp_len = sizeof comp;
  uLongf back_len = sizeof back;
  int rc;

  for (i = 0; i < RAW_SIZE; i++) {
    s = s * 1664525u + 1013904223u; /* LCG */
    /* Every 251st byte is structured text so both LZ matches and literal
     * paths are exercised; the rest is pseudo-random (incompressible). */
    raw[i] = (unsigned char)((i % 251 == 0) ? (unsigned int)('A' + (int)(i % 26))
                                             : (s >> 24));
  }

  rc = compress2(comp, &comp_len, raw, (uLong)RAW_SIZE, 9);
  if (rc != Z_OK) {
    printf("zlib_smoke: FAIL compress2 rc=%d\n", rc);
    return 1;
  }
  rc = uncompress(back, &back_len, comp, comp_len);
  if (rc != Z_OK || back_len != (uLongf)RAW_SIZE ||
      memcmp(raw, back, RAW_SIZE) != 0) {
    printf("zlib_smoke: FAIL uncompress rc=%d len=%lu\n", rc,
           (unsigned long)back_len);
    return 1;
  }

  printf("zlib_smoke: OK version=%s raw=%d comp=%lu roundtrip=exact "
         "crc32=%08lx adler32=%08lx fnv=%016llx\n",
         ZLIB_VERSION, RAW_SIZE, (unsigned long)comp_len,
         (unsigned long)crc32(crc32(0L, Z_NULL, 0), raw, (uInt)RAW_SIZE),
         (unsigned long)adler32(adler32(0L, Z_NULL, 0), raw, (uInt)RAW_SIZE),
         (unsigned long long)fnv1a(raw, RAW_SIZE));
  return 0;
}
