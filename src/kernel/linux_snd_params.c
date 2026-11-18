/* linux_snd_params.c — ALSA hw_params refine/choose (lane C4-audio).
 *
 * A from-scratch implementation of the semantics of Linux's
 * snd_pcm_hw_refine() / snd_pcm_hw_params_choose() for one fixed device
 * shape: interval arithmetic with open bounds and integer flags, the
 * standard dependency rules between the size/bytes/time/rate parameters,
 * iterated to a fixpoint.  alsa-lib drives every hw_params decision through
 * these two ioctls, so they must agree with what it computes itself
 * (alsa-lib pcm_params.c uses the same rules for its plugins).
 *
 * Device-free; the host golden suite runs the same code
 * (src/host/linux_snd_params_test.c). */
#include "linux_snd_params.h"

#define LSND_EINVAL 22
#define U32MAX 0xFFFFFFFFu

int lsnd_format_bits(int fmt) {
  switch (fmt) {
  case LSND_FORMAT_S16_LE: return 16;
  case LSND_FORMAT_S32_LE: return 32;
  case LSND_FORMAT_FLOAT_LE: return 32;
  default: return 0;
  }
}

/* --- intervals ---------------------------------------------------------- */

typedef struct lsnd_interval iv_t;
#define OMIN(i) (((i)->flags & LSND_IV_OPENMIN) != 0)
#define OMAX(i) (((i)->flags & LSND_IV_OPENMAX) != 0)
#define INTG(i) (((i)->flags & LSND_IV_INTEGER) != 0)

static void iv_set(iv_t *i, uint32_t min, int omin, uint32_t max, int omax, int integer) {
  i->min = min;
  i->max = max;
  i->flags = (omin ? LSND_IV_OPENMIN : 0) | (omax ? LSND_IV_OPENMAX : 0) |
             (integer ? LSND_IV_INTEGER : 0);
}

static int iv_empty(const iv_t *i) {
  if (i->flags & LSND_IV_EMPTY) return 1;
  return i->min > i->max || (i->min == i->max && (OMIN(i) || OMAX(i)));
}

int lsnd_iv_single(const iv_t *i) {
  return i->min == i->max || (i->min + 1 == i->max && (OMIN(i) || OMAX(i)));
}

uint32_t lsnd_iv_value(const iv_t *i) {
  if (OMIN(i) && !OMAX(i)) return i->max;
  return i->min;
}

/* Linux snd_interval_refine: i &= v.  1 changed, 0 unchanged, -EINVAL empty. */
static int iv_refine(iv_t *i, const iv_t *v) {
  int changed = 0;
  if (iv_empty(i)) return -LSND_EINVAL;
  if (i->min < v->min) {
    i->min = v->min;
    i->flags = (i->flags & ~LSND_IV_OPENMIN) | (v->flags & LSND_IV_OPENMIN);
    changed = 1;
  } else if (i->min == v->min && !OMIN(i) && OMIN(v)) {
    i->flags |= LSND_IV_OPENMIN;
    changed = 1;
  }
  if (i->max > v->max) {
    i->max = v->max;
    i->flags = (i->flags & ~LSND_IV_OPENMAX) | (v->flags & LSND_IV_OPENMAX);
    changed = 1;
  } else if (i->max == v->max && !OMAX(i) && OMAX(v)) {
    i->flags |= LSND_IV_OPENMAX;
    changed = 1;
  }
  if (!INTG(i) && INTG(v)) {
    i->flags |= LSND_IV_INTEGER;
    changed = 1;
  }
  if (INTG(i)) {
    if (OMIN(i)) {
      i->min++;
      i->flags &= ~LSND_IV_OPENMIN;
    }
    if (OMAX(i)) {
      i->max--;
      i->flags &= ~LSND_IV_OPENMAX;
    }
  } else if (!OMIN(i) && !OMAX(i) && i->min == i->max) {
    i->flags |= LSND_IV_INTEGER;
  }
  if (iv_empty(i)) {
    i->flags |= LSND_IV_EMPTY;
    return -LSND_EINVAL;
  }
  return changed;
}

