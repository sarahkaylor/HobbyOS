/* Does the guest load libfontconfig at all, and does FcInit succeed?
 *
 * libfontmanager.so does NOT link fontconfig (its NEEDED list is libawt, libc,
 * libfreetype, libjava, libjvm, libm) and the "Fontconfig head is null" string
 * lives in lib/modules -- so the JDK dlopen()s fontconfig at runtime, and the
 * only question that matters is whether that dlopen and FcInit work in the guest.
 * No fontconfig headers: dlsym by name, which is exactly what the JDK does.
 */
#include <stdio.h>
#include <dlfcn.h>

typedef void *(*fc_init_t)(void);
typedef int   (*fc_ver_t)(void);

int main(void) {
  const char *cand[4] = { "libfontconfig.so.1",
                          "/JDK/lib/libfontconfig.so.1",
                          "/lib/x86_64-linux-gnu/libfontconfig.so.1",
                          "/lib/libfontconfig.so.1" };
  void *h = 0;
  for (int i = 0; i < 4; i++) {
    h = dlopen(cand[i], RTLD_NOW);
    if (h) { printf("FCP dlopen %s OK\n", cand[i]); break; }
    printf("FCP dlopen %s FAILED: %s\n", cand[i], dlerror() ? dlerror() : "(null)");
  }
  if (!h) { printf("FCP VERDICT no-fontconfig-library\n"); return 1; }

  fc_ver_t ver = (fc_ver_t)dlsym(h, "FcGetVersion");
  printf("FCP version=%d\n", ver ? ver() : -1);

  fc_init_t init = (fc_init_t)dlsym(h, "FcInitLoadConfigAndFonts");
  if (!init) { printf("FCP FcInitLoadConfigAndFonts missing: %s\n", dlerror()); return 1; }
  void *cfg = init();
  printf("FCP FcInitLoadConfigAndFonts=%p\n", cfg);

  /* list how many fonts fontconfig actually knows about */
  void *(*fc_fontlist)(void *, const char *, void *) =
      (void *(*)(void *, const char *, void *))dlsym(h, "FcConfigGetFonts");
  if (fc_fontlist && cfg) {
    void *set = fc_fontlist(cfg, "accept", 0);
    int (*nfonts)(void *) = (int (*)(void *))dlsym(h, "FcFontSetFontCount");
    if (set && nfonts) printf("FCP fonts=%d\n", nfonts(set));
  }
  printf("FCP VERDICT %s\n", cfg ? "fontconfig-works" : "init-returned-null");
  return cfg ? 0 : 1;
}
