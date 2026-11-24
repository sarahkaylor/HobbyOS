/* ============================================================================
 *  nv_vram: the framebuffer aperture (BAR1) as a read/write channel.
 *
 *  Rounds 1-3 established that no host-side falcon channel answers after START:
 *  the windows read the fence word, the engine store queue accepts requests and
 *  never retires them.  BAR1 is not that path -- it is PFB's decode of VRAM, so
 *  a firmware write into the WPR region lands where this file can read it, and
 *  the falcon never sees the access.
 *
 *  The method, and why each step is here:
 *    - pattern-probe a low and a high VRAM offset.  The contract calls the
 *      1:1 correspondence of BAR1 offset and VRAM offset INFERRED; until this
 *      probe answers, a region that reads as zeros is indistinguishable from an
 *      aperture that does not decode at 24 GiB.
 *    - prefill the region with a non-zero sentinel and read the prefill back.
 *      "FWSEC wrote nothing" is a real answer only when the words it would have
 *      written were the host's own pattern first (PLAN.md section 6, rule 2).
 *    - scan the region again after the launch and count.  The sentinel makes
 *      the count exact: every word the firmware did not touch is still the
 *      pattern, so `other` is a byte-exact measure of what it wrote.
 *
 *  Nothing here is falcon-specific and nothing here writes a register: a VRAM
 *  write through BAR1 is a PFB access, which is why it is legal before START
 *  when an engine store request is not (round 3 cost a launch proving that).
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_vram.h"
#include "kmem.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* ---- window ------------------------------------------------------------- */

int nv_vram_open(struct nv_dev *d, uint64_t vram_off, uint32_t len,
                 struct nv_vram_win *w) {
  if (!d || !w) return -EINVAL;
  w->p = 0;
  w->phys = 0;
  w->vram_off = vram_off;
  w->len = len;
  if (!len) return -EINVAL;
  if (!d->bar1_window_phys || !d->bar1_window_size) return -ENODEV;
  /* The BAR1 window this driver mapped at boot covers VRAM from offset 0; a
   * second ioremap at base+off answers the same window when the request fits
   * inside it, and a new one otherwise.  Both are the same aperture. */
  uint64_t pa = d->bar1_window_phys + vram_off;
  volatile void *p = ioremap(pa, len);
  if (!p) return -ENOMEM;
  w->p = (volatile uint8_t *)p;
  w->phys = pa;
  return 0;
}

void nv_vram_close(struct nv_vram_win *w) {
  /* ioremap windows are never re-pointed or unmapped (src/kernel/arch/x64/
   * mmu.c), so closing is dropping the pointer.  Kept as a named step so a
   * reader does not look for a free(). */
  if (!w) return;
  w->p = 0;
}

uint32_t nv_vram_rd32(const struct nv_vram_win *w, uint32_t off) {
  if (!w || !w->p) return 0xffffffffu;
  volatile uint32_t *q = (volatile uint32_t *)(w->p + (off & ~3u));
  return *q;
}

void nv_vram_wr32(struct nv_vram_win *w, uint32_t off, uint32_t val) {
  if (!w || !w->p) return;
  volatile uint32_t *q = (volatile uint32_t *)(w->p + (off & ~3u));
  *q = val;
}

/* ---- scan --------------------------------------------------------------- */

void nv_vram_scan_reset(struct nv_vram_scan *s) {
  if (!s) return;
  s->words = 0;
  s->zero = 0;
  s->sentinel = 0;
  s->other = 0;
  s->first_other_off = 0;
  s->first_other_val = 0;
  s->sum = 0;
}

void nv_vram_scan_fold(struct nv_vram_scan *s, const uint32_t *w, uint32_t n,
                       uint32_t base_off, uint32_t sentinel) {
  if (!s || !w) return;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t v = w[i];
    s->words++;
    s->sum += v;
    if (v == 0)
      s->zero++;
    if (v == sentinel)
      s->sentinel++;
    else {
      if (s->other == 0) {
        s->first_other_off = base_off + i * 4u;
        s->first_other_val = v;
      }
      s->other++;
    }
  }
}

int nv_vram_scan_window(const struct nv_vram_win *win, uint32_t off,
                        uint32_t words, uint32_t sentinel,
                        struct nv_vram_scan *out) {
  if (!win || !win->p || !out) return -EINVAL;
  if ((uint64_t)off + (uint64_t)words * 4u > (uint64_t)win->len) return -EINVAL;
  nv_vram_scan_reset(out);
  /* 64 words at a time: one cache-line pair of MMIO reads per fold and no
   * 1 MiB buffer in the kernel image. */
  uint32_t chunk[64];
  uint32_t done = 0;
  while (done < words) {
    uint32_t n = words - done;
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++)
      chunk[i] = nv_vram_rd32(win, off + (done + i) * 4u);
    nv_vram_scan_fold(out, chunk, n, off + done * 4u, sentinel);
    done += n;
  }
  return 0;
}