static int iv_refine_range(iv_t *i, uint32_t min, uint32_t max, int integer) {
  iv_t v;
  iv_set(&v, min, 0, max, 0, integer);
  return iv_refine(i, &v);
}

static uint32_t mul32(uint32_t a, uint32_t b) {
  uint64_t r = (uint64_t)a * b;
  return r > U32MAX ? U32MAX : (uint32_t)r;
}
static uint32_t div32(uint32_t a, uint32_t b, uint32_t *rem) {
  if (b == 0) {
    *rem = 0;
    return U32MAX;
  }
  *rem = a % b;
  return a / b;
}
static uint32_t muldiv32(uint32_t a, uint32_t b, uint32_t c, uint32_t *rem) {
  uint64_t n = (uint64_t)a * b;
  if (c == 0) {
    *rem = 0;
    return U32MAX;
  }
  uint64_t q = n / c;
  if (q > U32MAX) {
    *rem = 0;
    return U32MAX;
  }
  *rem = (uint32_t)(n % c);
  return (uint32_t)q;
}

static void iv_mul(const iv_t *a, const iv_t *b, iv_t *c) {
  iv_set(c, mul32(a->min, b->min), OMIN(a) || OMIN(b), mul32(a->max, b->max),
         OMAX(a) || OMAX(b), INTG(a) && INTG(b));
}

static void iv_div(const iv_t *a, const iv_t *b, iv_t *c) {
  uint32_t r;
  uint32_t min = div32(a->min, b->max, &r);
  int omin = r || OMIN(a) || OMAX(b);
  uint32_t max;
  int omax;
  if (b->min > 0) {
    max = div32(a->max, b->min, &r);
    if (r) {
      if (max < U32MAX) max++;
      omax = 1;
    } else {
      omax = OMAX(a) || OMIN(b);
    }
  } else {
    max = U32MAX;
    omax = 0;
  }
  iv_set(c, min, omin, max, omax, 0);
}

/* c = a * b / k */
static void iv_muldivk(const iv_t *a, const iv_t *b, uint32_t k, iv_t *c) {
  uint32_t r;
  uint32_t min = muldiv32(a->min, b->min, k, &r);
  int omin = r || OMIN(a) || OMIN(b);
  uint32_t max = muldiv32(a->max, b->max, k, &r);
  int omax;
  if (r) {
    if (max < U32MAX) max++;
    omax = 1;
  } else {
    omax = OMAX(a) || OMAX(b);
  }
  iv_set(c, min, omin, max, omax, 0);
}

/* c = a * k / b */
static void iv_mulkdiv(const iv_t *a, uint32_t k, const iv_t *b, iv_t *c) {
  uint32_t r;
  uint32_t min = muldiv32(a->min, k, b->max, &r);
  int omin = r || OMIN(a) || OMAX(b);
  uint32_t max;
  int omax;
  if (b->min > 0) {
    max = muldiv32(a->max, k, b->min, &r);
    if (r) {
      if (max < U32MAX) max++;
      omax = 1;
    } else {
      omax = OMAX(a) || OMIN(b);
    }
  } else {
    max = U32MAX;
    omax = 0;
  }
  iv_set(c, min, omin, max, omax, 0);
}

/* --- masks --------------------------------------------------------------- */

