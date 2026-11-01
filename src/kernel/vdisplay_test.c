#ifdef KERNEL_MODE_UNIT_TEST

#include "vdisplay.h"
#include "unit_test.h"

/* Kernel in-EL1 coverage of the virtual-display core (icewm.md §9,
 * src/kernel/vdisplay.c).  The unit-test kernel never runs HBHOST, so the
 * table can be reset freely.  The same core is host-tested in
 * src/host/vdisplay_test.c. */

static void test_vdisp_alloc_free(void) {
  uart_puts("  Running test_vdisp_alloc_free...\n");
  tests_run++;
  vdisp_core_reset();
  int marker = 0;
  int a = vdisp_core_alloc(5, &marker);
  int b = vdisp_core_alloc(6, &marker);
  ASSERT(a >= 1 && b >= 1 && a != b);
  uint32_t ga = vdisp_core_gen(a);
  EXPECT_EQ(vdisp_core_valid(a, ga), 1);
  EXPECT_EQ(vdisp_core_owner(b), 6);
  ASSERT(vdisp_core_free(a) == &marker);
  EXPECT_EQ(vdisp_core_valid(a, ga), 0);           /* stale after free */
  int c = vdisp_core_alloc(7, &marker);             /* slot reuse */
  EXPECT_EQ(c, a);
  EXPECT_EQ(vdisp_core_valid(c, ga), 0);           /* old gen never revalidates */
  int n = 2;
  while (vdisp_core_alloc(9, &marker)) n++;
  EXPECT_EQ(n, VDISP_MAX);
  vdisp_core_reset();
}

static void test_vdisp_ring(void) {
  uart_puts("  Running test_vdisp_ring...\n");
  tests_run++;
  vdisp_core_reset();
  int marker = 0;
  int id = vdisp_core_alloc(1, &marker);
  struct vdisp_event ev[3] = {{1, 103, 1}, {1, 103, 0}, {1, 16, 1}};
  EXPECT_EQ(vdisp_core_inject(id, ev, 3), 3);
  struct vdisp_event out[8];
  EXPECT_EQ(vdisp_core_read(id, out, 2, 100), 2);
  EXPECT_EQ(out[0].code, 103);
  EXPECT_EQ((int)out[1].value, 0);
  EXPECT_EQ(vdisp_core_read(id, out, 8, 150), 1);
  EXPECT_EQ(out[0].code, 16);
  EXPECT_EQ(vdisp_core_read(id, out, 8, 160), 0);
  /* Ring full: VDISP_RING-1 accepted, the rest dropped. */
  int acc = 0;
  for (int i = 0; i < VDISP_RING + 10; i++) acc += vdisp_core_inject(id, ev, 1);
  EXPECT_EQ(acc, VDISP_RING - 1);
  struct vdisp_status st;
  vdisp_core_status(id, &st, 200);
  EXPECT_EQ((int)st.queued, VDISP_RING - 1);
  EXPECT_EQ((int)st.reads, 3);
  EXPECT_EQ((int)st.read_age_ms, 40);
  /* An unknown id reads nothing and injects nothing. */
  EXPECT_EQ(vdisp_core_inject(VDISP_MAX + 1, ev, 1), 0);
  EXPECT_EQ(vdisp_core_read(0, out, 8, 0), 0);
  vdisp_core_reset();
}

static void test_vdisp_damage(void) {
  uart_puts("  Running test_vdisp_damage...\n");
  tests_run++;
  vdisp_core_reset();
  int marker = 0;
  int id = vdisp_core_alloc(1, &marker);
  struct vdisp_status st;
  vdisp_core_status(id, &st, 0);
  EXPECT_EQ(st.dw, 0);
  EXPECT_EQ((int)st.read_age_ms, -1);               /* never read */
  vdisp_core_damage(id, 10, 20, 30, 40);
  vdisp_core_damage(id, 5, 50, 10, 30);
  vdisp_core_damage(id, 0, 0, 0, 0);                 /* empty: seq only */
  vdisp_core_mapped(id);
  vdisp_core_status(id, &st, 0);
  EXPECT_EQ((int)st.flush_seq, 3);
  EXPECT_EQ(st.dx, 5);
  EXPECT_EQ(st.dy, 20);
  EXPECT_EQ(st.dw, 35);
  EXPECT_EQ(st.dh, 60);
  EXPECT_EQ((int)st.mapped, 1);
  vdisp_core_status(id, &st, 0);                    /* STATUS clears damage */
  EXPECT_EQ(st.dw, 0);
  EXPECT_EQ((int)st.flush_seq, 3);
  vdisp_core_reset();
}

static void test_vdisp_pace(void) {
  uart_puts("  Running test_vdisp_pace...\n");
  tests_run++;
  vdisp_core_reset();
  int marker = 0;
  int id = vdisp_core_alloc(1, &marker);
  EXPECT_EQ(vdisp_core_pace(id, 1000), 0);
  EXPECT_EQ(vdisp_core_pace(id, 1004), VDISP_FRAME_MS - 4);
  EXPECT_EQ(vdisp_core_pace(id, 2000), 0);
  EXPECT_EQ(vdisp_core_pace(0, 2000), 0);           /* no display: no wait */
  vdisp_core_reset();
}

void vdisplay_test_suite(void) {
  uart_puts("vdisplay_test_suite:\n");
  test_vdisp_alloc_free();
  test_vdisp_ring();
  test_vdisp_damage();
  test_vdisp_pace();
}

#endif // KERNEL_MODE_UNIT_TEST
