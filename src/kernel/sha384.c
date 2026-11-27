/* ============================================================================
 *  sha384: SHA-384 (FIPS 180-4), byte-oriented, no SIMD, no wide loads.
 *
 *  Why this file exists: the COT that boots GSP-FMC carries a 48-byte
 *  `hash384` and the FSP validates the image against it.  If the image bytes
 *  and that hash disagree, the SP rejects the command and the boot says
 *  nothing about which of the two it disliked.  So the kernel recomputes the
 *  digest from the exact bytes it placed at gspFmcSysmemOffset and prints
 *  whether they match, on the card, in the same boot.
 *
 *  The algorithm is the standard one: 128-byte blocks, big-endian words,
 *  80 rounds, the SHA-384 IV and the top 384 bits of the state.  Verified
 *  against the NIST vectors ("", "abc") and against the digest the GB202 FMC
 *  container itself carries, which is the vector this file is for.
 * ========================================================================== */

#include <stdint.h>

#include "sha384.h"

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR64((x), 28) ^ ROTR64((x), 34) ^ ROTR64((x), 39))
#define BSIG1(x) (ROTR64((x), 14) ^ ROTR64((x), 18) ^ ROTR64((x), 41))
#define SSIG0(x) (ROTR64((x), 1) ^ ROTR64((x), 8) ^ ((x) >> 7))
#define SSIG1(x) (ROTR64((x), 19) ^ ROTR64((x), 61) ^ ((x) >> 6))

static const uint64_t sha384_k[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
  0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
  0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
  0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
  0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
  0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
  0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
  0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
  0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
  0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
  0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
  0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
  0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
  0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
  0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
  0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
  0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
  0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
  0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
  0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
  0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void sha384_block(struct sha384_ctx *c, const uint8_t *p) {
  uint64_t w[80], a, b, cc, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint64_t)p[i * 8] << 56) | ((uint64_t)p[i * 8 + 1] << 48) |
           ((uint64_t)p[i * 8 + 2] << 40) | ((uint64_t)p[i * 8 + 3] << 32) |
           ((uint64_t)p[i * 8 + 4] << 24) | ((uint64_t)p[i * 8 + 5] << 16) |
           ((uint64_t)p[i * 8 + 6] << 8) | (uint64_t)p[i * 8 + 7];
  }
  for (i = 16; i < 80; i++)
    w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

  for (i = 0; i < 80; i++) {
    uint64_t t1 = h + BSIG1(e) + CH(e, f, g) + sha384_k[i] + w[i];
    uint64_t t2 = BSIG0(a) + MAJ(a, b, cc);
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }

  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha384_init(struct sha384_ctx *c) {
  c->h[0] = 0xcbbb9d5dc1059ed8ULL;
  c->h[1] = 0x629a292a367cd507ULL;
  c->h[2] = 0x9159015a3070dd17ULL;
  c->h[3] = 0x152fecd8f70e5939ULL;
  c->h[4] = 0x67332667ffc00b31ULL;
  c->h[5] = 0x8eb44a8768581511ULL;
  c->h[6] = 0xdb0c2e0d64f98fa7ULL;
  c->h[7] = 0x47b5481dbefa4fa4ULL;
  c->len_lo = 0;
  c->len_hi = 0;
  c->used = 0;
}

void sha384_update(struct sha384_ctx *c, const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t add = (uint64_t)len;
  uint64_t lo = c->len_lo + (add << 3);
  if (lo < c->len_lo) c->len_hi++;
  c->len_lo = lo;
  c->len_hi += add >> 61;

  while (len > 0) {
    uint32_t take = 128u - c->used;
    if (take > len) take = len;
    for (uint32_t i = 0; i < take; i++) c->buf[c->used + i] = p[i];
    c->used += take;
    p += take;
    len -= take;
    if (c->used == 128u) {
      sha384_block(c, c->buf);
      c->used = 0;
    }
  }
}

void sha384_final(struct sha384_ctx *c, uint8_t out[48]) {
  uint64_t bits_lo = c->len_lo, bits_hi = c->len_hi;
  uint8_t pad[256];
  uint32_t padlen;
  uint32_t i;

  pad[0] = 0x80;
  for (i = 1; i < sizeof pad; i++) pad[i] = 0;
  /* pad to 112 mod 128, then the 16-byte big-endian bit length */
  padlen = (c->used < 112u) ? (112u - c->used) : (240u - c->used);
  sha384_update(c, pad, padlen);
  {
    uint8_t lenb[16];
    for (i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits_hi >> (56 - 8 * i));
    for (i = 0; i < 8; i++) lenb[8 + i] = (uint8_t)(bits_lo >> (56 - 8 * i));
    sha384_update(c, lenb, 16);
  }
  for (i = 0; i < 6; i++) {
    out[i * 8]     = (uint8_t)(c->h[i] >> 56);
    out[i * 8 + 1] = (uint8_t)(c->h[i] >> 48);
    out[i * 8 + 2] = (uint8_t)(c->h[i] >> 40);
    out[i * 8 + 3] = (uint8_t)(c->h[i] >> 32);
    out[i * 8 + 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 8 + 5] = (uint8_t)(c->h[i] >> 16);
    out[i * 8 + 6] = (uint8_t)(c->h[i] >> 8);
    out[i * 8 + 7] = (uint8_t)(c->h[i]);
  }
}

void sha384_digest(const void *data, uint32_t len, uint8_t out[48]) {
  struct sha384_ctx c;
  sha384_init(&c);
  sha384_update(&c, data, len);
  sha384_final(&c, out);
}
