/* libsha1.c -- see libsha1.h.  Byte-wise loads/stores only (no unaligned
 * word access), so it is safe on any alignment on both arches. */
#include "libsha1.h"
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80], a, b, d, e, cc, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6u; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_begin(sha1_ctx *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->used = 0;
}

void sha1_hash(const unsigned char *data, unsigned long len, sha1_ctx *c)
{
    c->len += len;
    while (len > 0) {
        unsigned int n = 64 - c->used;
        if (n > len) n = (unsigned int)len;
        memcpy(c->buf + c->used, data, n);
        c->used += n; data += n; len -= n;
        if (c->used == 64) { sha1_block(c, c->buf); c->used = 0; }
    }
}

void sha1_end(unsigned char hval[], sha1_ctx *c)
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;
    sha1_hash(&pad, 1, c);
    while (c->used != 56) sha1_hash(&zero, 1, c);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_hash(lenb, 8, c);
    for (i = 0; i < 5; i++) {
        hval[4 * i]     = (unsigned char)(c->h[i] >> 24);
        hval[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        hval[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        hval[4 * i + 3] = (unsigned char)c->h[i];
    }
}

#ifdef LIBSHA1_SELFTEST
#include <stdio.h>
int main(void)
{
    static const char *in[] = { "", "abc",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
    static const char *want[] = { "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "a9993e364706816aba3e25717850c26c9cd0d89d",
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1" };
    int i, j, bad = 0;
    for (i = 0; i < 3; i++) {
        sha1_ctx c; unsigned char d[20]; char hex[41];
        sha1_begin(&c); sha1_hash((const unsigned char *)in[i], strlen(in[i]), &c); sha1_end(d, &c);
        for (j = 0; j < 20; j++) sprintf(hex + 2 * j, "%02x", d[j]);
        if (strcmp(hex, want[i])) { printf("FAIL %d %s\n", i, hex); bad = 1; }
    }
    /* million 'a' */
    { sha1_ctx c; unsigned char d[20], a[1000]; char hex[41];
      memset(a, 'a', sizeof a); sha1_begin(&c);
      for (i = 0; i < 1000; i++) sha1_hash(a, 1000, &c);
      sha1_end(d, &c);
      for (j = 0; j < 20; j++) sprintf(hex + 2 * j, "%02x", d[j]);
      if (strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f")) { printf("FAIL M %s\n", hex); bad = 1; } }
    puts(bad ? "libsha1 selftest FAIL" : "libsha1 selftest ok");
    return bad;
}
#endif
