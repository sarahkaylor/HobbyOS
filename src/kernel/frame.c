#include "frame.h"
#include "lock.h"
#include "blk_cache.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void print_int(int val);
extern void uart_print_hex(uint64_t v);


/* GB-097 renamed the x64 raw sink to panic_* (non-returning paths only) and
 * kept only the lock-free single-character writer for returning diagnostics:
 * uart_putc_dbg.  These two helpers are the L60/L75 tripwire's row printer --
 * it runs on a returning path, so it must NOT use the panic sink. */
extern void uart_putc_dbg(char c);

static void frame_raw_puts(const char *s) {
  while (s && *s) uart_putc_dbg(*s++);
}

static void frame_raw_hex(uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  frame_raw_puts("0x");
  for (int sh = 60; sh >= 0; sh -= 4) uart_putc_dbg(hx[(v >> sh) & 0xF]);
}

/* Pool extents, per arch (design section 2.1).  The bitmap lays the
   extents end to end: frame index 0 is the first frame of extent 0.

   L75: THE POOL MUST BE RAM AND ONLY RAM.  The x64 extents used to be
   [0x80000000, 0x180000000), i.e. they started at 2 GiB -- which on this
   machine (`-M q35 -m 12288M`) is the base of the 32-bit PCI MMIO hole, not
   RAM.  QEMU's memory map is 0x100000-0x7FFFFFFF (RAM) then
   0x100000000-0x37FFFFFFF (RAM), so [0x80000000, 0x100000000) is MMIO -- and
   the NVMe BAR (BAR0) is placed at its base.  The allocator's forward walk
   therefore handed out frames that are the controller's register file:
   a page-cache fill that memcpy'd a 4 KiB file page into such a frame wrote
   the page straight into AQA/ASQ/ACQ/CC (resetting the controller) and read
   back register state instead of file bytes (the "zero"/"torn" pages of
   L65-L73).  Extent 1 is now 4-8 GiB, all of it RAM and all of it
   identity-mapped by PD4..PD7.  frame_init() proves each extent with a
   write/read-back probe and refuses an extent that is not RAM. */
/* P1 (S3b): the extents are a table, built at frame_init: ext0 and ext1 as
   before, then (x86_64) one extent per RAM range above 8 GiB that the firmware
   memory map reported and mmu.c mapped into the direct map (kmem_ram_hi_range),
   rounded inward to whole 32 MiB blocks.  The bitmap is static and sized for
   the direct map's ceiling (DMAP_RAM_MAX); only the live prefix is used.
   Frame indices run through the extents in table order. */
#define FRAME_EXT0_FRAMES ((FRAME_EXT0_TOP - FRAME_EXT0_BASE) / FRAME_SIZE)
#ifdef __x86_64__
#define FRAME_EXT_MAX 18 /* ext0, ext1, up to 16 ranges above 8 GiB */
#define FRAME_POOL_MAX_FRAMES                                         \
  (((FRAME_EXT0_TOP - FRAME_EXT0_BASE) + (DMAP_RAM_MAX - FRAME_EXT1_BASE)) / \
   FRAME_SIZE)
#else
#define FRAME_EXT_MAX 1
#define FRAME_POOL_MAX_FRAMES (FRAME_EXT0_FRAMES)
#endif
#define FRAME_WORDS_MAX ((int)(FRAME_POOL_MAX_FRAMES / 64))

static struct { uint64_t base, top; uint32_t first; } fext[FRAME_EXT_MAX];
static int fext_n;
static uint32_t frame_total_n; /* live pool size in frames */
static int frame_words_n;
static int block_count_n;
#define FRAME_TOTAL_FRAMES ((uint64_t)frame_total_n)
#define FRAME_WORDS frame_words_n
#define BLOCK_COUNT block_count_n
static uint64_t frame_hi_allocs_n; /* frames handed out above 8 GiB (S3b) */

static uint64_t frame_bits[FRAME_WORDS_MAX];
/* M6/L60 tripwire (see frame_watch below): declared here because frame_free()
 * must be able to arm/observe them before their accessor functions appear. */
