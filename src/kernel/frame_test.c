#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "frame.h"

extern void uart_puts(const char *s);

/* Expected pool sizes (design section 2.1).  ARM: [0x70000000,
   0x240000000); x64: [0x20000000, 0x70000000) + [0x80000000,
   0x180000000).  These are the VMM constants the boot log line
   validates; a mismatch here means a constant and the bitmap drifted. */
#ifdef __x86_64__
#define EXPECT_FRAMES 1376256
#else
#define EXPECT_FRAMES 1900544
#endif

static void test_frame_counts(void) {
  tests_run++;
  uart_puts("  Running test_frame_counts...\n");
  EXPECT_EQ(frame_total_count(), EXPECT_FRAMES);
  EXPECT_EQ(frame_total_count(), frame_free_count() + frame_used_count());
  EXPECT_EQ((frame_high_water() >= frame_used_count()), 1);
  EXPECT_EQ((frame_block_count() == frame_total_count() / FRAME_BLOCK_FRAMES), 1);
}

static void test_frame_alloc_free_delta(void) {
  tests_run++;
  uart_puts("  Running test_frame_alloc_free_delta...\n");

  int free0 = frame_free_count();
  uint64_t f = frame_alloc();
  EXPECT_EQ((f != 0), 1);
  EXPECT_EQ((f & (FRAME_SIZE - 1)), 0);
  /* In-pool: between the lowest possible base (0x20000000 x64) and the
     highest possible top (0x240000000 ARM). */
  EXPECT_EQ((f >= 0x20000000ULL && f < 0x240000000ULL), 1);
  EXPECT_EQ(frame_free_count(), free0 - 1);
  EXPECT_EQ(frame_used_count() >= 1, 1);

  frame_free(f);
  EXPECT_EQ(frame_free_count(), free0);

  /* Freeing again is idempotent (no double-decrement). */
  frame_free(f);
  EXPECT_EQ(frame_free_count(), free0);

  /* Out-of-pool / misaligned frees are ignored. */
  frame_free(0x1000);
  frame_free(f + 1);
  EXPECT_EQ(frame_free_count(), free0);
}

static void test_frame_reuse(void) {
  tests_run++;
  uart_puts("  Running test_frame_reuse...\n");

  /* The rotating hint + find-first-zero word scan makes this exact:
     the frame just freed is the first zero bit the next scan sees. */
  uint64_t f = frame_alloc();
  EXPECT_EQ((f != 0), 1);
  frame_free(f);
  uint64_t g = frame_alloc();
  EXPECT_EQ((g == f), 1);
  frame_free(g);
}

static void test_frame_alloc_zeroed(void) {
  tests_run++;
  uart_puts("  Running test_frame_alloc_zeroed...\n");

  uint64_t f = frame_alloc_zeroed();
  EXPECT_EQ((f != 0), 1);
  volatile uint64_t *p = (volatile uint64_t *)f;
  EXPECT_EQ((p[0] == 0), 1);
  EXPECT_EQ((p[FRAME_SIZE / 8 - 1] == 0), 1);

  /* Dirty the frame, free it, and prove the next zeroed alloc of the
     same frame is clean (the demand-frame guarantee). */
  p[0] = 0x1122334455667788ULL;
  p[FRAME_SIZE / 8 - 1] = 0xDEADBEEFCAFEF00DULL;
  frame_free(f);
  uint64_t g = frame_alloc_zeroed();
  EXPECT_EQ((g == f), 1);
  volatile uint64_t *q = (volatile uint64_t *)g;
  EXPECT_EQ((q[0] == 0), 1);
  EXPECT_EQ((q[FRAME_SIZE / 8 - 1] == 0), 1);
  frame_free(g);
}

