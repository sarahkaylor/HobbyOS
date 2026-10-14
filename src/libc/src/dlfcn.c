/*
 * HobbyOS selfhost W2-3: dlfcn "not supported" stubs.
 *
 * HobbyOS has no dynamic loader (everything is statically linked), so each
 * entry point reports failure the way a loader-less system should:
 *
 *   dlopen  -> NULL, errno ENOSYS
 *   dlsym   -> NULL, errno ENOSYS
 *   dlclose -> -1,   errno ENOSYS
 *   dlerror -> a static explanation string (glibc returns a message for
 *              the most recent dl* error; ours is the only error there is)
 *   dladdr  -> 0 ("no symbol found"), dli_* filled with the empty/zero
 *              form — the Linux contract for an address with no mapping
 *
 * LLVM's HAVE_DLOPEN=0 build (config.h, §13 of os-gap-audit.md) never
 * CALLS dlopen/dlsym; these exist so a future port or a direct call site
 * fails loudly instead of silently doing nothing.
 *
 * Under HOST_TEST the symbols are renamed hb_*.
 */
#include <errno.h>

#ifdef HOST_TEST
/* glibc's <dlfcn.h> only exposes Dl_info under _GNU_SOURCE; give the hb_*
 * host unit the POSIX shape so it compiles without feature-test flags. */
typedef struct dl_info {
  const char *dli_fname;
  void       *dli_fbase;
  const char *dli_sname;
  void       *dli_saddr;
} Dl_info;
#else
#include <dlfcn.h>
#endif

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

void *HB_NAME(dlopen)(const char *filename, int flags) {
  (void)filename;
  (void)flags;
  errno = ENOSYS;
  return 0;
}

void *HB_NAME(dlsym)(void *handle, const char *symbol) {
  (void)handle;
  (void)symbol;
  errno = ENOSYS;
  return 0;
}

int HB_NAME(dlclose)(void *handle) {
  (void)handle;
  errno = ENOSYS;
  return -1;
}

char *HB_NAME(dlerror)(void) {
  return "HobbyOS: no dynamic loader (dlopen not supported)";
}

int HB_NAME(dladdr)(const void *addr, Dl_info *info) {
  (void)addr;
  if (info) {
    info->dli_fname = "";
    info->dli_fbase = 0;
    info->dli_sname = 0;
    info->dli_saddr = 0;
  }
  return 0; /* 0 = no symbol found (Linux contract) */
}