static int mask_empty(const struct lsnd_mask *m) {
  for (int i = 0; i < 8; i++)
    if (m->bits[i]) return 0;
  return 1;
}
static int mask_refine64(struct lsnd_mask *m, uint64_t allow) {
  struct lsnd_mask o = *m;
  m->bits[0] &= (uint32_t)allow;
  m->bits[1] &= (uint32_t)(allow >> 32);
  for (int i = 2; i < 8; i++) m->bits[i] = 0;
  if (mask_empty(m)) return -LSND_EINVAL;
  for (int i = 0; i < 8; i++)
    if (o.bits[i] != m->bits[i]) return 1;
  return 0;
}
static uint64_t mask_get64(const struct lsnd_mask *m) {
  return (uint64_t)m->bits[0] | ((uint64_t)m->bits[1] << 32);
}
static int mask_first(struct lsnd_mask *m) {
  uint64_t v = mask_get64(m);
  if (!v) return -LSND_EINVAL;
  uint64_t low = v & (~v + 1);
  return mask_refine64(m, low);
}

/* --- rules ------------------------------------------------------------------ */

enum { OP_MUL, OP_DIV, OP_MULDIVK, OP_MULKDIV };
struct rule {
  uint8_t op, t, a, b;
  uint32_t k;
};
#define P(x) LSND_PARAM_##x
static const struct rule rules[] = {
    {OP_MUL, P(FRAME_BITS), P(SAMPLE_BITS), P(CHANNELS), 0},
    {OP_DIV, P(SAMPLE_BITS), P(FRAME_BITS), P(CHANNELS), 0},
    {OP_DIV, P(CHANNELS), P(FRAME_BITS), P(SAMPLE_BITS), 0},
    {OP_MULKDIV, P(FRAME_BITS), P(PERIOD_BYTES), P(PERIOD_SIZE), 8},
    {OP_MULKDIV, P(FRAME_BITS), P(BUFFER_BYTES), P(BUFFER_SIZE), 8},
    {OP_MULKDIV, P(RATE), P(PERIOD_SIZE), P(PERIOD_TIME), 1000000},
    {OP_MULKDIV, P(RATE), P(BUFFER_SIZE), P(BUFFER_TIME), 1000000},
    {OP_DIV, P(PERIODS), P(BUFFER_SIZE), P(PERIOD_SIZE), 0},
    {OP_DIV, P(PERIOD_SIZE), P(BUFFER_SIZE), P(PERIODS), 0},
    {OP_MULKDIV, P(PERIOD_SIZE), P(PERIOD_BYTES), P(FRAME_BITS), 8},
    {OP_MULDIVK, P(PERIOD_SIZE), P(PERIOD_TIME), P(RATE), 1000000},
    {OP_MUL, P(BUFFER_SIZE), P(PERIOD_SIZE), P(PERIODS), 0},
    {OP_MULKDIV, P(BUFFER_SIZE), P(BUFFER_BYTES), P(FRAME_BITS), 8},
    {OP_MULDIVK, P(BUFFER_SIZE), P(BUFFER_TIME), P(RATE), 1000000},
    {OP_MULDIVK, P(PERIOD_BYTES), P(PERIOD_SIZE), P(FRAME_BITS), 8},
    {OP_MULDIVK, P(BUFFER_BYTES), P(BUFFER_SIZE), P(FRAME_BITS), 8},
    {OP_MULKDIV, P(PERIOD_TIME), P(PERIOD_SIZE), P(RATE), 1000000},
    {OP_MULKDIV, P(BUFFER_TIME), P(BUFFER_SIZE), P(RATE), 1000000},
};

static int apply_rule(struct lsnd_hw_params *p, const struct rule *r) {
  iv_t t;
  const iv_t *a = lsnd_iv(p, r->a), *b = lsnd_iv(p, r->b);
  switch (r->op) {
  case OP_MUL: iv_mul(a, b, &t); break;
  case OP_DIV: iv_div(a, b, &t); break;
  case OP_MULDIVK: iv_muldivk(a, b, r->k, &t); break;
  default: iv_mulkdiv(a, r->k, b, &t); break;
  }
  return iv_refine(lsnd_iv(p, r->t), &t);
}

