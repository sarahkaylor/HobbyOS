/* WDMAP.BIN — WD lane positive-control chroma paint.
 *
 * Booted as DESKTOP.BIN (single-program boot) on the x64 repro disk:
 * maps the system framebuffer (SYS_MAP_FB), fills the whole 1024x768
 * surface with EXPLICIT known colors (probe green #40A060 on the top
 * half, dark page bg #102030 on the bottom, a pure-white vertical
 * spine at x=512 and a white horizontal band at y=384), then does a
 * full SYS_FLUSH_FB.  Nothing else runs, so the screen after boot must
 * equal what this program wrote — pixel-for-pixel proof that an
 * arbitrary second process's write + flush reaches the visible scanout.
 *
 * The kernel's [WD] fb-sample probe cross-checks the SAME pixels in the
 * kernel framebuffer static at flush time (serial: "[WD] n=.. P1..P6").
 *
 * Rebuild:  clang <USER_CFLAGS> -c wdmap.c;  ld -T linker.ld
 * wdmap.o user_libc.o user_malloc.o libc_string.o -> .elf;  objcopy -O
 * binary -> .bin;  mcopy into a disk as ::/DESKTOP.BIN.
 */
#include "libc.h"
#include <sched.h>
#include <stdint.h>

#define FB_W 1024
#define FB_H 768

#define C_PROBE 0xFF40A060u /* "page green" #40a060 */
#define C_BG    0xFF102030u /* page bg #102030 */
#define C_WHITE 0xFFFFFFFFu

/* Keep the process alive so the frame persists for the QMP screendump
 * (QEMU halts when the last process exits).  Busy loop: nothing else
 * runs in this single-program boot. */
static void spin(void) {
  for (;;) {
    volatile int sink = 0;
    (void)sink;
  }
}

int main(void) {
  volatile uint32_t *fb = (volatile uint32_t *)map_fb();
  if (!fb) {
    print_console("WDMAP map_fb FAIL\n");
    return 1;
  }
  for (int y = 0; y < FB_H; y++) {
    uint32_t row_color = (y < 384) ? C_PROBE : C_BG;
    for (int x = 0; x < FB_W; x++) {
      uint32_t c = row_color;
      if (x == 512 || y == 384) c = C_WHITE;
      fb[y * FB_W + x] = c;
    }
  }
  flush_fb();
  print_console("WDMAP painted (probe green top / page-bg bottom / white spine+band)\n");
  spin();
  return 0;
}
