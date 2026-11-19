/* linux_snd_params_test.c — host golden test for the ALSA hw_params engine
 * (src/kernel/linux_snd_params.c, lane C4-audio).  Same TU as the kernel.
 *
 * The cases are what alsa-lib actually sends: snd_pcm_hw_params_any() then a
 * refine; fixing access/format/channels/rate; a period_time / buffer_time
 * "near" request the way Wine's winealsa and aplay do it (they narrow the
 * interval and refine); and the HW_PARAMS choose.  Expected numbers are the
 * Linux kernel's for the same constraints (worked by hand from
 * pcm_native.c's rules; frame = 4 bytes, rate 48000). */
#include <stdio.h>
#include <string.h>
#include "linux_snd_params.h"

static int fails, checks;
#define CHECK(c, ...)                         \
  do {                                        \
    checks++;                                 \
    if (!(c)) {                               \
      fails++;                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                    \
      printf("\n");                           \
    }                                         \
  } while (0)

static struct lsnd_hw_caps caps(void) {
  struct lsnd_hw_caps c;
  memset(&c, 0, sizeof c);
  c.access_mask = 1u << LSND_ACCESS_RW_INTERLEAVED;
  c.format_mask = 1ull << LSND_FORMAT_S16_LE;
  c.channels_min = c.channels_max = 2;
  c.rate_min = c.rate_max = 48000;
  c.period_bytes_min = 1024;
  c.period_bytes_max = 65536;
  c.periods_min = 2;
  c.periods_max = 64;
  c.buffer_bytes_max = 512 * 1024;
  c.info = 0x100 | 0x10000 | 0x40000 | 0x80000;
  return c;
}

static void set_iv(struct lsnd_hw_params *p, int k, uint32_t lo, uint32_t hi) {
  struct lsnd_interval *i = lsnd_iv(p, k);
  i->min = lo;
  i->max = hi;
  i->flags = 0;
}