/* FORMAT <-> SAMPLE_BITS */
static int format_bits_rules(struct lsnd_hw_params *p) {
  uint64_t fm = mask_get64(&p->masks[LSND_PARAM_FORMAT]);
  iv_t *sb = lsnd_iv(p, LSND_PARAM_SAMPLE_BITS);
  uint64_t keep = 0;
  uint32_t bmin = U32MAX, bmax = 0;
  for (int f = 0; f < 64; f++) {
    if (!(fm & (1ull << f))) continue;
    uint32_t bits = (uint32_t)lsnd_format_bits(f);
    /* the format survives when its width is inside SAMPLE_BITS */
    if (bits && !(bits < sb->min || bits > sb->max || (bits == sb->min && OMIN(sb)) ||
                  (bits == sb->max && OMAX(sb)))) {
      keep |= 1ull << f;
      if (bits < bmin) bmin = bits;
      if (bits > bmax) bmax = bits;
    }
  }
  int c1 = mask_refine64(&p->masks[LSND_PARAM_FORMAT], keep);
  if (c1 < 0) return c1;
  int c2 = iv_refine_range(sb, bmin, bmax, 1);
  if (c2 < 0) return c2;
  return c1 || c2;
}

static int refine_rules(struct lsnd_hw_params *p) {
  for (int pass = 0; pass < 100; pass++) {
    int changed = 0;
    int c = format_bits_rules(p);
    if (c < 0) return c;
    changed |= c;
    for (unsigned i = 0; i < sizeof rules / sizeof rules[0]; i++) {
      c = apply_rule(p, &rules[i]);
      if (c < 0) return c;
      changed |= c;
    }
    if (!changed) return 0;
  }
  return 0;
}

static int same_iv(const iv_t *a, const iv_t *b) {
  return a->min == b->min && a->max == b->max && a->flags == b->flags;
}

int lsnd_hw_refine(struct lsnd_hw_params *p, const struct lsnd_hw_caps *caps) {
  struct lsnd_hw_params before = *p;
  int r;
  for (int k = LSND_PARAM_FIRST_INTERVAL; k <= LSND_PARAM_LAST_INTERVAL; k++)
    if (iv_empty(lsnd_iv(p, k))) return -LSND_EINVAL;
  if ((r = mask_refine64(&p->masks[LSND_PARAM_ACCESS], caps->access_mask)) < 0) return r;
  if ((r = mask_refine64(&p->masks[LSND_PARAM_FORMAT], caps->format_mask)) < 0) return r;
  if ((r = mask_refine64(&p->masks[LSND_PARAM_SUBFORMAT], 1)) < 0) return r; /* STD */
  struct {
    int k;
    uint32_t min, max;
  } hw[] = {
      {LSND_PARAM_FRAME_BITS, 1, U32MAX},
      {LSND_PARAM_CHANNELS, caps->channels_min, caps->channels_max},
      {LSND_PARAM_RATE, caps->rate_min, caps->rate_max},
      {LSND_PARAM_PERIOD_SIZE, 1, U32MAX},
      {LSND_PARAM_PERIOD_BYTES, caps->period_bytes_min, caps->period_bytes_max},
      {LSND_PARAM_PERIODS, caps->periods_min, caps->periods_max},
      {LSND_PARAM_BUFFER_SIZE, 1, U32MAX},
      {LSND_PARAM_BUFFER_BYTES, 1, caps->buffer_bytes_max},
  };
  for (unsigned i = 0; i < sizeof hw / sizeof hw[0]; i++)
    if ((r = iv_refine_range(lsnd_iv(p, hw[i].k), hw[i].min, hw[i].max, 1)) < 0) return r;
  if ((r = refine_rules(p)) < 0) return r;

  uint32_t cm = 0;
  for (int k = 0; k < 3; k++)
    for (int w = 0; w < 8; w++)
      if (p->masks[k].bits[w] != before.masks[k].bits[w]) cm |= 1u << k;
  for (int k = LSND_PARAM_FIRST_INTERVAL; k <= LSND_PARAM_LAST_INTERVAL; k++)
    if (!same_iv(lsnd_iv(p, k), &before.intervals[k - LSND_PARAM_FIRST_INTERVAL]))
      cm |= 1u << k;
  p->cmask |= cm;
  p->rmask = 0;
  p->info = caps->info;
  const iv_t *sb = lsnd_iv(p, LSND_PARAM_SAMPLE_BITS);
  if (lsnd_iv_single(sb)) p->msbits = lsnd_iv_value(sb);
  const iv_t *rt = lsnd_iv(p, LSND_PARAM_RATE);
  if (lsnd_iv_single(rt)) {
    p->rate_num = lsnd_iv_value(rt);
    p->rate_den = 1;
  }
  return 0;
}

