/*
 * dlfcn.h — dynamic loading interface for the HobbyOS sysroot (W2-3).
 *
 * HobbyOS links everything statically (repo policy) and has no dynamic
 * loader, so the full POSIX surface is declared for source compatibility
 * and every entry point reports "not supported" (see src/libc/src/dlfcn.c;
 * semantics documented per function there).  LLVM's
 * llvm/lib/Support/Unix/Unix.h includes <dlfcn.h> unconditionally on Unix,
 * and Unix/DynamicLibrary.inc uses Dl_info under HAVE_DLOPEN, so the type
 * must exist regardless of the config flag.
 */
#ifndef HOBBYOS_DLFCN_H
#define HOBBYOS_DLFCN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY    0x00001
#define RTLD_NOW     0x00002
#define RTLD_GLOBAL  0x00100
#define RTLD_LOCAL   0x00000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)

  typedef struct dl_info {
    const char *dli_fname;
    void       *dli_fbase;
    const char *dli_sname;
    void       *dli_saddr;
  } Dl_info;

  void *dlopen(const char *filename, int flags);
  void *dlsym(void *handle, const char *symbol);
  int   dlclose(void *handle);
  char *dlerror(void);
  int   dladdr(const void *addr, Dl_info *info);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_DLFCN_H */