static uint64_t frame_watch_phys;
static int frame_watch_hits;
static uint32_t frame_hint; /* rotating alloc hint: word index */
static int frame_dfree_n;   /* C1-k6: frees of an already-free frame */
static int frame_used;      /* count of set bits */
static int frame_high;      /* high-water mark of frame_used */
static uint32_t frame_hole_allocs_n; /* L75: frames handed out in the PCI hole */
int frame_ext1_is_ram;             /* L75: extent 1 passed the RAM probe */
static int block_used;      /* blocks currently claimed as whole units */
static spinlock_t frame_lock;

/* Frame index -> physical address. */
static uint64_t frame_phys_of(int idx) {
  for (int e = fext_n - 1; e > 0; e--)
    if ((uint32_t)idx >= fext[e].first)
      return fext[e].base + (uint64_t)((uint32_t)idx - fext[e].first) * FRAME_SIZE;
  return fext[0].base + (uint64_t)idx * FRAME_SIZE;
}

/* Physical address -> frame index.  *ok is set to 0 when the address is
   outside every pool extent (or unaligned). */
static int frame_index_of(uint64_t phys, int *ok) {
  *ok = 1;
  if (phys & (FRAME_SIZE - 1))
    goto bad;
  for (int e = 0; e < fext_n; e++)
    if (phys >= fext[e].base && phys < fext[e].top)
      return (int)(fext[e].first + (uint32_t)((phys - fext[e].base) / FRAME_SIZE));
bad:
  *ok = 0;
  return -1;
}

static int popcount64(uint64_t v) {
  int n = 0;
  while (v) {
    v &= v - 1;
    n++;
  }
  return n;
}

/* Reserve [lo, hi) (physical; rounded outward to whole 32 MiB blocks) so
   the pool never hands out frames over it.  Boot-time use: the kernel
   image must be reserved -- the allocation hint walks forward with churn
   (contig blocks set it past each run), and once it crossed into the
   image the allocator handed out frames over live kernel memory and
   frame_alloc_zeroed wiped them (exception vector table + text), an
   unrecoverable freeze.

   Blocks are always reserved as whole units (all frames marked + the
   block claimed): the legacy block layer partitions blocks into free vs
   claimed (phys_block_free_count + blocks_used == block_count), so a
   partially-reserved unclaimed block would break that partition.
   Idempotent: re-reserving an already-claimed block is a no-op.  Must
   be called before any allocation (boot). */
void frame_reserve_range(uint64_t lo, uint64_t hi) {
  if (hi <= lo)
    return;
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  uint64_t a_lo = lo & ~((uint64_t)FRAME_SIZE - 1);
  uint64_t a_hi = (hi + FRAME_SIZE - 1) & ~((uint64_t)FRAME_SIZE - 1);
  int ok0 = 0;
  int ok1 = 0;
  int idx_lo = frame_index_of(a_lo, &ok0);
  int idx_hi = frame_index_of(a_hi - FRAME_SIZE, &ok1);
  if (!ok0 || !ok1) {
    /* Outside every pool extent on this arch: nothing to reserve. */
    spinlock_release_irqrestore(&frame_lock, flags);
    return;
  }
  int fb0 = (idx_lo / FRAME_BLOCK_FRAMES) * FRAME_BLOCK_FRAMES;
  int fb1 = ((idx_hi / FRAME_BLOCK_FRAMES) + 1) * FRAME_BLOCK_FRAMES;
  for (int bk = fb0; bk < fb1; bk += FRAME_BLOCK_FRAMES) {
    int whole = 1;
    for (int i = bk; i < bk + FRAME_BLOCK_FRAMES; i++) {
      if (!(frame_bits[i / 64] & (1ULL << (i % 64)))) {
        whole = 0;
        break;
      }
    }
    if (!whole)
      block_used++; /* newly a whole unit (idempotent otherwise) */
    for (int i = bk; i < bk + FRAME_BLOCK_FRAMES; i++) {
      uint32_t w = (uint32_t)i / 64;
      uint32_t b = (uint32_t)i % 64;
      if (frame_bits[w] & (1ULL << b))
        continue;
      frame_bits[w] |= (1ULL << b);
      frame_used++;
    }
  }
  if (frame_used > frame_high)
    frame_high = frame_used;
  spinlock_release_irqrestore(&frame_lock, flags);
}

