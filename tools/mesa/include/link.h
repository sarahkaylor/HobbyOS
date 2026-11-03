/* link.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/build_id.c reads the running libGL's GNU build-id through
 * dl_iterate_phdr() to key the shader cache.  HobbyOS's compat dlfcn.h has
 * dladdr()/Dl_info but no phdr walk, and -Dshader-cache=disabled means the
 * cache never opens; the walk is therefore a fail-closed stub (gl_stubs.c).
 * The struct layout is the Linux x86_64 ELF one, so the stub can honestly
 * report "no program headers". */
#ifndef HOBBYOS_LINK_H
#define HOBBYOS_LINK_H 1

#include <stddef.h>
#include <stdint.h>
#include <elf.h>

struct dl_phdr_info {
  uintptr_t dlpi_addr;
  const char *dlpi_name;
  const ElfW(Phdr) *dlpi_phdr;
  uint16_t dlpi_phnum;
  /* glibc 2.34+ */
  unsigned long long dlpi_adds;
  unsigned long long dlpi_subs;
  size_t dlpi_tls_modid;
  void *dlpi_tls_data;
};

#ifdef __cplusplus
extern "C" {
#endif
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
                                    void *data),
                    void *data);
#ifdef __cplusplus
}
#endif

#endif
