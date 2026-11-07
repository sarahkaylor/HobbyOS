/*
 * tools/rw/udev-absent-probe.c — host-only A/B probe for the M6 display wall.
 *
 * The guest has no libudev.so.1/.so.0, and the record of the game's stall
 * (docs/rimworld-launch-log.md §11.4) needed to show that the missing udev is
 * what produces
 *
 *     Error getting num native displays: Video subsystem has not been initialized
 *
 * rather than the compat X11 layer.  Build it and preload it into a HOST run of
 * the same launcher:
 *
 *   gcc -shared -fPIC -o /tmp/noudev.so tools/rw/udev-absent-probe.c -ldl
 *   HOME=/tmp/rwhost DISPLAY=:99 LD_PRELOAD=/tmp/noudev.so \
 *     /srv/nfs/rimworld/RimWorldLinux -force-glcore ... -logFile /tmp/rwhost/Player.log
 *
 * With the preload the host Player.log prints the guest's exact error line
 * (three times) and then recovers to `Initialize engine version`/`GfxDevice`;
 * without it the host run is clean (`Display 0 'screen'`, `GfxDevice`,
 * llvmpipe).  That is the evidence that SDL's joystick stage is the trigger.
 *
 * Never staged into the guest and never linked into a build product.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

static void *(*real_dlopen)(const char *, int);

void *dlopen(const char *file, int mode) {
  if (!real_dlopen) {
    void *(*real_dlvsym)(void *, const char *, const char *) =
        (void *(*)(void *, const char *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    real_dlopen = (void *(*)(const char *, int))
                      real_dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
  }
  if (file && strstr(file, "libudev")) return 0;
  return real_dlopen(file, mode);
}
