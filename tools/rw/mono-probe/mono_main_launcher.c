/* tools/rw/mono-probe/mono_main_launcher.c — run an assembly under the
 * GAME's own embedded Mono.
 *
 * Unity's MonoBleedingEdge ships libmonobdwgc-2.0.so, a shared runtime that
 * exports mono_main, but no launcher binary.  dlopen it with RTLD_GLOBAL so
 * libmono-native.so resolves its mono_* imports against the same runtime, then
 * call mono_main(argc, argv) exactly as a normal mono would (argv[0] becomes
 * the "mono" path, argv[1..] are mono's own options and the assembly).
 *
 *   MONO_ASSEMBLY_DIR=/usr/lib/mono/4.5 MONO_CONFIG_DIR=/etc \
 *   MONO_LIB=/usr/lib/libmonobdwgc-2.0.so launcher --config /MONO.CONFIG /TP.EXE
 *
 * mono_set_dirs() is called first: the Unity build's compiled-in prefix is
 * /home/bokken/build/output/Unity-Technologies/mono/tmp, which does not exist
 * anywhere else, so without it mscorlib cannot be found outside the player.
 * ($mono_libdir stays the compiled-in /usr/lib, which is where the dllmap
 * for System.Native points — the same path the game itself uses.)
 *
 * This is the host half of a host-vs-guest differential: the same runtime
 * bytes, the same mscorlib, the same probe — only the kernel differs.  It is
 * also the only way to exercise Unity's runtime outside Unity: the official
 * mono-sgen cannot load libmono-native.so (that build hides four mono_*
 * symbols), so a probe under mono-sgen says "the ThreadPool is dead" on ANY
 * kernel.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*mono_main_fn)(int, char **);
typedef void (*mono_set_dirs_fn)(const char *, const char *);

int main(int argc, char **argv) {
  const char *lib = getenv("MONO_LIB");
  if (!lib || !*lib)
    lib = "/usr/lib/libmonobdwgc-2.0.so";
  void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    fprintf(stderr, "dlopen(%s): %s\n", lib, dlerror());
    return 2;
  }
  mono_set_dirs_fn sd = (mono_set_dirs_fn)dlsym(h, "mono_set_dirs");
  mono_main_fn mm = (mono_main_fn)dlsym(h, "mono_main");
  if (!mm) {
    fprintf(stderr, "dlsym(mono_main): %s\n", dlerror());
    return 2;
  }
  /* /TPTRACE (staged in the image) turns on Mono's own threadpool trace, the
     rows that separate "the worker never parked" from "the worker parked and
     timed out": worker starting / parking / unparking / timed out. */
  {
    FILE *tf = fopen("/TPTRACE", "r");
    if (tf) {
      fclose(tf);
      setenv("MONO_LOG_LEVEL", "debug", 1);
      setenv("MONO_LOG_MASK", "threadpool", 1);
    }
  }
  if (sd && getenv("MONO_NO_SET_DIRS") == NULL) {
    const char *ad = getenv("MONO_ASSEMBLY_DIR");
    const char *cd = getenv("MONO_CONFIG_DIR");
    if (!ad || !*ad)
      ad = "/usr/lib/mono/4.5";
    if (!cd || !*cd)
      cd = "/etc";
    sd(ad, cd);
    /* mono_main() re-applies its compile-time MONO_ASSEMBLIES prefix after
       this, so the assembly directory must also ride in MONO_PATH — that is
       the one search path mono_init() does not overwrite. */
    setenv("MONO_PATH", ad, 1);
  }
  return mm(argc, argv);
}