static void frame_ext_add(uint64_t base, uint64_t top) {
  fext[fext_n].base = base;
  fext[fext_n].top = top;
  fext[fext_n].first = frame_total_n;
  fext_n++;
  frame_total_n += (uint32_t)((top - base) / FRAME_SIZE);
}

void frame_init(void) {
  spinlock_init(&frame_lock);
  fext_n = 0;
  frame_total_n = 0;
  frame_ext_add(FRAME_EXT0_BASE, FRAME_EXT0_TOP);
#ifdef __x86_64__
  frame_ext_add(FRAME_EXT1_BASE, FRAME_EXT1_TOP);
  /* P1 (S3b): RAM above 8 GiB from the firmware memory map, exactly the
     ranges mmu.c mapped into the direct map, rounded inward to whole 32 MiB
     blocks (the block layer's unit).  L75 lesson (the pool once handed out
     the NVMe BAR as RAM): an extent is accepted only if the map calls ALL of
     it RAM and three write/read-back probes through the direct map agree;
     it can never touch the 32-bit PCI hole (it starts above 8 GiB). */
  {
    extern int kmem_ram_hi_range(int i, uint64_t *lo, uint64_t *hi);
    extern int kmem_map_is_ram(uint64_t lo, uint64_t hi);
    extern uint64_t frame_ram_probe(uint64_t phys);
    for (int i = 0; fext_n < FRAME_EXT_MAX; i++) {
      uint64_t lo, hi;
      if (kmem_ram_hi_range(i, &lo, &hi) != 0)
        break;
      lo = (lo + FRAME_BLOCK_SIZE - 1) & ~((uint64_t)FRAME_BLOCK_SIZE - 1);
      hi &= ~((uint64_t)FRAME_BLOCK_SIZE - 1);
      uint64_t room = (FRAME_POOL_MAX_FRAMES - frame_total_n) * FRAME_SIZE;
      if (hi > lo && hi - lo > room)
        hi = lo + (room & ~((uint64_t)FRAME_BLOCK_SIZE - 1));
      if (hi <= lo || lo < 0x200000000ULL)
        continue;
      uint64_t mid = ((lo + hi) / 2) & ~0x1FFFFFULL;
      int ram = kmem_map_is_ram(lo, hi) == 1 && frame_ram_probe(lo) &&
                frame_ram_probe(mid) && frame_ram_probe(hi - FRAME_SIZE);
      uart_puts("[FRAME] hi ext=0x");
      uart_print_hex(lo);
      uart_puts("-0x");
      uart_print_hex(hi);
      uart_puts(ram ? " ram=1\n" : " ram=0 -- refused (not RAM per map/probe)\n");
      if (ram)
        frame_ext_add(lo, hi);
    }
  }
#endif
  frame_words_n = (int)(frame_total_n / 64);
  block_count_n = (int)(frame_total_n / FRAME_BLOCK_FRAMES);
  for (int i = 0; i < FRAME_WORDS; i++)
    frame_bits[i] = 0;
  frame_hint = 0;
  frame_used = 0;
  frame_high = 0;
  block_used = 0;

  /* L75: prove each extent is RAM before handing it out.  A frame must
     accept a store and give it back; an MMIO hole answers with a device
     register (or all-ones).  The old extent 1 base (0x80000000) is the NVMe
     BAR on this machine, so this probe is also the tripwire that would have
     named the L65-L73 block-layer bug at boot: it writes 8 bytes at 0x14 of
     the frame -- the controller's CC register -- and reads back CAP/CC
     instead of its own pattern.  It runs before anything is allocated, so it
     marks no bits; on success the frame is simply left zeroed. */
  {
    extern uint64_t frame_ram_probe(uint64_t phys);
    /* Three probes per extent (base / middle / last frame): a device scratch
       register can echo one pattern, not three addresses across GiB. */
    uint64_t e0_mid = (FRAME_EXT0_BASE + FRAME_EXT0_TOP) / 2;
    int e0 = (int)(frame_ram_probe(FRAME_EXT0_BASE) &
                   frame_ram_probe(e0_mid & ~0x1FFFFFULL) &
                   frame_ram_probe(FRAME_EXT0_TOP - FRAME_SIZE));
    uart_puts("[FRAME] ram ext0=");
    print_int(e0);
    if (!e0) {
      uart_puts(" ext0=NOT-RAM -- reserved");
      frame_reserve_range(FRAME_EXT0_BASE, FRAME_EXT0_TOP);
    }
#ifdef __x86_64__
    uint64_t e1_mid = (FRAME_EXT1_BASE + FRAME_EXT1_TOP) / 2;
    int e1 = (int)(frame_ram_probe(FRAME_EXT1_BASE) &
                   frame_ram_probe(e1_mid & ~0x1FFFFFULL) &
                   frame_ram_probe(FRAME_EXT1_TOP - FRAME_SIZE));
    frame_ext1_is_ram = e1;
    uart_puts(" ext1=");
    print_int(e1);
    /* Not RAM: refuse the whole extent rather than hand out device pages. */
    if (!e1) {
      uart_puts(" ext1=NOT-RAM(that is a device window, not memory) -- reserved");
      frame_reserve_range(FRAME_EXT1_BASE, FRAME_EXT1_TOP);
    }
#endif
    uart_puts("\n");
  }

  /* Keep the kernel image out of the pool: _start..__stack_top covers
     text/rodata/data/bss and the boot stacks; +64 KiB guard.  Without
     this the hint walk reaches the image after ~1.88M frames of churn
     (soak: fork seq ~5900) and zeroes live kernel pages. */
  {
    extern char _start[];
    extern char __stack_top[];
    uint64_t img_lo = kimg_pa(_start);
    uint64_t img_hi = kimg_pa(__stack_top) + 0x10000;
    frame_reserve_range(img_lo, img_hi);
  }

  /* P1 (S3b): start the rotating hint in RAM above 8 GiB when there is any.
     Those frames have no identity alias in ANY root (kernel root included),
     so every frame handed out first is also a tripwire for a leftover
     physical-address-as-pointer, and the guest provably uses its extra RAM.
     The v1 block layer still scans from block 0 (ext0) upward. */
  if (fext_n > 2)
    frame_hint = fext[2].first / 64;

  /* Boot log line (design section 2.1): the pool constants are VMM
     constants; this line is their validation in every boot's log.
     reserved= is the kernel-image reservation above (non-zero). */
  uart_puts("[FRAME] frames=");
  print_int((int)FRAME_TOTAL_FRAMES);
  uart_puts(" total_mib=");
  print_int((int)((uint64_t)FRAME_TOTAL_FRAMES * FRAME_SIZE / 0x100000));
  uart_puts(" ext0=0x");
  uart_print_hex(FRAME_EXT0_BASE);
  uart_puts("-0x");
  uart_print_hex(FRAME_EXT0_TOP);
#ifdef __x86_64__
  uart_puts(" ext1=0x");
  uart_print_hex(FRAME_EXT1_BASE);
  uart_puts("-0x");
  uart_print_hex(FRAME_EXT1_TOP);
  uart_puts(" extents=");
  print_int(fext_n);
  uart_puts(" hi_mib=");
  {
    uint64_t hb = 0;
    for (int e = 2; e < fext_n; e++)
      hb += fext[e].top - fext[e].base;
    print_int((int)(hb >> 20));
  }
#endif
  uart_puts(" reserved=");
  print_int(frame_used);
  uart_puts("\n");
}

