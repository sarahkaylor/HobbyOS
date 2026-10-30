/*
 * vdisplay_test.c - host golden test for the virtual-display core
 * (src/kernel/vdisplay.c built with -DVDISP_HOST_TEST; icewm.md §9).
 * Locks the record layout HBHOST and the kernel share and the ring /
 * damage / generation semantics.
 */
#include <stdio.h>
#include <stddef.h>
#include "vdisplay.h"
#include "virtio_input.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
  CHECK(sizeof(struct vdisp_event) == 8);
  CHECK(sizeof(struct vdisp_event) == sizeof(struct virtio_input_event));
  CHECK(offsetof(struct vdisp_event, value) == offsetof(struct virtio_input_event, value));
  CHECK(sizeof(struct vdisp_status) == 36);

  vdisp_core_reset();
  int o = 0;
  int a = vdisp_core_alloc(3, &o);
  CHECK(a == 1);
  uint32_t g = vdisp_core_gen(a);
  CHECK(vdisp_core_valid(a, g));
  struct vdisp_event ev[2] = {{1, 30, 1}, {1, 30, 0}};
  CHECK(vdisp_core_inject(a, ev, 2) == 2);
  struct vdisp_event out[4];
  CHECK(vdisp_core_read(a, out, 4, 1000) == 2);
  CHECK(out[0].code == 30 && out[0].value == 1 && out[1].value == 0);
  vdisp_core_damage(a, 100, 100, 10, 10);
  vdisp_core_damage(a, 50, 120, 10, 10);
  struct vdisp_status st;
  vdisp_core_status(a, &st, 1250);
  CHECK(st.flush_seq == 2 && st.dx == 50 && st.dy == 100 && st.dw == 60 && st.dh == 30);
  CHECK(st.read_age_ms == 250 && st.reads == 1 && st.queued == 0);
  vdisp_core_status(a, &st, 1250);
  CHECK(st.dw == 0);
  CHECK(vdisp_core_free(a) == &o);
  CHECK(!vdisp_core_valid(a, g));
  int b = vdisp_core_alloc(4, &o);
  CHECK(b == a && vdisp_core_gen(b) != g);
  printf("vdisplay_test: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
