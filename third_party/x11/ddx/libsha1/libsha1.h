/*
 * libsha1.h -- minimal SHA-1 with the "libsha1" API the X server's
 * os/xsha1.c (HAVE_SHA1_IN_LIBSHA1, meson -Dsha1=libsha1) expects:
 * sha1_ctx, sha1_begin(), sha1_hash(), sha1_end().  HobbyOS has no
 * libmd/nettle/gcrypt/libcrypto; the server only uses SHA-1 to hash
 * Render glyph bits, so a small portable implementation is enough.
 * Written for HobbyOS (third_party/x11/ddx/libsha1), FIPS 180-4.
 */
#ifndef HOBBYOS_LIBSHA1_H
#define HOBBYOS_LIBSHA1_H
#include <stdint.h>
#include <stddef.h>

#define SHA1_DIGEST_SIZE 20
#define SHA1_BLOCK_SIZE 64

typedef struct {
    uint32_t h[5];
    uint64_t len;            /* total bytes hashed */
    unsigned char buf[64];
    unsigned int used;       /* bytes in buf */
} sha1_ctx;

void sha1_begin(sha1_ctx *ctx);
void sha1_hash(const unsigned char *data, unsigned long len, sha1_ctx *ctx);
void sha1_end(unsigned char hval[], sha1_ctx *ctx);

#endif