/* Write/read-back probe for one frame.  Returns 1 when the address behaves
 * like RAM, 0 when it does not.  Leaves the frame zeroed and unallocated. */
uint64_t frame_ram_probe(uint64_t phys) {
  volatile uint64_t *p = (volatile uint64_t *)phys_to_virt(phys);
  uint64_t a = 0x5A5A5A5A5A5A5A5AULL, b = 0xA5A5A5A5A5A5A5A5ULL;
  int ok;
  p[0] = a;
  ok = (p[0] == a);
  if (ok) {
    p[0] = b;
    ok = (p[0] == b);
  }
  if (ok) {
    p[0] = 0;
    ok = (p[0] == 0);
  }
#ifdef __x86_64__
  if (phys == FRAME_EXT1_BASE)
    frame_ext1_is_ram = ok;
#endif
  return ok ? 1 : 0;
}

uint64_t frame_alloc(void) {
  for (;;) {
    uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
    uint64_t res = 0;
    uint32_t hint = frame_hint;

    /* Find-first-zero-word scan from the rotating hint, wrapping once.
       Sequential allocation (faults, loader, spawns) advances the hint
       word by word, so the scan is amortized O(1) per frame; a nearly
       full pool degrades to a bounded full scan and then reports
       exhaustion with 0. */
    for (int pass = 0; pass < 2 && res == 0; pass++) {
      uint32_t start = (pass == 0) ? hint : 0;
      uint32_t end = (pass == 0) ? FRAME_WORDS : hint;
      for (uint32_t w = start; w < end; w++) {
        uint64_t inv = ~frame_bits[w];
        if (!inv)
          continue;
        int bit = __builtin_ctzll(inv);
        frame_bits[w] |= (1ULL << bit);
        frame_hint = w;
        frame_used++;
        if (frame_used > frame_high)
          frame_high = frame_used;
        res = frame_phys_of((int)(w * 64 + (uint32_t)bit));
        /* L75 tripwire: a frame outside RAM (the 32-bit PCI hole) would be
           a device, not memory.  The extents make this impossible; counting
           it means a future extent edit cannot do it silently. */
        if (res >= 0x80000000ULL && res < 0x100000000ULL)
          frame_hole_allocs_n++;
        if (res >= 0x200000000ULL)
          frame_hi_allocs_n++; /* P1 (S3b): RAM above 8 GiB in use */
        break;
      }
    }
    spinlock_release_irqrestore(&frame_lock, flags);
    if (res)
      return res;

    /* Pool exhausted: discard cached units and retry.  evict takes its
       own locks (never called with frame_lock held); a no-progress evict
       means the cache is empty/not ready — give up.  (blk-cache hooks,
       docs/blk-cache.md section 6.) */
    if (blk_cache_evict_pages(64) == 0)
      return 0;
  }
}