int nv_vram_fill(struct nv_vram_win *w, uint32_t off, uint32_t words,
                 uint32_t val) {
  if (!w || !w->p) return -EINVAL;
  if ((uint64_t)off + (uint64_t)words * 4u > (uint64_t)w->len) return -EINVAL;
  for (uint32_t i = 0; i < words; i++) nv_vram_wr32(w, off + i * 4u, val);
  return 0;
}

/* ---- the verdict -------------------------------------------------------- */

int nv_vram_judge(const struct nv_vram_scan *pre,
                  const struct nv_vram_scan *post) {
  if (!pre || !post) return NV_VRAM_EFFECT_BLIND;
  /* The prefill is the instrument.  If the region did not read back as the
   * pattern the host itself just wrote, then this channel cannot report a
   * firmware write either -- and a "0 words changed" from such a region is
   * meaningless, not negative. */
  if (pre->sentinel != pre->words || pre->other != 0) return NV_VRAM_EFFECT_BLIND;
  if (post->sentinel + post->other != post->words) return NV_VRAM_EFFECT_MIXED;
  if (post->sentinel == post->words) return NV_VRAM_EFFECT_NOTHING;
  /* Words that were the sentinel are not anymore, and no word appeared that
   * was not counted: a straight write over the host's pattern. */
  return NV_VRAM_EFFECT_WROTE;
}

const char *nv_vram_effect_name(int e) {
  switch (e) {
  case NV_VRAM_EFFECT_NOTHING: return "NOTHING-WRITTEN";
  case NV_VRAM_EFFECT_WROTE: return "WRITTEN";
  case NV_VRAM_EFFECT_MIXED: return "MIXED";
  default: return "BLIND";
  }
}

/* ---- the booter's arrival receipt ---------------------------------------
 *
 * Same blind rule as nv_vram_judge, then one more question that FWSEC's FRTS
 * check never had to ask: are the words the ones the booter would have put
 * there?  The bootloader's data region is a file on our own disk, so a byte
 * match is a fingerprint and not a coincidence -- while a region that changed
 * to anything else is reported as OTHER with its counts, never promoted.
 */
int nv_bootbin_verdict(const struct nv_vram_scan *pre,
                       const struct nv_vram_scan *post, uint32_t match_words) {
  if (!pre || !post) return NV_BOOTBIN_BLIND;
  if (pre->words == 0 || post->words != pre->words) return NV_BOOTBIN_BLIND;
  if (pre->sentinel != pre->words || pre->other != 0 || pre->zero != 0)
    return NV_BOOTBIN_BLIND;
  if (post->other == 0) return NV_BOOTBIN_UNTOUCHED;
  /* Every word has to match, not most of them: a half-finished DMA is not the
   * booter having arrived, and "8191 of 8192" is a number the row prints. */
  if (match_words >= post->words) return NV_BOOTBIN_MATCHED;
  return NV_BOOTBIN_OTHER;
}

const char *nv_bootbin_verdict_name(int v) {
  switch (v) {
  case NV_BOOTBIN_UNTOUCHED: return "UNTOUCHED";
  case NV_BOOTBIN_MATCHED: return "MATCHED";
  case NV_BOOTBIN_OTHER: return "OTHER";
  default: return "BLIND";
  }
}

int nv_vram_match(const struct nv_vram_win *w, uint32_t off,
                  const uint8_t *expect, uint32_t len, uint32_t *match_words,
                  uint32_t *first_bad_off, uint32_t *first_bad_val) {
  if (!w || !w->p || !expect || !match_words) return -EINVAL;
  if ((uint64_t)off + (uint64_t)len > (uint64_t)w->len) return -EINVAL;
  uint32_t words = len / 4u;
  uint32_t match = 0;
  uint32_t bad_off = 0, bad_val = 0;
  int have_bad = 0;
  for (uint32_t i = 0; i < words; i++) {
    const uint8_t *e = expect + i * 4u;
    /* Byte-wise on both sides: the wire image is a byte buffer and this kernel
     * does not make wide loads of structures at arbitrary offsets. */
    uint32_t ew = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                  ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
    uint32_t v = nv_vram_rd32(w, off + i * 4u);
    if (v == ew) {
      match++;
    } else if (!have_bad) {
      have_bad = 1;
      bad_off = off + i * 4u;
      bad_val = v;
    }
  }
  *match_words = match;
  if (first_bad_off) *first_bad_off = bad_off;
  if (first_bad_val) *first_bad_val = bad_val;
  return 0;
}

