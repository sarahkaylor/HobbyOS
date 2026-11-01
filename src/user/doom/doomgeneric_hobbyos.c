/*
 * doomgeneric_hobbyos.c — HobbyOS platform backend for doomgeneric
 * (the S0 native-track port: DOOM runs as a user-space process on the
 * HobbyOS ABI under QEMU/virtio-gpu).
 *
 * Implements the six DG_* callbacks the engine needs:
 *   DG_Init          map the VirtIO GPU framebuffer
 *   DG_DrawFrame     blit the 640x400 render buffer to the fb (2x, centered)
 *   DG_SleepMs       framerate pacing via the HobbyOS sleep()
 *   DG_GetTicksMs    monotonic ms via sysinfo(1) uptime
 *   DG_GetKey        drain the VirtIO input queue into DOOM key codes
 *   DG_SetWindowTitle no-op (full-screen game)
 *
 * Input: raw VirtIO input events (get_events), Linux evdev scan codes, mapped
 * to the doomkeys.h codes the engine expects (arrows/ctrl/space/letters/…).
 * The desktop forwards the guest keyboard; down events are value==1.
 *
 * Rendering: the engine synthesizes a 640x400 uint32 RGBA frame in
 * DG_ScreenBuffer (i_video.c I_FinishUpdate, fb_scaling=2).  We blit it to
 * the 1920x1080 B8G8R8A8 framebuffer at 2x nearest-neighbor (1280x800),
 * centred on a black letterbox.  License: GPLv2+ (see third_party/doomgeneric/LICENSE).
 */
#include "doomgeneric.h"
#include "graphics/graphics.h"
#include "libc.h"
#include <stdint.h>

#define DOOM_SCALE 2

static uint32_t *hbfb = 0;   /* mapped framebuffer (B8G8R8A8) */
static int      hb_w = 0;
static int      hb_h = 0;

/* --- key queue ─────────────────────────────────────────────────────── */

/* (pressed<<8)|doomkey pairs, drained by DG_GetKey one per call. */
#define KQBUF 64
struct kg {
  uint8_t pressed;
  uint8_t key;
};
static struct kg kq[KQBUF];
static int kqr = 0, kqw = 0;

static void kq_push(uint8_t pressed, uint8_t key) {
  int n = (kqw + 1) % KQBUF;
  if (n == kqr) return;            /* full: drop lowest priority */
  kq[kqw].pressed = pressed;
  kq[kqw].key = key;
  kqw = n;
}

/* Map a Linux evdev scan code to a DOOM key code (doomkeys.h). */
static uint8_t evdev_to_doom(uint8_t code) {
  switch (code) {
  case 1:  return 27;             /* ESC      -> KEY_ESCAPE   */
  case 15: return 9;              /* TAB      -> KEY_TAB      */
  case 28: return 13;             /* ENTER    -> KEY_ENTER    */
  case 57: return 0xa2;           /* SPACE    -> KEY_USE      */
  case 29: return 0xa3;           /* LCTRL    -> KEY_FIRE     */
  case 97: return 0xa3;           /* RCTRL    -> KEY_FIRE     */
  case 42: case 54: return 0xb6;  /* SHIFT    -> KEY_RSHIFT   */
  case 14: return 8;              /* BACKSPACE-> KEY_BACKSPACE(8) */
  case 103: return 0xad;          /* UP       -> KEY_UPARROW  */
  case 108: return 0xaf;          /* DOWN     -> KEY_DOWNARROW*/
  case 105: return 0xac;          /* LEFT     -> KEY_LEFTARROW*/
  case 106: return 0xae;          /* RIGHT    -> KEY_RIGHTARROW*/
  /* number row 1..0 (evdev 2..11) -> ASCII '1'..'0' */
  case 2: return '1';  case 3: return '2';  case 4: return '3';
  case 5: return '4';  case 6: return '5';  case 7: return '6';
  case 8: return '7';  case 9: return '8';  case 10: return '9';
  case 11: return '0';
  case 12: return '-'; case 13: return '=';
  case 16: return 'q'; case 17: return 'w'; case 18: return 'e';
  case 19: return 'r'; case 20: return 't'; case 21: return 'y';
  case 22: return 'u'; case 23: return 'i'; case 24: return 'o';
  case 25: return 'p'; case 26: return '['; case 27: return ']';
  case 30: return 'a'; case 31: return 's'; case 32: return 'd';
  case 33: return 'f'; case 34: return 'g'; case 35: return 'h';
  case 36: return 'j'; case 37: return 'k'; case 38: return 'l';
  case 47: return ';'; case 48: return '\'';
  case 39: return 'z'; case 40: return 'x'; case 41: return 'c';
  case 43: return 'v'; case 44: return 'b'; case 45: return 'n';
  case 46: return 'm';
  default: return 0;              /* unmapped */
  }
}