uint64_t frame_alloc_zeroed(void) {
  uint64_t p = frame_alloc();
  if (!p)
    return 0;
  /* Plain 64-bit stores (kernel C is -mgeneral-regs-only: no SIMD). */
  volatile uint64_t *d = (volatile uint64_t *)phys_to_virt(p);
  for (int i = 0; i < FRAME_SIZE / 8; i++)
    d[i] = 0;
  return p;
}

void frame_free(uint64_t phys) {
  int ok = 0;
  int idx = frame_index_of(phys, &ok);
  if (!ok)
    return;
  /* The tripwire fires BEFORE the bit is cleared, so the row names a frame
   * that was still owned at the moment of the free. */
  extern uint64_t frame_watch_peek(void);
  if (frame_watch_phys && phys == frame_watch_phys) {
    extern int frame_watch_bump(void);
    frame_watch_bump();
    static int rows;
    if (rows < 8) {
      rows++;
      frame_raw_puts("[FREEW] phys=");
      frame_raw_hex(phys);
      frame_raw_puts(" caller=");
      frame_raw_hex((uint64_t)(uintptr_t)__builtin_return_address(0));
      frame_raw_puts("\n");
    }
  }
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  uint32_t w = (uint32_t)idx / 64;
  uint32_t bit = (uint32_t)idx % 64;
  int dfree = 0;
  if (frame_bits[w] & (1ULL << bit)) {
    frame_bits[w] &= ~(1ULL << bit);
    frame_used--;
  } else {
    dfree = ++frame_dfree_n;
  }
  spinlock_release_irqrestore(&frame_lock, flags);
  if (dfree) {
    /* C1-k6 (docs/cp-k6.md §7; C1-pe's open item): a free of a frame that is
     * already free.  It is still ignored (nothing else changes), but it is no
     * longer silent: a double free whose frame was handed out again in
     * between gives one frame two owners -- the shape C1-pe could not rule
     * out for the single-threaded wineserver's c0000130 read.  Bounded: the
     * first 16, then every 1024th, each with the running count. */
    if (dfree <= 16 || (dfree & 1023) == 0) {
      frame_raw_puts("[FRAME-DFREE] n=");
      frame_raw_hex((uint64_t)dfree);
      frame_raw_puts(" phys=");
      frame_raw_hex(phys);
      frame_raw_puts(" caller=");
      frame_raw_hex((uint64_t)(uintptr_t)__builtin_return_address(0));
      frame_raw_puts("\n");
    }
  }
}

