/* Does the guest load libfontconfig, and does FcInit succeed?
 *
 * libfontmanager.so does NOT link fontconfig (NEEDED: libawt, libc, libfreetype,
 * libjava, libjvm, libm) and the "Fontconfig head is null" string lives in
 * lib/modules, so the JDK dlopen()s it at runtime -- everything hinges on that
 * dlopen working in the guest.
 *
 * The first version of this probe printed "(null)" for every failure because it
 * called dlerror() twice: the first call returns and CLEARS the message, the
 * second returns NULL.  Capture it once, into a buffer.
 */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

static void *open_one(const char *p, char *err, size_t n) {
  (void) dlerror();                 /* clear any previous error */
  void *h = dlopen(p, RTLD_NOW);
  if (!h) {
    const char *e = dlerror();      /* the ONE call that has the text */
    snprintf(err, n, "%s", e ? e : "(dlerror returned nothing)");
  }
  return h;
}

int main(void) {
  const char *cand[5] = { "libfontconfig.so.1",
                          "/JDK/lib/libfontconfig.so.1",
                          "/lib/x86_64-linux-gnu/libfontconfig.so.1",
                          "/lib/libfontconfig.so.1",
                          "/lib/x86_64-linux-gnu/libexpat.so.1" };
  void *h = 0; char err[512] = {0}; int which = -1;
  for (int i = 0; i < 5; i++) {
    h = open_one(cand[i], err, sizeof err);
    if (h) { printf("FCP dlopen %s OK\n", cand[i]); which = i; break; }
    printf("FCP dlopen %s FAILED: %s\n", cand[i], err);
  }
  if (!h) { printf("FCP VERDICT no-fontconfig-library\n"); return 1; }

  int (*ver)(void) = (int (*)(void))dlsym(h, "FcGetVersion");
  printf("FCP version=%d\n", ver ? ver() : -1);

  void *(*init)(void) = (void *(*)(void))dlsym(h, "FcInitLoadConfigAndFonts");
  if (!init) { printf("FCP FcInitLoadConfigAndFonts missing: %s\n", dlerror()); return 1; }
  void *cfg = init();
  printf("FCP FcInitLoadConfigAndFonts=%p\n", cfg);

  if (cfg) {
    void *(*getfonts)(void *, const char *, void *) =
        (void *(*)(void *, const char *, void *))dlsym(h, "FcConfigGetFonts");
    int (*count)(void *) = (int (*)(void *))dlsym(h, "FcFontSetFontCount");
    if (getfonts && count) {
      void *set = getfonts(cfg, "accept", 0);
      printf("FCP fonts=%d (set=%p)\n", set ? count(set) : -1, set);
    }
  }
  printf("FCP VERDICT %s\n", cfg ? "fontconfig-works" : "init-returned-null");
  return cfg ? 0 : 1;
}