/* Linux snd_interval_refine_first / _last. */
static int iv_first(iv_t *i) {
  if (iv_empty(i)) return -LSND_EINVAL;
  if (lsnd_iv_single(i)) return 0;
  uint32_t last_max = i->max;
  i->max = i->min;
  if (OMIN(i)) i->max++;
  if (!(OMAX(i) && i->max >= last_max)) i->flags &= ~LSND_IV_OPENMAX;
  return 1;
}
static int iv_last(iv_t *i) {
  if (iv_empty(i)) return -LSND_EINVAL;
  if (lsnd_iv_single(i)) return 0;
  uint32_t last_min = i->min;
  i->min = i->max;
  if (OMAX(i)) i->min--;
  if (!(OMIN(i) && i->min <= last_min)) i->flags &= ~LSND_IV_OPENMIN;
  return 1;
}

int lsnd_hw_choose(struct lsnd_hw_params *p, const struct lsnd_hw_caps *caps) {
  static const int vars[] = {LSND_PARAM_ACCESS,      LSND_PARAM_FORMAT,      LSND_PARAM_SUBFORMAT,
                             LSND_PARAM_CHANNELS,    LSND_PARAM_RATE,        LSND_PARAM_PERIOD_TIME,
                             LSND_PARAM_BUFFER_SIZE, LSND_PARAM_TICK_TIME};
  int r = lsnd_hw_refine(p, caps);
  if (r < 0) return r;
  for (unsigned v = 0; v < sizeof vars / sizeof vars[0]; v++) {
    int k = vars[v];
    if (k < LSND_PARAM_FIRST_INTERVAL)
      r = mask_first(&p->masks[k]);
    else if (k == LSND_PARAM_BUFFER_SIZE)
      r = iv_last(lsnd_iv(p, k));
    else
      r = iv_first(lsnd_iv(p, k));
    if (r < 0) return r;
    if ((r = lsnd_hw_refine(p, caps)) < 0) return r;
  }
  static const int need[] = {LSND_PARAM_CHANNELS, LSND_PARAM_RATE, LSND_PARAM_PERIOD_SIZE,
                             LSND_PARAM_PERIODS, LSND_PARAM_BUFFER_SIZE, LSND_PARAM_SAMPLE_BITS};
  for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++)
    if (!lsnd_iv_single(lsnd_iv(p, need[i]))) return -LSND_EINVAL;
  return 0;
}

void lsnd_hw_params_any(struct lsnd_hw_params *p) {
  uint8_t *b = (uint8_t *)p;
  for (unsigned i = 0; i < sizeof *p; i++) b[i] = 0;
  for (int k = 0; k < 3; k++)
    for (int w = 0; w < 8; w++) p->masks[k].bits[w] = U32MAX;
  for (int k = LSND_PARAM_FIRST_INTERVAL; k <= LSND_PARAM_LAST_INTERVAL; k++)
    iv_set(lsnd_iv(p, k), 0, 0, U32MAX, 0, 0);
  p->rmask = U32MAX;
  p->info = U32MAX;
}
