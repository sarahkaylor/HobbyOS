/* tools/rw/xext-absent-probe.c -- host-only A/B: hide XRandR from SDL so a
 * host run sees the guest's "no RandR" display surface.
 *
 * Question it answers: the HobbyOS guest stages no `libXrandr.so.2`, so SDL's
 * X11 driver has no mode list (only `SDL_GetDesktopDisplayMode` works).  Does
 * a host run with RandR hidden stop where the guest stops (after
 * `Display 0 '…': WxH (primary device).`, before `XCreateWindow`)?
 *
 * Measured answer (2026-11-04, docs/rimworld-launch-log.md §12.4): **no**.
 * With only `libXrandr` hidden, the host still reaches `Initialize engine
 * version` / `GfxDevice` / `OPENGL LOG: Creating OpenGL 4.5`.  Hiding
 * `libXext`/`libXi` too breaks SDL_Init itself (the log collapses to the
 * 497-byte `Video subsystem has not been initialized` shape), which confounds
 * the experiment — so this probe hides RandR only.
 *
 * Host-only; never staged, never linked into a build.
 *
 *   gcc -shared -fPIC -o /tmp/noxext.so tools/rw/xext-absent-probe.c -ldl
 *   Xvfb :179 -screen 0 1280x800x24 &
 *   HOME=/tmp/rwhost DISPLAY=:179 SDL_VIDEODRIVER=x11 \
 *     LD_PRELOAD=/tmp/noxext.so ./RimWorldLinux -force-glcore \
 *     -screen-width 1024 -screen-height 600 -screen-fullscreen 0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

static void *(*real_dlopen)(const char *, int);

void *dlopen(const char *file, int mode) {
  static const char *hide[] = {
      /* Only RandR: hiding libXext/libXi as well breaks SDL_Init itself
       * (measured: 497-byte log, "Video subsystem has not been initialized"),
       * which confounds the experiment. */
      "libXrandr", 0
  };
  if (!real_dlopen) {
    void *(*real_dlvsym)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    real_dlopen = (void *(*)(const char *, int))
                      real_dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
  }
  if (file) {
    for (int i = 0; hide[i]; i++)
      if (strstr(file, hide[i])) return 0;
  }
  return real_dlopen(file, mode);
}
