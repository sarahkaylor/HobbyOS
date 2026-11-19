/* linux_snd_params.h — the ALSA hw_params constraint engine (lane C4-audio,
 * docs/cp-audio.md §3).  Device-free: the same TU links into the kernel
 * (linux_snd.c's HW_REFINE / HW_PARAMS) and the host golden suite
 * (src/host/linux_snd_params_test.c).
 *
 * Layouts are Linux's x86_64 uapi <sound/asound.h> (protocol 2.0.14+):
 * struct snd_pcm_hw_params is 608 bytes, struct snd_interval 12. */
#ifndef LINUX_SND_PARAMS_H
#define LINUX_SND_PARAMS_H

#include <stdint.h>

#define LSND_PARAM_ACCESS 0
#define LSND_PARAM_FORMAT 1
#define LSND_PARAM_SUBFORMAT 2
#define LSND_PARAM_FIRST_INTERVAL 8
#define LSND_PARAM_SAMPLE_BITS 8
#define LSND_PARAM_FRAME_BITS 9
#define LSND_PARAM_CHANNELS 10
#define LSND_PARAM_RATE 11
#define LSND_PARAM_PERIOD_TIME 12
#define LSND_PARAM_PERIOD_SIZE 13
#define LSND_PARAM_PERIOD_BYTES 14
#define LSND_PARAM_PERIODS 15
#define LSND_PARAM_BUFFER_TIME 16
#define LSND_PARAM_BUFFER_SIZE 17
#define LSND_PARAM_BUFFER_BYTES 18
#define LSND_PARAM_TICK_TIME 19
#define LSND_PARAM_LAST_INTERVAL 19

#define LSND_IV_OPENMIN 1u
#define LSND_IV_OPENMAX 2u
#define LSND_IV_INTEGER 4u
#define LSND_IV_EMPTY 8u

struct lsnd_mask {
  uint32_t bits[8];
};
struct lsnd_interval {
  uint32_t min, max;
  uint32_t flags; /* the uapi bitfield word: openmin:1 openmax:1 integer:1 empty:1 */
};
struct lsnd_hw_params {
  uint32_t flags;
  struct lsnd_mask masks[3];
  struct lsnd_mask mres[5];
  struct lsnd_interval intervals[12];
  struct lsnd_interval ires[9];
  uint32_t rmask, cmask, info, msbits, rate_num, rate_den;
  uint64_t fifo_size;
  uint8_t reserved[64];
};
_Static_assert(sizeof(struct lsnd_hw_params) == 608, "snd_pcm_hw_params is 608 bytes");

/* What the hardware supports.  Formats: SNDRV_PCM_FORMAT_* bits; sample bits
 * per enabled format come from lsnd_format_bits(). */
struct lsnd_hw_caps {
  uint32_t access_mask;    /* SNDRV_PCM_ACCESS_* bits */
  uint64_t format_mask;    /* SNDRV_PCM_FORMAT_* bits (< 64) */
  uint32_t channels_min, channels_max;
  uint32_t rate_min, rate_max; /* a single rate in practice */
  uint32_t period_bytes_min, period_bytes_max;
  uint32_t periods_min, periods_max;
  uint32_t buffer_bytes_max;
  uint32_t info;           /* SNDRV_PCM_INFO_* */
};

#define LSND_FORMAT_S16_LE 2
#define LSND_FORMAT_S32_LE 10
#define LSND_FORMAT_FLOAT_LE 14
#define LSND_ACCESS_RW_INTERLEAVED 3

int lsnd_format_bits(int fmt); /* physical sample bits, 0 = unsupported */

/* Linux snd_pcm_hw_refine: narrow every mask/interval to what the caps and
 * the dependency rules allow, set cmask/info/msbits/rate_num/rate_den.
 * Returns 0, or -EINVAL (22) negated when a parameter became empty. */
int lsnd_hw_refine(struct lsnd_hw_params *p, const struct lsnd_hw_caps *caps);
/* Linux snd_pcm_hw_params_choose after a refine: one value per parameter
 * (first, except BUFFER_SIZE: last).  0 or -EINVAL. */
int lsnd_hw_choose(struct lsnd_hw_params *p, const struct lsnd_hw_caps *caps);

/* Helpers for the PCM layer and the tests. */
int lsnd_iv_single(const struct lsnd_interval *i);
uint32_t lsnd_iv_value(const struct lsnd_interval *i);
static inline struct lsnd_interval *lsnd_iv(struct lsnd_hw_params *p, int k) {
  return &p->intervals[k - LSND_PARAM_FIRST_INTERVAL];
}
/* "any" params, as alsa-lib's snd_pcm_hw_params_any() sends them. */
void lsnd_hw_params_any(struct lsnd_hw_params *p);

#endif
