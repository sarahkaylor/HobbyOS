#include "frame.h"
#include "lock.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void print_int(int val);

/* Pool extents, per arch (design section 2.1).  The bitmap lays the
   extents end to end: frame index 0 is the first frame of extent 0. */
#ifdef __x86_64__
#define FRAME_EXT0_BASE 0x20000000ULL
#define FRAME_EXT0_TOP  0x70000000ULL
#define FRAME_EXT1_BASE 0x80000000ULL
#define FRAME_EXT1_TOP  0x180000000ULL
#define FRAME_EXT0_FRAMES ((FRAME_EXT0_TOP - FRAME_EXT0_BASE) / FRAME_SIZE)
#define FRAME_TOTAL_FRAMES \
  ((FRAME_EXT0_FRAMES) + (FRAME_EXT1_TOP - FRAME_EXT1_BASE) / FRAME_SIZE)
#else
#define FRAME_EXT0_BASE 0x70000000ULL
#define FRAME_EXT0_TOP  0x240000000ULL
#define FRAME_EXT0_FRAMES ((FRAME_EXT0_TOP - FRAME_EXT0_BASE) / FRAME_SIZE)
#define FRAME_TOTAL_FRAMES (FRAME_EXT0_FRAMES)
#endif

#define FRAME_WORDS ((int)(FRAME_TOTAL_FRAMES / 64))
#define BLOCK_COUNT ((int)(FRAME_TOTAL_FRAMES / FRAME_BLOCK_FRAMES))

static uint64_t frame_bits[FRAME_WORDS];
static uint32_t frame_hint; /* rotating alloc hint: word index */
static int frame_used;      /* count of set bits */
static int frame_high;      /* high-water mark of frame_used */
static int block_used;      /* blocks currently claimed as whole units */
static spinlock_t frame_lock;

/* Frame index -> physical address (kernel VA == phys, both arches). */
static uint64_t frame_phys_of(int idx) {
#ifdef __x86_64__
  if (idx < (int)FRAME_EXT0_FRAMES)
    return FRAME_EXT0_BASE + (uint64_t)idx * FRAME_SIZE;
  return FRAME_EXT1_BASE +
         (uint64_t)(idx - (int)FRAME_EXT0_FRAMES) * FRAME_SIZE;
#else
  return FRAME_EXT0_BASE + (uint64_t)idx * FRAME_SIZE;
#endif
}

/* Physical address -> frame index.  *ok is set to 0 when the address is
   outside every pool extent (or unaligned). */
static int frame_index_of(uint64_t phys, int *ok) {
  *ok = 1;
  if (phys & (FRAME_SIZE - 1))
    goto bad;
  if (phys >= FRAME_EXT0_BASE && phys < FRAME_EXT0_TOP)
    return (int)((phys - FRAME_EXT0_BASE) / FRAME_SIZE);
#ifdef __x86_64__
  if (phys >= FRAME_EXT1_BASE && phys < FRAME_EXT1_TOP)
    return (int)FRAME_EXT0_FRAMES + (int)((phys - FRAME_EXT1_BASE) / FRAME_SIZE);
#endif
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

void frame_init(void) {
  spinlock_init(&frame_lock);
  for (int i = 0; i < FRAME_WORDS; i++)
    frame_bits[i] = 0;
  frame_hint = 0;
  frame_used = 0;
  frame_high = 0;
  block_used = 0;

  /* Boot log line (design section 2.1): the pool constants are VMM
     constants; this line is their validation in every boot's log. */
  uart_puts("[FRAME] frames=");
  print_int((int)FRAME_TOTAL_FRAMES);
  uart_puts(" total_mib=");
  print_int((int)((uint64_t)FRAME_TOTAL_FRAMES * FRAME_SIZE / 0x100000));
  uart_puts("\n");
}

uint64_t frame_alloc(void) {
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
      break;
    }
  }
  spinlock_release_irqrestore(&frame_lock, flags);
  return res;
}

uint64_t frame_alloc_zeroed(void) {
  uint64_t p = frame_alloc();
  if (!p)
    return 0;
  /* Plain 64-bit stores (kernel C is -mgeneral-regs-only: no SIMD). */
  volatile uint64_t *d = (volatile uint64_t *)p;
  for (int i = 0; i < FRAME_SIZE / 8; i++)
    d[i] = 0;
  return p;
}

void frame_free(uint64_t phys) {
  int ok = 0;
  int idx = frame_index_of(phys, &ok);
  if (!ok)
    return;
  uint64_t flags = spinlock_acquire_irqsave(&frame_lock);
  uint32_t w = (uint32_t)idx / 64;
  uint32_t bit = (uint32_t)idx % 64;
  if (frame_bits[w] & (1ULL << bit)) {
    frame_bits[w] &= ~(1ULL << bit);
    frame_used--;
  }
  spinlock_release_irqrestore(&frame_lock, flags);
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
  return found;
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

#ifdef KERNEL_MODE_UNIT_TEST
/* Test-only: snapshot / fake-full / restore for the exhaustion path. */
static uint64_t frame_test_snapshot[FRAME_WORDS];
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
