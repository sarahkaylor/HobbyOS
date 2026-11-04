/*
 * rwmain.c — AD-9: the RimWorld executable, rebuilt as a HobbyOS compat PIE.
 *
 * The shipped RimWorldLinux is a 14 KB ET_EXEC stub linked at 0x200000 whose
 * main is literally `jmp PlayerMain@plt`; HobbyOS's compat loader refuses an
 * ET_EXEC below its 64 GiB user window (docs/rimworld-launch-log.md §4), and
 * the v2 page tables cannot represent 0x200000.  This program is the same
 * program at a legal address: it dlopen()s UnityPlayer.so, dlsyms the mangled
 * PlayerMain symbol the stub imports, and calls it with the arguments it was
 * given.  Nothing about the game's own code changes.
 *
 * It must be installed as `<game dir>/RimWorldLinux` because Unity derives its
 * data folder from /proc/self/exe and opens `<dir>/RimWorldLinux_Data`; the
 * real stub is kept next to it as RimWorldLinux.orig in the NFS staging copy.
 *
 * AD-10 (DSOs staged locally, data on NFS): UnityPlayer.so is loaded through
 * this launcher (RW_UNITYPLAYER, else next to the executable), and the Mono /
 * plugin DSOs Unity loads later are redirected from their NFS paths to the
 * local staging directory by the dlopen() wrapper below.  mmap of an NFS file
 * is -ENOTSUP in HobbyOS (M5b), which is the whole reason for the staging.
 * The redirect is by basename inside RW_DSO_DIR, so it does not depend on how
 * Unity spells the data path.
 *
 * The wrapper is a normal dynamic symbol of this executable (linked
 * -rdynamic).  glibc resolves UnityPlayer.so's dlopen@GLIBC_2.2.5 through the
 * global scope, and the main executable is first in it; the real dlopen is
 * reached with dlsym(RTLD_NEXT, "dlopen").
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RW_UNITYPLAYER_DEFAULT
#define RW_UNITYPLAYER_DEFAULT "/RWDSO/UnityPlayer.so"
#endif
#define RW_PLAYERMAIN "_Z10PlayerMainiPPc"

static void *(*next_dlopen)(const char *, int);
static const char *dso_dir;

static const char *base_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* Map a DSO name Unity asks for onto the local staging copy, if one exists.
 * Only the basename is used: Unity asks for
 *   <data>/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so
 *   <data>/Plugins/lib_burst_generated.so
 * and the local copy is /RWDSO/<basename> regardless of where the data tree
 * lives.  Mono's native loader first probes the name without ".so" (measured
 * on the host: .../libmono-native then .../libmono-native.so), so that form is
 * mapped too. */
static int map_local(const char *name, char *out, size_t cap) {
  if (!dso_dir || !dso_dir[0] || !name || !name[0]) return 0;
  const char *base = base_of(name);
  if (!base[0]) return 0;
  if (strncmp(name, dso_dir, strlen(dso_dir)) == 0) return 0; /* already local */
  if (snprintf(out, cap, "%s/%s", dso_dir, base) >= (int)cap) return 0;
  if (access(out, R_OK) == 0) return 1;
  if (!strchr(base, '.')) {
    if (snprintf(out, cap, "%s/%s.so", dso_dir, base) < (int)cap &&
        access(out, R_OK) == 0)
      return 1;
  }
  return 0;
}

void *dlopen(const char *file, int mode) {
  char local[512];
  if (!next_dlopen) {
    next_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    if (!next_dlopen) {
      printf("[RWMAIN] FATAL: cannot resolve the real dlopen via RTLD_NEXT\n");
      fflush(stdout);
      return 0;
    }
  }
  if (file && map_local(file, local, sizeof local)) {
    printf("[RWMAIN] dlopen remap %s -> %s\n", file, local);
    fflush(stdout);
    void *r = next_dlopen(local, mode);
    if (!r) {
      /* Unity only says "Unable to load mono library"; the loader's reason
         (a missing NEEDED, an undefined symbol, a version) is the evidence. */
      const char *e = dlerror();
      printf("[RWMAIN] dlopen FAILED %s: %s\n", local, e ? e : "(no dlerror)");
      fflush(stdout);
    }
    return r;
  }
  return next_dlopen(file, mode);
}

/* The executable path, and the directory Unity will append "_Data" to. */
static int exe_dir(char *out, size_t cap) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return -1;
  exe[n] = 0;
  printf("[RWMAIN] /proc/self/exe = %s\n", exe);
  char *slash = strrchr(exe, '/');
  if (!slash) return -1;
  *slash = 0;
  if (!exe[0]) strcpy(exe, "/");
  snprintf(out, cap, "%s", exe);
  return 0;
}

int main(int argc, char **argv) {
  printf("[RWMAIN] AD-9 PlayerMain launcher start argc=%d\n", argc);
  fflush(stdout);

  char dir[4096];
  if (exe_dir(dir, sizeof dir) != 0) {
    printf("[RWMAIN] FAIL readlink(/proc/self/exe)\n");
    return 1;
  }
  printf("[RWMAIN] data dir = %s/RimWorldLinux_Data\n", dir);

  dso_dir = getenv("RW_DSO_DIR");
  printf("[RWMAIN] RW_DSO_DIR = %s\n", dso_dir && dso_dir[0] ? dso_dir : "(unset)");

  char beside[4096], chosen[4096];
  snprintf(beside, sizeof beside, "%s/UnityPlayer.so", dir);

  const char *env = getenv("RW_UNITYPLAYER");
  if (env && env[0]) {
    snprintf(chosen, sizeof chosen, "%s", env);
    printf("[RWMAIN] UnityPlayer.so from RW_UNITYPLAYER\n");
  } else if (access(beside, R_OK) == 0) {
    snprintf(chosen, sizeof chosen, "%s", beside);
    printf("[RWMAIN] UnityPlayer.so next to the executable\n");
  } else {
    snprintf(chosen, sizeof chosen, "%s", RW_UNITYPLAYER_DEFAULT);
    printf("[RWMAIN] UnityPlayer.so from the built-in default\n");
  }

  printf("[RWMAIN] dlopen(%s, RTLD_NOW|RTLD_GLOBAL)\n", chosen);
  fflush(stdout);
  void *h = dlopen(chosen, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    printf("[RWMAIN] FAIL dlopen: %s\n", dlerror());
    fflush(stdout);
    return 2;
  }
  printf("[RWMAIN] UnityPlayer.so handle=%p\n", h);
  fflush(stdout);

  dlerror();
  void *sym = dlsym(h, RW_PLAYERMAIN);
  const char *err = dlerror();
  if (!sym) {
    printf("[RWMAIN] FAIL dlsym(" RW_PLAYERMAIN "): %s\n", err ? err : "?");
    fflush(stdout);
    return 3;
  }
  printf("[RWMAIN] PlayerMain=%p argc=%d — calling\n", sym, argc);
  fflush(stdout);

  int rc = ((int (*)(int, char **))sym)(argc, argv);
  printf("[RWMAIN] PlayerMain returned %d\n", rc);
  fflush(stdout);
  return rc;
}
