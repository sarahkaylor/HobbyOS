/* HobbyOS sysroot compat header: <dlfcn.h> (declarations only).
 * LLVM's llvm/lib/Support/Unix/Unix.h:42 includes <dlfcn.h> unconditionally
 * on Unix, and DynamicLibrary.inc compiles the dlopen()-based path whenever
 * HAVE_DLOPEN is on.  HobbyOS has no dynamic linking (repo policy: static
 * everywhere), so this header declares the API to let LLVMSupport compile
 * and, if any tool actually calls dlopen, the link will report it (L2 work
 * item / decide to stub to ENOTSUP).
 */
#ifndef HOBBYOS_DLFCN_H
#define HOBBYOS_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY        0x00001
#define RTLD_NOW         0x00002
#define RTLD_GLOBAL      0x00100
#define RTLD_LOCAL       0x00000
#define RTLD_DEFAULT     ((void *)0)
#define RTLD_NEXT        ((void *)-1)

/* Full Dl_info: Unix/Signals.inc uses `Dl_info dlinfo;` by value under
 * #if HAVE_DLOPEN (stack-dump symbolization), including dli_fname. */
typedef struct dl_info {
  const char *dli_fname;
  void       *dli_fbase;
  const char *dli_sname;
  void       *dli_saddr;
} Dl_info;

/* Not implemented by HobbyOS (no dynamic loader); declarations only. */
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);
int   dladdr(const void *addr, Dl_info *info);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_DLFCN_H */
