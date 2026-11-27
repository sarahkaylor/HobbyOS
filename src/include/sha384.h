#ifndef SHA384_H
#define SHA384_H

#include <stdint.h>

/* SHA-384 (FIPS 180-4).  Streaming form plus the one-shot digest; the COT
 * probe uses the one-shot over the FMC image bytes it placed in sysmem. */
struct sha384_ctx {
  uint64_t h[8];
  uint64_t len_lo; /* message length in bits, low 64 */
  uint64_t len_hi; /* message length in bits, high 64 (never set in practice) */
  uint32_t used;   /* bytes buffered in buf */
  uint8_t buf[128];
};

void sha384_init(struct sha384_ctx *c);
void sha384_update(struct sha384_ctx *c, const void *data, uint32_t len);
void sha384_final(struct sha384_ctx *c, uint8_t out[48]);
void sha384_digest(const void *data, uint32_t len, uint8_t out[48]);

#endif /* SHA384_H */