int nv_vram_pattern_probe(struct nv_vram_win *w, uint32_t off, uint32_t pat,
                          uint32_t *obs) {
  if (!w || !w->p) return -EINVAL;
  uint32_t was = nv_vram_rd32(w, off);
  nv_vram_wr32(w, off, pat);
  uint32_t got = nv_vram_rd32(w, off);
  nv_vram_wr32(w, off, was);
  uint32_t back = nv_vram_rd32(w, off);
  if (obs) *obs = got;
  if (got != pat) return 0;
  return back == was;
}

/* ---- is the window answering memory, or a poison? ----------------------
 *
 * A region that reads 0xBAD0AC0A when asked twice for the same word and
 * 0xBAD0AC2A the second time is not memory: it is an access counter, and every
 * conclusion drawn from "the words changed" would be false.  The discriminator
 * is three questions asked of one address:
 *
 *   rep     the same word, eight reads in a row.  Memory answers the same
 *           value eight times; a counter answers a sequence.
 *   two     the word and the same word 4 KiB up, alternating.  If both
 *           addresses walk one shared sequence the counter is global; if each
 *           keeps its own the region is addressable but poisoned.
 *   wb      write a known value and read it back four times.  This separates
 *           "reads are poisoned" from "the region is write-protected too".
 *
 * Returns 1 when the window answers memory, 0 when it is a poison, negative on
 * a refused window.
 */
int nv_vram_reads_kind(const uint32_t *v, uint32_t n) {
  if (!v || n < 2) return NV_VRAM_READS_OTHER;
  int same = 1, step1 = 1;
  for (uint32_t i = 1; i < n; i++) {
    if (v[i] != v[0]) same = 0;
    if (v[i] != v[i - 1] + 1u) step1 = 0;
  }
  if (same) return NV_VRAM_READS_MEMORY;
  if (step1) return NV_VRAM_READS_COUNTER;
  return NV_VRAM_READS_OTHER;
}

int nv_vram_page_map(struct nv_dev *d, uint64_t first_off, uint32_t n_pages,
                     char *out) {
  if (!d || !out || !n_pages) return -EINVAL;
  for (uint32_t i = 0; i < n_pages; i++) {
    struct nv_vram_win w;
    if (nv_vram_open(d, first_off + (uint64_t)i * 0x1000ULL, 4, &w) != 0) {
      out[i] = '?';
      continue;
    }
    uint32_t v[8];
    for (uint32_t k = 0; k < 8; k++) v[k] = nv_vram_rd32(&w, 0);
    int kind = nv_vram_reads_kind(v, 8);
    out[i] = (kind == NV_VRAM_READS_MEMORY) ? 'M'
             : (kind == NV_VRAM_READS_COUNTER ? 'P' : 'n');
  }
  out[n_pages] = 0;
  return 0;
}

