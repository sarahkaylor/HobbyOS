/*
 * libwebp 1.6.0 host smoke test (L6 lane browser/l6-libs1).
 *
 * Lossless RGBA roundtrip: WebPEncodeLosslessRGBA on a generated 64x64
 * gradient (with alpha), WebPDecodeRGBA back, require byte-exact equality
 * (lossless must be exact) and print the pixel fingerprint so repeated runs
 * are comparable (checksum-stability gate).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/encode.h>

#define W 64
#define H 64

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
  size_t raw_size = (size_t)W * H * 4;
  unsigned char *rgba, *dec;
  uint8_t *enc = NULL;
  size_t enc_len;
  int x, y, w = 0, h = 0;

  rgba = malloc(raw_size);
  if (!rgba) {
    return 2;
  }
  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      size_t o = ((size_t)y * W + x) * 4;
      rgba[o + 0] = (unsigned char)(x * 4);
      rgba[o + 1] = (unsigned char)(y * 4);
      rgba[o + 2] = (unsigned char)((x * 7 + y * 3) & 0xff);
      rgba[o + 3] = (unsigned char)(255 - ((x + y) & 0xff));
    }
  }

  enc_len = WebPEncodeLosslessRGBA(rgba, W, H, W * 4, &enc);
  if (enc_len == 0 || enc == NULL) {
    printf("webp_smoke: FAIL encode\n");
    return 1;
  }
  dec = WebPDecodeRGBA(enc, enc_len, &w, &h);
  if (!dec || w != W || h != H) {
    printf("webp_smoke: FAIL decode %dx%d\n", w, h);
    return 1;
  }
  if (memcmp(rgba, dec, raw_size) != 0) {
    printf("webp_smoke: FAIL lossless roundtrip not byte-exact\n");
    return 1;
  }

  printf("webp_smoke: OK encoder=0x%08x %dx%d rgba8 lossless encoded=%zu "
         "roundtrip=exact fnv=%016llx\n",
         (unsigned)WebPGetEncoderVersion(), W, H, enc_len,
         (unsigned long long)fnv1a(dec, raw_size));

  WebPFree(enc);
  WebPFree(dec);
  free(rgba);
  return 0;
}