static void kq_drain(void) {
  struct virtio_input_event ev[16];
  int n = get_events(ev, 16);
  for (int i = 0; i < n; i++) {
    if (ev[i].type != 0x01) continue;          /* EV_KEY */
    uint8_t k = evdev_to_doom((uint8_t)ev[i].code);
    if (k == 0) continue;
    kq_push((uint8_t)(ev[i].value == 1), k);   /* 1=press, 0=release */
  }
  /* The desktop forwards the focused window's keystrokes to stdin (PONG
   * reads it too; the desktop's event loop usually wins the raw virtio
   * queue, so in a window this is the path that actually delivers input).
   * Arrows arrive as ESC [ A/B/C/D; plain keys as one byte.  With no
   * stdin drain, DOOM gets zero input when launched from the start menu
   * and sits frozen on the title screen forever. */
  while (available(0) > 0) {
    char buf[32];
    int m = read(0, buf, sizeof(buf));
    if (m <= 0) break;
    for (int i = 0; i < m; i++) {
      char c = buf[i];
      if (c == '\033') {                       /* ESC sequence, or a lone ESC */
        if (i + 2 < m && buf[i+1] == '[') {
          switch (buf[i+2]) {
          case 'A': kq_push(1, 0xad); break; /* Up    */
          case 'B': kq_push(1, 0xaf); break; /* Down  */
          case 'C': kq_push(1, 0xae); break; /* Right */
          case 'D': kq_push(1, 0xac); break; /* Left  */
          default: break;
          }
          i += 2;
        } else {
          /* The desktop forwards the Escape key as one 0x1B byte (keymap[1]),
           * not as a CSI sequence.  Dropping it meant DOOM's menu could not
           * be opened from the keyboard at all (GB-091). */
          kq_push(1, 27);
        }
        continue;
      }
      if (c == '\n' || c == '\r') { kq_push(1, 13); continue; } /* Enter */
      if (c == ' ' )             { kq_push(1, 0xa2); continue; } /* Use   */
      if (c >= 'a' && c <= 'z')  { kq_push(1, (uint8_t)c); continue; }
      if (c >= '1' && c <= '9')  { kq_push(1, (uint8_t)c); continue; }
      if (c >= 'A' && c <= 'Z')  { kq_push(1, (uint8_t)(c - 'A' + 'a')); continue; }
    }
  }
}

/* --- DG_* callbacks ────────────────────────────────────────────────── */

void DG_Init(void) {
  if (graphics_init() != 0) {
    print("DOOM: graphics_init failed\n");
    exit(1);
  }
  hbfb = graphics_fb();
  hb_w = graphics_width();
  hb_h = graphics_height();
  if (!hbfb || hb_w <= 0 || hb_h <= 0) exit(1);
  /* start on a black letterboxed screen */
  graphics_clear(0xFF000000u);
  graphics_flush();
}

void DG_DrawFrame(void) {
  pixel_t *src = DG_ScreenBuffer;
  if (!src || !hbfb) return;
  int sw = DOOMGENERIC_RESX, sh = DOOMGENERIC_RESY;
  int dw = sw * DOOM_SCALE, dh = sh * DOOM_SCALE;
  int x0 = (hb_w - dw) / 2; if (x0 < 0) x0 = 0;
  int y0 = (hb_h - dh) / 2; if (y0 < 0) y0 = 0;
  for (int y = 0; y < sh; y++) {
    pixel_t *srow = src + (unsigned)y * sw;
    for (int sy = 0; sy < DOOM_SCALE; sy++) {
      uint32_t *drow = hbfb + (unsigned)(y0 + y * DOOM_SCALE + sy) * hb_w + x0;
      for (int x = 0; x < sw; x++) {
        uint32_t p = (uint32_t)srow[x] | 0xFF000000u;   /* force opaque alpha */
        for (int sx = 0; sx < DOOM_SCALE; sx++)
          drow[x * DOOM_SCALE + sx] = p;
      }
    }
  }
  /* Push the GPU-visible framebuffer changes (PONG calls graphics_flush()
   * each frame; without it the direct hbfb writes never reach the screen). */
  graphics_flush();
}

void DG_SleepMs(uint32_t ms) {
  /* libc's sleep() is POSIX SECONDS (sleep(1) == 1000 ms), so the engine's
   * per-tic I_Sleep(1) was a one-second nap: DOOM ran at ~1 tic/s instead of
   * 35, the title screen never reached its demo, and a keypress advanced the
   * frame by one tic and then looked frozen (GB-091).  usleep() is the
   * millisecond-granular path (SYS_SLEEP takes ms underneath). */
  usleep(ms * 1000u);
}

uint32_t DG_GetTicksMs(void) {
  /* NB: the kernel's sysinfo(cmd 1) returns the uptime in ms as its RETURN
   * VALUE (tf->regs[0] = timer_get_ms()) — it does not fill a buffer and
   * does not return 0.  Passing a struct and testing ==0 made this always
   * return 0, so the engine's 35 Hz tic loop never advanced, BuildNewTic
   * never ran, DG_GetKey was never called, and DOOM froze on the title
   * screen after WAD load.  Match PONG/clock: sysinfo(1, 0, 0) -> ms. */
  int ms = sysinfo(1, 0, 0);
  return ms > 0 ? (uint32_t)ms : 0;
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
  if (kqr == kqw) kq_drain();       /* refill on empty */
  if (kqr == kqw) return 0;
  *pressed = kq[kqr].pressed;
  *doomKey = kq[kqr].key;
  kqr = (kqr + 1) % KQBUF;
  return 1;
}

void DG_SetWindowTitle(const char *title) {
  (void)title;                      /* full-screen game: no window chrome */
}

/* --- freestanding ABI shims ------------------------------------------- */

/* i_system.c I_Error tries to pop a zenity/errorbox dialog via system().
 * HobbyOS has no host shell UI for that; returning -1 makes DOOM fall back
 * to its in-console error path.  libc.a does not define system() here. */
int system(const char *cmd) {
  (void)cmd;
  return -1;
}
