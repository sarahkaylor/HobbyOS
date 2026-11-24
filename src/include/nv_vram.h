#ifndef NV_VRAM_H
#define NV_VRAM_H

/* ============================================================================
 *  nv_vram: the VRAM aperture (BAR1), read and written by the host.
 *
 *  Why this exists: BAR0's falcon windows are read-fenced after START, so every
 *  fact about what the firmware did has had to come from a register the fence
 *  covers (rounds 1-3) or from halt time (round 3's instrument).  BAR1 is a
 *  different path: it is the framebuffer aperture, decoded by PFB, with no
 *  falcon fence anywhere on it.  If FWSEC leaves anything at all in VRAM -- a
 *  completion word, a header, a garbage write -- this is the channel that can
 *  see it, and if it leaves nothing then this channel says so with a prefill
 *  that makes "nothing" an observation rather than a guess.
 *
 *  Address model (docs/nv-gsp-wpr-contract.md section 3.4/3.5): the WPR layout
 *  fields are VRAM byte offsets from the base of FB.  BAR1 is 32 GiB against
 *  24 GiB of VRAM, so an offset inside FB is addressable as BAR1 base + offset
 *  *if* the aperture decodes linearly.  That 1:1 property is INFERRED in the
 *  contract and is what nv_vram_pattern_probe() measures in the run that uses
 *  it -- a region read is only evidence once the probe has answered, because
 *  an aperture that does not decode at 24 GiB answers the same zeros a region
 *  nobody wrote does.
 * ========================================================================== */

#ifdef __x86_64__

#include <stdint.h>

struct nv_dev;

/* One window onto VRAM.  `p` is the ioremap'd, uncached kernel pointer for
 * `vram_off`; offsets passed to the accessors are relative to it. */
struct nv_vram_win {
  volatile uint8_t *p; /* 0 when not open */
  uint64_t phys;       /* BAR1 physical base + vram_off */
  uint64_t vram_off;   /* VRAM byte offset from the base of FB */
  uint32_t len;        /* bytes asked for */
};

/* Map [vram_off, vram_off+len) of VRAM through BAR1.  Returns 0, or a negative
 * errno: -EINVAL (no card / not a hardware dev / misaligned), -ENODEV (no BAR1
 * window), -ENOMEM (ioremap refused).  A second open of the same window is
 * cheap: ioremap answers an existing window in place. */
int nv_vram_open(struct nv_dev *d, uint64_t vram_off, uint32_t len,
                 struct nv_vram_win *w);
void nv_vram_close(struct nv_vram_win *w);

uint32_t nv_vram_rd32(const struct nv_vram_win *w, uint32_t off);
void nv_vram_wr32(struct nv_vram_win *w, uint32_t off, uint32_t val);

/* Statistics over a region, folded chunk by chunk so a 1 MiB scan needs no
 * 1 MiB buffer.  Counting against a `sentinel` the host wrote there first is
 * the instrument: `sentinel` words are words nothing touched, `other` words are
 * words something changed, and the count is exact because every word in the
 * region was written by the host before the fence went up. */
struct nv_vram_scan {
  uint32_t words;           /* words examined */
  uint32_t zero;            /* == 0 */
  uint32_t sentinel;        /* == sentinel (untouched) */
  uint32_t other;           /* neither (changed) */
  uint32_t first_other_off; /* byte offset of the first changed word */
  uint32_t first_other_val;
  uint32_t sum; /* additive checksum of the words read (change detector) */
};

void nv_vram_scan_reset(struct nv_vram_scan *s);
void nv_vram_scan_fold(struct nv_vram_scan *s, const uint32_t *w, uint32_t n,
                       uint32_t base_off, uint32_t sentinel);
/* Read `words` words at `off` through the window and fold them in. */
int nv_vram_scan_window(const struct nv_vram_win *win, uint32_t off,
                        uint32_t words, uint32_t sentinel,
                        struct nv_vram_scan *out);
/* Write `words` words of `val` at `off` through the window. */
int nv_vram_fill(struct nv_vram_win *w, uint32_t off, uint32_t words,
                 uint32_t val);

/* What the two scans together say.  Pure: the unit tier pins the truth table,
 * including the blind case, because a region whose prefill did not stick cannot
 * report a write. */
enum nv_vram_effect {
  NV_VRAM_EFFECT_NOTHING = 0, /* prefill intact, nothing changed: no write */
  NV_VRAM_EFFECT_WROTE = 1,   /* words that were sentinel are now something else */
  NV_VRAM_EFFECT_BLIND = 2,   /* the host's own prefill did not survive: no verdict */
  NV_VRAM_EFFECT_MIXED = 3,   /* changed AND disappeared counts disagree: report raw */
};
int nv_vram_judge(const struct nv_vram_scan *pre, const struct nv_vram_scan *post);
const char *nv_vram_effect_name(int e);