/* C1-k6: how many already-free frames frame_free() has been handed. */
int frame_dfree_count(void) { return frame_dfree_n; }

uint64_t frame_watch_peek(void) { return frame_watch_phys; }
int frame_watch_bump(void) { return ++frame_watch_hits; }

int frame_is_alloced(uint64_t phys) {
  int ok = 0;
  int idx = frame_index_of(phys, &ok);
  if (!ok)
    return 0;
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int used = (frame_bits[(uint32_t)idx / 64] & (1ULL << ((uint32_t)idx % 64))) != 0;
  spinlock_release_irqrestore(&frame_lock, flags);
  return used;
}

/* M6/L60: a one-frame tripwire.  `[UDX]` shows an executable text page whose
 * leaf is present, read-only, NX-clear, owned (alloc=1) and yet reads back as
 * zeros; the file it was copied from holds code.  Either something else wrote
 * that physical frame after we published it, or it went back to the allocator
 * and was handed out again.  Arming the frame at map time and printing every
 * frame_free() that names it (with the caller's return address, so nm on
 * hobbyos.elf names the site) separates those two with one row. */
void frame_watch(uint64_t phys) {
  frame_watch_phys = phys;
  frame_watch_hits = 0;
}
int frame_watch_count(void) { return frame_watch_hits; }

uint64_t frame_alloc_contig(int n) {
  if (n <= 0 || (uint64_t)n > FRAME_TOTAL_FRAMES)
    return 0;
  for (;;) {
    uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
    uint32_t total = (uint32_t)FRAME_TOTAL_FRAMES;
    uint32_t run_start = 0;
    uint32_t run = 0;
    uint64_t res = 0;

    /* Two passes: [hint, end) then [0, hint).  Bit-by-bit with a running
       counter; n is small (<= a few hundred) so the worst-case cost is
       bounded by the pool size once, same class as the exhaustion scan. */
    for (int pass = 0; pass < 2 && !res; pass++) {
      uint32_t b = (pass == 0) ? frame_hint * 64 : 0;
      uint32_t e = (pass == 0) ? total : frame_hint * 64;
      if (b >= e)
        continue;
      run = 0;
      for (uint32_t i = b; i < e; i++) {
        if (frame_bits[i / 64] & (1ULL << (i % 64))) {
          run = 0;
          continue;
        }
        /* P1 (S3b): a run must be physically contiguous, so it may not
           cross into the next extent unless the two extents touch. */
        if (run) {
          for (int x = 1; x < fext_n; x++)
            if (fext[x].first == i && fext[x].base != fext[x - 1].top) {
              run = 0;
              break;
            }
        }
        if (run == 0)
          run_start = i;
        if (++run == (uint32_t)n) {
          for (uint32_t j = run_start; j < run_start + (uint32_t)n; j++)
            frame_bits[j / 64] |= (1ULL << (j % 64));
          frame_hint = (run_start + (uint32_t)n) / 64;
          if (frame_hint >= (uint32_t)FRAME_WORDS)
            frame_hint = 0;
          frame_used += (uint32_t)n;
          if (frame_used > frame_high)
            frame_high = frame_used;
          res = frame_phys_of((int)run_start);
          if (res >= 0x200000000ULL)
            frame_hi_allocs_n += (uint32_t)n;
          break;
        }
      }
    }
    spinlock_release_irqrestore(&frame_lock, flags);
    if (res)
      return res;

    /* Pool can't provide a contiguous run: evict up to `n` cached units
       (frames go back to the pool) and retry; give up when no progress
       (cache empty/not ready).  No cache lock is held here (frame_lock was
       released above), so the eviction cannot self-deadlock. */
    if (blk_cache_evict_pages((uint64_t)n) == 0)
      return 0;
  }
}

