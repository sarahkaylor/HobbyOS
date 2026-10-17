/*
 * doom.c — HobbyOS entry point for the doomgeneric port.
 *
 * Launched by the desktop as a full-screen game (DOOM.BIN).  Hands the
 * process environment to doomgeneric_Create (WAD discovery, title screen,
 * attract demo, gameplay), then drives the game loop; each doomgeneric_Tick
 * renders one frame and pumps input/timer through the HobbyOS backend
 * (doomgeneric_hobbyos.c).
 *
 * License: GPLv2+ (see third_party/doomgeneric/LICENSE).
 */
#include "doomgeneric.h"
#include "libc.h"

int main(int argc, char **argv) {
  doomgeneric_Create(argc, argv);
  for (;;) {
    doomgeneric_Tick();
  }
  return 0;
}