static void test_frame_block_layer(void) {
  tests_run++;
  uart_puts("  Running test_frame_block_layer...\n");

  int blocks = frame_block_count();
  EXPECT_EQ((blocks >= 40), 1);
  EXPECT_EQ(frame_phys_block_free_count(), blocks - frame_blocks_used_count());

  /* First-fit handover: the block returned is the lowest free one. */
  int expect_first = -1;
  for (int i = 0; i < blocks; i++) {
    if (frame_block_is_free(i)) {
      expect_first = i;
      break;
    }
  }
  EXPECT_EQ((expect_first >= 0), 1);

  int free0 = frame_phys_block_free_count();
  int used0 = frame_blocks_used_count();
  int frames_free0 = frame_free_count();

  int b = frame_block_alloc_first_free();
  EXPECT_EQ((b == expect_first), 1);
  EXPECT_EQ(frame_block_is_free(b), 0);
  EXPECT_EQ(frame_phys_block_free_count(), free0 - 1);
  EXPECT_EQ(frame_blocks_used_count(), used0 + 1);
  EXPECT_EQ(frame_free_count(), frames_free0 - FRAME_BLOCK_FRAMES);

  /* The block's base is 32 MiB-aligned and in-pool. */
  uint64_t base = frame_block_base_phys(b);
  EXPECT_EQ((base & (FRAME_BLOCK_SIZE - 1)), 0);
  EXPECT_EQ((base >= 0x20000000ULL && base < 0x240000000ULL), 1);

  frame_block_free(b);
  EXPECT_EQ(frame_block_is_free(b), 1);
  EXPECT_EQ(frame_phys_block_free_count(), free0);
  EXPECT_EQ(frame_blocks_used_count(), used0);
  EXPECT_EQ(frame_free_count(), frames_free0);

  /* A second allocation round gets the same block back (first-fit). */
  int b2 = frame_block_alloc_first_free();
  EXPECT_EQ((b2 == b), 1);
  frame_block_free(b2);
  EXPECT_EQ(frame_free_count(), frames_free0);
}

static void test_frame_exhaustion_path(void) {
  tests_run++;
  uart_puts("  Running test_frame_exhaustion_path...\n");

  /* Bounded real churn first: many allocs stay distinct and freeing
     each restores the count exactly. */
  int free0 = frame_free_count();
  uint64_t batch[64];
  for (int i = 0; i < 64; i++) {
    batch[i] = frame_alloc();
    EXPECT_EQ((batch[i] != 0), 1);
  }
  for (int i = 0; i < 64; i++) {
    for (int j = i + 1; j < 64; j++) {
      if (batch[j] == batch[i]) {
        uart_puts("ASSERTION FAILED: duplicate frame\n");
        tests_failed++;
        return;
      }
    }
  }
  EXPECT_EQ(frame_free_count(), free0 - 64);
  for (int i = 0; i < 64; i++)
    frame_free(batch[i]);
  EXPECT_EQ(frame_free_count(), free0);

  /* Exhaustion path: with the pool full, frame_alloc must report 0
     (not spin).  Taking 1.9M real allocs would burn the unit-tier
     budget, so snapshot the bitmap, fake it full, check the return,
     and restore — the scan is exactly the code under test. */
  frame_test_save_all();
  frame_test_pretend_full();
  EXPECT_EQ(frame_free_count(), 0);
  EXPECT_EQ(frame_alloc(), (uint64_t)0);
  EXPECT_EQ(frame_alloc_zeroed(), (uint64_t)0);
  frame_test_restore_all();
  EXPECT_EQ(frame_free_count(), free0);
  EXPECT_EQ(frame_blocks_used_count(), frame_block_count() - frame_phys_block_free_count());
}

void frame_test_suite(void) {
  uart_puts("frame_test_suite:\n");
  test_frame_counts();
  test_frame_alloc_free_delta();
  test_frame_reuse();
  test_frame_alloc_zeroed();
  test_frame_block_layer();
  test_frame_exhaustion_path();
}

#endif // KERNEL_MODE_UNIT_TEST