int nv_vram_behaviour(struct nv_dev *d, uint64_t vram_off, const char *tag) {
  struct nv_vram_win w;
  if (!d) return -EINVAL;
  if (nv_vram_open(d, vram_off, 0x2000, &w) != 0) {
    uart_puts("[NV-BAR1] probe tag=");
    uart_puts(tag ? tag : "?");
    uart_puts(" off=");
    uart_print_hex(vram_off);
    uart_puts(" verdict=WINDOW-REFUSED\n");
    return -ENODEV;
  }

  uint32_t rep[8];
  for (uint32_t i = 0; i < 8; i++) rep[i] = nv_vram_rd32(&w, 0);
  uart_puts("[NV-BAR1] probe tag=");
  uart_puts(tag ? tag : "?");
  uart_puts(" off=");
  uart_print_hex(vram_off);
  uart_puts(" rep:");
  for (uint32_t i = 0; i < 8; i++) {
    uart_puts(" ");
    uart_print_hex(rep[i]);
  }
  int kind = nv_vram_reads_kind(rep, 8);
  int stable = (kind == NV_VRAM_READS_MEMORY);
  uart_puts(kind == NV_VRAM_READS_MEMORY
                ? " verdict=MEMORY-SAME-VALUE-8x\n"
                : (kind == NV_VRAM_READS_COUNTER
                       ? " verdict=ACCESS-COUNTER (+1 per read: poison, not "
                         "memory)\n"
                       : " verdict=NOT-MEMORY (reads change, not by +1)\n"));

  /* Address independence: the same counter for both addresses means the
   * window ignores the address entirely. */
  uint32_t two[8];
  for (uint32_t i = 0; i < 4; i++) {
    two[2 * i] = nv_vram_rd32(&w, 0);
    two[2 * i + 1] = nv_vram_rd32(&w, 0x1000);
  }
  uart_puts("[NV-BAR1] probe tag=");
  uart_puts(tag ? tag : "?");
  uart_puts(" two-addr:");
  for (uint32_t i = 0; i < 8; i++) {
    uart_puts(" ");
    uart_print_hex(two[i]);
  }
  int shared = 1;
  for (uint32_t i = 1; i < 8; i++)
    if (two[i] != two[i - 1] + 1u) shared = 0;
  uart_puts(shared ? " verdict=ONE-COUNTER-FOR-BOTH (the address is ignored)\n"
                   : " verdict=TWO-LANES (each address keeps its own answer)\n");

  /* Write-back: is the region still writable from the host? */
  uint32_t was = nv_vram_rd32(&w, 0);
  nv_vram_wr32(&w, 0, 0x4E563441u);
  uint32_t rb[4];
  for (uint32_t i = 0; i < 4; i++) rb[i] = nv_vram_rd32(&w, 0);
  nv_vram_wr32(&w, 0, was);
  int landed = 1;
  for (uint32_t i = 0; i < 4; i++)
    if (rb[i] != 0x4E563441u) landed = 0;
  uart_puts("[NV-BAR1] probe tag=");
  uart_puts(tag ? tag : "?");
  uart_puts(" writeback wrote=0x4E563441 read:");
  for (uint32_t i = 0; i < 4; i++) {
    uart_puts(" ");
    uart_print_hex(rb[i]);
  }
  uart_puts(landed ? " verdict=WRITABLE-AND-READS-ITS-OWN-WRITE\n"
                   : " verdict=WRITE-NOT-READABLE-BACK (the address does not "
                     "hold the value: write protected, or reads are a constant "
                     "poison -- ambiguous)\n");
  /* Three answers, because two conflate a real distinction:
   *   1  stable reads AND a write read back  -> memory, a scan means something
   *   0  the reads walk a sequence            -> poison, a scan means nothing
   *   2  stable reads, write does not land    -> a constant poison and a
   *      write-protected page look identical here, so the scan is reported as
   *      ambiguous rather than as a firmware write. */
  if (!stable) return 0;
  return landed ? 1 : 2;
}

/* ---- the pre-launch channel test + prefill ------------------------------ */

/* Offsets, from docs/nv-gsp-wpr-contract.md sections 3.2 and 3.5:
 *   - 0x01000000 is inside the 1 GiB window nv_pci.c maps, so a probe there
 *     answers "does BAR1 work at all" independently of where the layout sits.
 *   - 0x5F0000000 is 134 MiB below nonWprHeapOffset (0x5F8600000), i.e. inside
 *     the range the contract leaves to the driver, and ~23.7 GiB up the
 *     aperture -- the address class the WPR layout lives in, so a probe there
 *     is the one that licenses reading the FRTS region. */
#define NV_VRAM_PROBE_LOW 0x01000000u
#define NV_VRAM_PROBE_HIGH 0x5F0000000ULL
#define NV_VRAM_PAT 0xA4B1C0DEu