int frame_total_count(void) { return (int)FRAME_TOTAL_FRAMES; }

int frame_used_count(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int u = frame_used;
  spinlock_release_irqrestore(&frame_lock, flags);
  return u;
}

int frame_free_count(void) { return frame_total_count() - frame_used_count(); }

int frame_high_water(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int h = frame_high;
  spinlock_release_irqrestore(&frame_lock, flags);
  return h;
}

/* L75: the pool's extents, for the invariant test (0 on success). */
int frame_extent_bounds(int idx, uint64_t *lo, uint64_t *hi) {
  if (!lo || !hi)
    return -1;
  if (idx < 0 || idx >= fext_n)
    return -1;
  *lo = fext[idx].base;
  *hi = fext[idx].top;
  return 0;
}

/* P1 (S3b): frames ever handed out above 8 GiB (single + contiguous). */
uint64_t frame_hi_allocs(void) { return frame_hi_allocs_n; }

/* Is `phys` inside any pool extent?  (The BAR-in-pool tripwire.) */
int frame_phys_in_pool(uint64_t phys) {
  for (int e = 0; e < fext_n; e++)
    if (phys >= fext[e].base && phys < fext[e].top)
      return 1;
  return 0;
}

/* L75: frames handed out outside RAM (must stay 0; see FRAME_EXT1_BASE). */
int frame_hole_allocs(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int h = (int)frame_hole_allocs_n;
  spinlock_release_irqrestore(&frame_lock, flags);
  return h;
}

/* ---------------------------------------------------------------------
 * 32 MiB block layer (v1 process regions) on the same bitmap.
 * ------------------------------------------------------------------- */

int frame_block_count(void) { return BLOCK_COUNT; }

uint64_t frame_block_base_phys(int idx) {
  if (idx < 0 || idx >= BLOCK_COUNT)
    return 0;
  return frame_phys_of(idx * FRAME_BLOCK_FRAMES);
}

/* Caller holds frame_lock: is every bit of the block clear? */
static int block_is_free_locked(int idx) {
  uint32_t w0 = ((uint32_t)idx * FRAME_BLOCK_FRAMES) / 64;
  for (uint32_t i = 0; i < FRAME_BLOCK_FRAMES / 64; i++) {
    if (frame_bits[w0 + i])
      return 0;
  }
  return 1;
}

int frame_block_is_free(int idx) {
  if (idx < 0 || idx >= BLOCK_COUNT)
    return 0;
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int free_ = block_is_free_locked(idx);
  spinlock_release_irqrestore(&frame_lock, flags);
  return free_;
}

