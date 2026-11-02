/* DOOML.BIN -- launch the Linux-ELF Doom on the 8-bit screen.
 *
 * linuxdoom-1.10 asks XMatchVisualInfo(screen, 8, PseudoColor) for the screen it
 * is pointed at and refuses to start without it.  The desktop screen (:0.0) is
 * 24-bit TrueColor, and the X protocol does not allow an 8-bit window under a
 * 24-bit root, so the server carries a second screen (:0.1) at depth 8 and this
 * launcher points the game at it.
 *
 * The menu entry is `prog "Doom" app /DOOML.BIN`, i.e. IceWM starts this program
 * and it hands over to /DOOM.ELF.  If the second screen is missing (an older
 * server, or a session booted with HB_XDEPTH=8 where :0.0 is already depth 8)
 * DISPLAY stays :0 -- the game then prints its own one-line diagnosis instead of
 * the launcher failing silently.
 */
#include "libc.h"

#define GAME "/DOOM.ELF"

int main(int argc, char **argv) {
  const char *disp = getenv("HB_GAME_DISPLAY");
  int rc;

  (void) argc;
  (void) argv;
  if (!disp)
    disp = ":0.1";
  if (setenv("DISPLAY", disp, 1) != 0)
    print_console("[DOOML] setenv(DISPLAY) failed\n");
  print_console("[DOOML] DISPLAY=");
  print_console(disp);
  print_console(" -> ");
  print_console(GAME);
  print_console("\n");

  rc = spawn(GAME, "");
  print_console("[DOOML] spawn returned\n");
  return rc < 0 ? 1 : 0;
}