/* ---- the booter's arrival receipt --------------------------------------
 *
 * The SEC2 booter's one host-visible job: it DMAs the bootloader's data region
 * (meta.sysmem_addr_bootloader, size_bootloader = 0x8000 bytes) into VRAM at
 * `boot_bin_offset`.  A prefill through BAR1 makes that arrival a count, and
 * comparing the words against the bootloader's own bytes makes it a
 * fingerprint: a random write and a firmware DMA are different events, and
 * "something changed" cannot tell them apart.
 *
 * The verdict is pure so the truth table is pinned in the unit tier, including
 * the case this project keeps paying runs for -- a region whose prefill did not
 * survive must not be read as "the firmware wrote nothing".
 */
#define NV_BOOTBIN_BLIND 0     /* the prefill did not stick: no verdict */
#define NV_BOOTBIN_UNTOUCHED 1 /* every word is still the host's pattern */
#define NV_BOOTBIN_MATCHED 2   /* the region now holds the bootloader's bytes */
#define NV_BOOTBIN_OTHER 3     /* changed, but not to the expected bytes */
int nv_bootbin_verdict(const struct nv_vram_scan *pre,
                       const struct nv_vram_scan *post, uint32_t match_words);
const char *nv_bootbin_verdict_name(int v);

/* Count the words of VRAM at `off` that equal the little-endian words of
 * `expect` (a host byte buffer), reading through the open window.  `expect` is
 * assembled byte by byte, so a caller may hand it any alignment.  Fills
 * `match_words`, and the first disagreement's offset/value when there is one.
 * Returns 0, or a negative errno when the request is not inside the window. */
int nv_vram_match(const struct nv_vram_win *w, uint32_t off,
                  const uint8_t *expect, uint32_t len, uint32_t *match_words,
                  uint32_t *first_bad_off, uint32_t *first_bad_val);

/* One 1:1 pattern probe: read the word, write `pat`, read it back, restore the
 * word it had.  Returns 1 when the pattern read back and the original was
 * restored, 0 when it did not, negative on a dead window.  `obs` gets what the
 * read-back actually answered. */
int nv_vram_pattern_probe(struct nv_vram_win *w, uint32_t off, uint32_t pat,
                          uint32_t *obs);

/* Does this VRAM offset answer as memory?  Reads one word eight times, two
 * addresses alternately, and writes a value back.  A window that answers an
 * incrementing sequence is a poison, not memory, and no scan of it means
 * anything.  Returns 1 = memory, 0 = poison, negative = refused. */
int nv_vram_behaviour(struct nv_dev *d, uint64_t vram_off, const char *tag);

/* Classify a sequence of repeated reads of ONE address (pure, unit-tested):
 *   NV_VRAM_READS_MEMORY   every read answered the same value
 *   NV_VRAM_READS_COUNTER  each read answered the previous one + 1
 *   NV_VRAM_READS_OTHER    it changed in some other way (or fewer than 2 reads)
 * MEMORY is the only answer a scan of the region can be built on. */
#define NV_VRAM_READS_COUNTER 0
#define NV_VRAM_READS_MEMORY 1
#define NV_VRAM_READS_OTHER 3
int nv_vram_reads_kind(const uint32_t *v, uint32_t n);

/* Fill `out` with one character per 4 KiB page from `first_off`: 'M' when the
 * page answers as memory (eight reads of its first word, all equal), 'P' when
 * it answers a sequence (a poison), '?' when the window refused.  Reads only --
 * this is the instrument that measures the *extent* of a fence without writing
 * to the pages it walks.  Returns 0 or a negative errno. */
int nv_vram_page_map(struct nv_dev *d, uint64_t first_off, uint32_t n_pages,
                     char *out);

/* The BAR1 channel self-test and prefill, in one call, for one run:
 *   1. pattern-probe a low VRAM page (inside the window the driver maps at
 *      boot) and a high one (~24 GiB, the region the WPR layout lives in),
 *   2. prefill [vram_off, vram_off+len) with `sentinel`,
 *   3. scan it back: `pre` is the proof the prefill stuck.
 * Returns 0 when the prefill is trustworthy (the high probe read back), so the
 * caller can post the run's write against it; negative otherwise, with the rows
 * already logged. */
int nv_vram_prefill(struct nv_dev *d, struct nv_vram_win *w, uint64_t vram_off,
                    uint32_t len, uint32_t sentinel, struct nv_vram_scan *pre);

#endif /* __x86_64__ */

#endif /* NV_VRAM_H */