int frame_block_alloc_first_free(void) {
  for (;;) {
    uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
    int found = -1;
    for (int idx = 0; idx < BLOCK_COUNT; idx++) {
      if (!block_is_free_locked(idx))
        continue;
      uint32_t w0 = ((uint32_t)idx * FRAME_BLOCK_FRAMES) / 64;
      for (uint32_t i = 0; i < FRAME_BLOCK_FRAMES / 64; i++) {
        uint64_t prev = frame_bits[w0 + i];
        frame_used += 64 - popcount64(prev);
        frame_bits[w0 + i] = ~(uint64_t)0;
      }
      if (frame_used > frame_high)
        frame_high = frame_used;
      block_used++;
      found = idx;
      break;
    }
    spinlock_release_irqrestore(&frame_lock, flags);
    if (found >= 0)
      return found;

    /* No fully-free block: cached units squat in otherwise-partially-used
       blocks (docs/blk-cache.md section 6).  Evict batches until a block
       comes free or the cache empties.  This runs under proc_lock on the
       caller's side, so the batch loop stays tight with an early exit. */
    if (blk_cache_evict_pages(FRAME_BLOCK_FRAMES) == 0)
      return -1;
  }
}

void frame_block_free(int idx) {
  if (idx < 0 || idx >= BLOCK_COUNT)
    return;
  uint32_t w0 = ((uint32_t)idx * FRAME_BLOCK_FRAMES) / 64;
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  for (uint32_t i = 0; i < FRAME_BLOCK_FRAMES / 64; i++) {
    uint64_t prev = frame_bits[w0 + i];
    if (prev) {
      frame_used -= popcount64(prev);
      frame_bits[w0 + i] = 0;
    }
  }
  if (block_used > 0)
    block_used--;
  spinlock_release_irqrestore(&frame_lock, flags);
}

int frame_phys_block_free_count(void) {
  int n = 0;
  for (int idx = 0; idx < BLOCK_COUNT; idx++) {
    if (frame_block_is_free(idx))
      n++;
  }
  return n;
}

int frame_blocks_used_count(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  int b = block_used;
  spinlock_release_irqrestore(&frame_lock, flags);
  return b;
}

/* ---------------------------------------------------------------------
 * blk-cache reclaim helpers (docs/blk-cache.md section 6).  Evict cached
 * units until the requested headroom exists or the cache is empty.  Never
 * called with frame_lock held (eviction takes fill/bucket locks).  Both
 * are hard no-ops (return 0) before blk_cache_init() — blk_cache_evict
 * gates on cache readiness.
 * ------------------------------------------------------------------- */

int frame_reclaim_to_blocks(int want) {
  if (want <= 0)
    return 1;
  while (frame_phys_block_free_count() < want) {
    if (blk_cache_evict_pages(FRAME_BLOCK_FRAMES) == 0)
      return 0;
  }
  return 1;
}

int frame_reclaim_to_free_frames(int want) {
  if (want <= 0)
    return 1;
  while (frame_free_count() < want) {
    if (blk_cache_evict_pages(1024) == 0)
      return 0;
  }
  return 1;
}

#ifdef KERNEL_MODE_UNIT_TEST
/* Test-only: snapshot / fake-full / restore for the exhaustion path. */
static uint64_t frame_test_snapshot[FRAME_WORDS_MAX];
static int frame_test_snap_used;
static int frame_test_snap_hint;

void frame_test_save_all(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  for (int i = 0; i < FRAME_WORDS; i++)
    frame_test_snapshot[i] = frame_bits[i];
  frame_test_snap_used = frame_used;
  frame_test_snap_hint = (int)frame_hint;
  spinlock_release_irqrestore(&frame_lock, flags);
}

void frame_test_pretend_full(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  for (int i = 0; i < FRAME_WORDS; i++)
    frame_bits[i] = ~(uint64_t)0;
  frame_used = (int)FRAME_TOTAL_FRAMES;
  spinlock_release_irqrestore(&frame_lock, flags);
}

void frame_test_restore_all(void) {
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  for (int i = 0; i < FRAME_WORDS; i++)
    frame_bits[i] = frame_test_snapshot[i];
  frame_used = frame_test_snap_used;
  frame_hint = (uint32_t)frame_test_snap_hint;
  spinlock_release_irqrestore(&frame_lock, flags);
}
#endif /* KERNEL_MODE_UNIT_TEST */
