/*
 * main.c — HobbyOS entry for the compact NES emulator (GB-035).
 *
 * Boots a hand-authored NROM homebrew ROM on the compact NES core (nes.c),
 * runs a frame's worth of 6502 cycles per iteration, renders the 256x240
 * PPU framebuffer onto the HobbyOS VirtIO-GPU display (4x upscaled, centred),
 * and maps keyboard input to the NES controller so the screen responds to
 * input (Left/Right/Up/Down -> A/B/Sel/St; Space inverts the frame; Q quits).
 *
 * License: MIT (swarm-authored).
 */
#include "libc.h"
#include "graphics/graphics.h"
#include "nes.h"
#include "homebrew_rom.h"

static nes_machine g_m;
static unsigned g_joy_prev = 0;
static int g_phase = 0;

static void load_rom(void){
  int i;
  for (i = 0; i < HOME_ROM_LEN; i++) g_m.prg[i] = home_rom[i];
  g_m.prg[0x7FFC] = 0x00;     /* reset vector -> $8000 */
  g_m.prg[0x7FFD] = 0x80;
}

/* Map an input code (EV_KEY scan code or stdin byte) to a gamepad bit. */
static int key_to_fire(int code){
  switch (code) {
    case 0x69: case 'a': case 'A': return 0;   /* Left  -> A   */
    case 0x6a: case 'd': case 'D': return 1;   /* Right -> B   */
    case 0x67: case 'w': case 'W': return 2;   /* Up    -> Sel */
    case 0x6c: case 's': case 'S': return 3;   /* Down  -> St  */
    default: return -1;
  }
}

static void drain_input(void){
  /* Desktop-window processes get keys on stdin (arrows ESC[A-D, chars as
     bytes); also drain the raw VirtIO queue in case it wins focus. */
  struct virtio_input_event ev[8];
  int n = get_events(ev, 8);
  for (int i = 0; i < n; i++)
    if (ev[i].type == 0x01 && ev[i].value == 1) {
      int b = key_to_fire(ev[i].code);
      if (b >= 0) g_m.joy |= (1u << b);
    }
  while (available(0) > 0) {
    char buf[16];
    int m = read(0, buf, sizeof(buf));
    if (m <= 0) break;
    for (int i = 0; i < m; i++) {
      char c = buf[i];
      if (c == '\033' && i + 2 < m && buf[i+1] == '[') {
        static const int escmap[4] = { 0x67, 0x6c, 0x69, 0x6a }; /* A B C D */
        int b = key_to_fire(escmap[buf[i+2] - 'A']);
        if (b >= 0) g_m.joy |= (1u << b);
        i += 2;
      } else {
        if (c == 0x20) g_phase = !g_phase;           /* space: invert */
        if (c == 'q' || c == 'Q') { exit(0); }
        int b = key_to_fire(c);
        if (b >= 0) g_m.joy |= (1u << b);
      }
    }
  }
}

/* Present a single-button press edge to the machine (not a held level). */
static void apply_input_edges(void){
  unsigned edge = g_m.joy & ~g_joy_prev;
  g_joy_prev = g_m.joy;
  g_m.joy = edge;
}

int main(void){
  if (graphics_init() != 0) { print("NES: graphics_init failed\n"); return 1; }
  load_rom();
  nes_reset(&g_m);

  int W = SCREEN_WIDTH, H = SCREEN_HEIGHT;
  int scale = 4;
  int x0 = (W - NES_SCREEN_W * scale) / 2;
  int y0 = (H - NES_SCREEN_H * scale) / 2;

  for (;;) {
    drain_input();
    apply_input_edges();

    /* run the homebrew ROM for roughly one frame of 6502 cycles */
    nes_run(&g_m, 24000);

    /* render and upscale the PPU buffer to the display */
    nes_ppu_render(&g_m);
    for (int y = 0; y < NES_SCREEN_H; y++) {
      for (int x = 0; x < NES_SCREEN_W; x++) {
        u8 sh = g_m.ppu.fb[y * NES_SCREEN_W + x];
        if (g_phase) sh = (u8)(255 - sh);            /* space: invert */
        uint32_t c = COLOR(sh, sh, sh);
        graphics_draw_rect(x0 + x * scale, y0 + y * scale, scale, scale, c);
      }
    }
    graphics_flush();
    sleep(20);
  }
  return 0;
}