int nv_vram_prefill(struct nv_dev *d, struct nv_vram_win *w, uint64_t vram_off,
                    uint32_t len, uint32_t sentinel, struct nv_vram_scan *pre) {
  if (!d || !w || !pre) return -EINVAL;

  int rc = nv_vram_open(d, vram_off, len, w);
  uart_puts("[NV-BAR1] window vram=");
  uart_print_hex(vram_off);
  uart_puts(" len=");
  uart_print_hex(len);
  uart_puts(" phys=");
  uart_print_hex(w->phys);
  uart_puts(" aperture=");
  uart_print_hex(d->bar1_window_size);
  uart_puts(rc == 0 ? " OPEN\n" : " REFUSED\n");
  if (rc != 0) return rc;

  /* The low probe runs inside the window the driver already holds: reading it
   * cannot hurt, and the word is restored. */
  struct nv_vram_win low;
  int low_ok = -1;
  if (nv_vram_open(d, NV_VRAM_PROBE_LOW, 16, &low) == 0) {
    uint32_t obs = 0;
    low_ok = nv_vram_pattern_probe(&low, 0, NV_VRAM_PAT, &obs);
    uart_puts("[NV-BAR1] probe low off=");
    uart_print_hex(NV_VRAM_PROBE_LOW);
    uart_puts(" read=");
    uart_print_hex(obs);
    uart_puts(low_ok == 1 ? " verdict=PATTERN-LANDED-AND-RESTORED\n"
                          : " verdict=NO-ROUND-TRIP\n");
  } else {
    uart_puts("[NV-BAR1] probe low: window refused\n");
  }

  /* The high probe is the one that matters, and it is judged on its own: a
   * low probe passing while this one fails means the aperture does not cover
   * the WPR, which is exactly the contract's open question 11.  It proves two
   * things at once, because "the pattern came back" is not enough: two words
   * 4 KiB apart must hold *different* values, or the window is aliasing one
   * page over the whole aperture and every read of the FRTS region would be a
   * read of some other page. */
  struct nv_vram_win high;
  int high_ok = -1;
  uint32_t low_ref = 0;
  int low_ref_ok = 0;
  if (low.p) {
    low_ref = nv_vram_rd32(&low, 0);
    low_ref_ok = 1;
  }
  if (nv_vram_open(d, NV_VRAM_PROBE_HIGH, 0x2000, &high) == 0) {
    const uint32_t p0 = NV_VRAM_PAT ^ 0x5A5A0000u;
    const uint32_t p1 = p0 ^ 0x0000FFFFu;
    uint32_t was0 = nv_vram_rd32(&high, 0), was1 = nv_vram_rd32(&high, 0x1000);
    nv_vram_wr32(&high, 0, p0);
    nv_vram_wr32(&high, 0x1000, p1);
    uint32_t got0 = nv_vram_rd32(&high, 0), got1 = nv_vram_rd32(&high, 0x1000);
    nv_vram_wr32(&high, 0, was0);
    nv_vram_wr32(&high, 0x1000, was1);
    int restored = (nv_vram_rd32(&high, 0) == was0) &&
                   (nv_vram_rd32(&high, 0x1000) == was1);
    high_ok = (got0 == p0 && got1 == p1 && restored) ? 1 : 0;
    uart_puts("[NV-BAR1] probe high off=");
    uart_print_hex(NV_VRAM_PROBE_HIGH);
    uart_puts(" w0=");
    uart_print_hex(got0);
    uart_puts(" w1000=");
    uart_print_hex(got1);
    uart_puts(" restored=");
    print_int(restored);
    uart_puts(high_ok == 1
                  ? " verdict=PATTERN-LANDED, PAGES INDEPENDENT (BAR1 decodes "
                    "1:1 at 23.7 GiB)\n"
                  : " verdict=NO-ROUND-TRIP or ALIASED (the aperture does not "
                    "decode this offset: a zero region further up is NOT "
                    "evidence)\n");
    /* Third control: writing 23.7 GiB up the aperture must not disturb the
     * low page.  If it does, the high window wraps onto low VRAM and every
     * offset above it means something else than the layout thinks. */
    if (low_ref_ok) {
      uint32_t low_now = nv_vram_rd32(&low, 0);
      uart_puts("[NV-BAR1] alias check low@");
      uart_print_hex(NV_VRAM_PROBE_LOW);
      uart_puts(" was=");
      uart_print_hex(low_ref);
      uart_puts(" after-high-write=");
      uart_print_hex(low_now);
      uart_puts(low_now == low_ref ? " verdict=NO-ALIAS\n"
                                   : " verdict=ALIASED (the high window and "
                                     "the low page are the same memory)\n");
      if (low_now != low_ref) high_ok = 0;
    }
  } else {
    uart_puts("[NV-BAR1] probe high: window refused (no VRAM access at 23.7 "
              "GiB)\n");
  }

  /* Prefill and read back.  This is the negative control for the whole run:
   * the words FWSEC would write must be the host's pattern first. */
  nv_vram_fill(w, 0, len / 4u, sentinel);
  nv_vram_scan_window(w, 0, len / 4u, sentinel, pre);
  uart_puts("[NV-BAR1] prefill sentinel=");
  uart_print_hex(sentinel);
  uart_puts(" words=");
  uart_print_hex(pre->words);
  uart_puts(" stuck=");
  uart_print_hex(pre->sentinel);
  uart_puts(" other=");
  uart_print_hex(pre->other);
  uart_puts(" sum=");
  uart_print_hex(pre->sum);
  uart_puts(pre->sentinel == pre->words && pre->other == 0
                ? " verdict=PREFILL-STUCK (a later non-sentinel word is a "
                  "firmware write, and 'none' is an observation)\n"
                : " verdict=PREFILL-DID-NOT-STICK (this region cannot report a "
                  "write in this run; do not read the post-scan)\n");

  if (pre->sentinel != pre->words || pre->other != 0) return -EIO;
  return 0;
}

#endif /* __x86_64__ */