int main(void) {
  struct lsnd_hw_caps c = caps();
  struct lsnd_hw_params p;

  /* 1. any() -> refine: the device's whole space. */
  lsnd_hw_params_any(&p);
  int r = lsnd_hw_refine(&p, &c);
  CHECK(r == 0, "refine(any) = %d", r);
  CHECK(p.masks[LSND_PARAM_ACCESS].bits[0] == (1u << 3), "access mask %#x",
        p.masks[LSND_PARAM_ACCESS].bits[0]);
  CHECK(p.masks[LSND_PARAM_FORMAT].bits[0] == (1u << 2), "format mask %#x",
        p.masks[LSND_PARAM_FORMAT].bits[0]);
  CHECK(lsnd_iv(&p, LSND_PARAM_SAMPLE_BITS)->min == 16 && lsnd_iv(&p, LSND_PARAM_SAMPLE_BITS)->max == 16,
        "sample bits");
  CHECK(lsnd_iv(&p, LSND_PARAM_FRAME_BITS)->min == 32 && lsnd_iv(&p, LSND_PARAM_FRAME_BITS)->max == 32,
        "frame bits %u..%u", lsnd_iv(&p, LSND_PARAM_FRAME_BITS)->min, lsnd_iv(&p, LSND_PARAM_FRAME_BITS)->max);
  CHECK(lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->min == 256 && lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->max == 16384,
        "period size %u..%u", lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->min, lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->max);
  CHECK(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->min == 512 && lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->max == 131072,
        "buffer size %u..%u", lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->min, lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->max);
  CHECK(lsnd_iv(&p, LSND_PARAM_PERIOD_TIME)->min == 5333 && lsnd_iv(&p, LSND_PARAM_PERIOD_TIME)->max == 341334,
        "period time %u..%u flags %#x", lsnd_iv(&p, LSND_PARAM_PERIOD_TIME)->min,
        lsnd_iv(&p, LSND_PARAM_PERIOD_TIME)->max, lsnd_iv(&p, LSND_PARAM_PERIOD_TIME)->flags);
  CHECK(p.msbits == 16 && p.rate_num == 48000 && p.rate_den == 1, "msbits %u rate %u/%u", p.msbits,
        p.rate_num, p.rate_den);
  CHECK(p.cmask != 0 && p.rmask == 0, "cmask %#x rmask %#x", p.cmask, p.rmask);

  /* 2. HW_PARAMS with nothing narrowed: Linux picks the minimum period time
     and then the LARGEST buffer that still fits. */
  r = lsnd_hw_choose(&p, &c);
  CHECK(r == 0, "choose = %d", r);
  CHECK(lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)) == 256, "chosen period %u",
        lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)));
  CHECK(lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_PERIODS)) == 64, "chosen periods %u",
        lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_PERIODS)));
  CHECK(lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)) == 16384, "chosen buffer %u",
        lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)));

  /* 3. A Wine-style request: 10 ms periods, 4 periods. */
  lsnd_hw_params_any(&p);
  set_iv(&p, LSND_PARAM_PERIOD_TIME, 10000, 10000);
  set_iv(&p, LSND_PARAM_PERIODS, 4, 4);
  r = lsnd_hw_refine(&p, &c);
  CHECK(r == 0, "refine(10ms x4) = %d", r);
  CHECK(lsnd_iv_single(lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)) &&
            lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)) == 480,
        "period %u..%u", lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->min, lsnd_iv(&p, LSND_PARAM_PERIOD_SIZE)->max);
  CHECK(lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)) == 1920 &&
            lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_BYTES)) == 7680,
        "buffer %u bytes %u", lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)),
        lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_BYTES)));
  r = lsnd_hw_choose(&p, &c);
  CHECK(r == 0, "choose(10ms x4) = %d", r);

  /* 4. An unsupported rate / format / channel count is EINVAL (alsa-lib's
     plug layer converts in user space). */
  lsnd_hw_params_any(&p);
  set_iv(&p, LSND_PARAM_RATE, 44100, 44100);
  CHECK(lsnd_hw_refine(&p, &c) == -22, "44100 must be EINVAL");
  lsnd_hw_params_any(&p);
  memset(&p.masks[LSND_PARAM_FORMAT], 0, sizeof p.masks[0]);
  p.masks[LSND_PARAM_FORMAT].bits[0] = 1u << LSND_FORMAT_FLOAT_LE;
  CHECK(lsnd_hw_refine(&p, &c) == -22, "FLOAT_LE must be EINVAL");
  lsnd_hw_params_any(&p);
  set_iv(&p, LSND_PARAM_CHANNELS, 6, 6);
  CHECK(lsnd_hw_refine(&p, &c) == -22, "6 channels must be EINVAL");

  /* 5. A buffer too large for the ring is clipped, a period too small is EINVAL. */
  lsnd_hw_params_any(&p);
  set_iv(&p, LSND_PARAM_BUFFER_TIME, 0, 10000000);
  CHECK(lsnd_hw_refine(&p, &c) == 0 && lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->max == 131072,
        "buffer max %u", lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)->max);
  lsnd_hw_params_any(&p);
  set_iv(&p, LSND_PARAM_PERIOD_SIZE, 64, 64);
  CHECK(lsnd_hw_refine(&p, &c) == -22, "64-frame period must be EINVAL");

  /* 6. Open-interval "near" narrowing (alsa-lib snd_pcm_hw_param_set_near
     refines [v, v] first, then (v-x, v+x) with open bounds). */
  lsnd_hw_params_any(&p);
  {
    struct lsnd_interval *i = lsnd_iv(&p, LSND_PARAM_BUFFER_TIME);
    i->min = 99999;
    i->max = 100001;
    i->flags = LSND_IV_OPENMIN | LSND_IV_OPENMAX;
  }
  r = lsnd_hw_refine(&p, &c);
  CHECK(r == 0 && lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)) == 4800,
        "100 ms buffer -> %u frames (r=%d)", lsnd_iv_value(lsnd_iv(&p, LSND_PARAM_BUFFER_SIZE)), r);

  printf("linux_snd_params_test: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
