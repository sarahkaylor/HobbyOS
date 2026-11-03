/* DOOML.BIN -- launch the Linux-ELF Doom on the 8-bit screen.
 *
 * linuxdoom-1.10 asks XMatchVisualInfo(screen, 8, PseudoColor) for the screen it
 * is pointed at and refuses to start without it.  The desktop screen (:0.0) is
 * 24-bit TrueColor, and the X protocol does not allow an 8-bit window under a
 * 24-bit root, so the server carries a second screen (:0.1) at depth 8 and this
 * launcher points the game at it.
 *
 * The menu entry is `prog "Doom (Linux ELF)" app /DOOML.BIN`, i.e. IceWM starts
 * this program and it hands over to /DOOM.ELF.  Arguments are forwarded, so a
 * `prog` line can carry the game's own switches (`... /DOOML.BIN -warp 1 1`).
 *
 * The environment has to be passed EXPLICITLY: on HobbyOS a child inherits the
 * process group's environment BLOB, which only an envp-carrying spawn rewrites.
 * setenv() alone changes this process's `environ` and a plain spawn()ed child
 * never sees it (measured: the launcher logged "DISPLAY=:0.1" and Doom still
 * connected to :0, printed "xdoom currently only supports 256-color
 * PseudoColor screens" and died).  spawn_ex() marshals the envp we hand it, so
 * this builds a copy of our environment with DISPLAY replaced.
 *
 * If the second screen is missing (an older server, or a session booted with
 * HB_XDEPTH=8 where :0.0 is already depth 8) the game fails to pick a visual
 * and prints its own one-line diagnosis instead of the launcher failing
 * silently; HB_GAME_DISPLAY overrides the target display.
 */
#include "libc.h"

#define GAME "/DOOM.ELF"
#define MAX_ENV 62
#define MAX_ARGV 12

extern char **environ;

/* Copy `environ` into `out`, dropping any DISPLAY= row, and append
 * "DISPLAY=<disp>".  Returns the number of entries.  Static storage: this is a
 * one-shot launcher and libc.a's malloc is not linked into every flavour. */
static int build_env(const char *disp, char **out) {
  static char dispbuf[64];
  const char *p = "DISPLAY=";
  int i = 0, n = 0, k = 0;

  while (*p && k < (int) sizeof dispbuf - 1) dispbuf[k++] = *p++;
  while (*disp && k < (int) sizeof dispbuf - 1) dispbuf[k++] = *disp++;
  dispbuf[k] = '\0';

  while (environ && environ[i] && n < MAX_ENV - 1) {
    if (strncmp(environ[i], "DISPLAY=", 8) == 0) {
      i++;
      continue;
    }
    out[n++] = environ[i++];
  }
  out[n++] = dispbuf;
  out[n] = NULL;
  return n;
}

int main(int argc, char **argv) {
  const char *disp = getenv("HB_GAME_DISPLAY");
  static char *envp[MAX_ENV + 1];
  static char *av[MAX_ARGV];
  int pid, i;

  if (!disp)
    disp = ":0.1";
  if (setenv("DISPLAY", disp, 1) != 0)
    print_console("[DOOML] setenv(DISPLAY) failed\n");
  print_console("[DOOML] DISPLAY=");
  print_console(disp);
  print_console(" -> ");
  print_console(GAME);
  print_console("\n");

  build_env(disp, envp);
  /* Forward our arguments to the game: argv[0] becomes the game's path. */
  av[0] = (char *) GAME;
  for (i = 1; i < argc && i < MAX_ARGV - 1; i++)
    av[i] = argv[i];
  av[i] = NULL;
  pid = spawn_ex(GAME, av, envp, NULL, 0);
  if (pid < 0) {
    print_console("[DOOML] spawn_ex(" GAME ") failed\n");
    return 1;
  }
  print_console("[DOOML] spawned\n");
  return 0;
}
