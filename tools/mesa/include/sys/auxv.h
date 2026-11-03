/* sys/auxv.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/u_cpu_detect.c asks getauxval(AT_HWCAP) on ARM/MIPS branches; the
 * x86_64 branch uses CPUID and never calls it.  Defining it keeps the TU
 * portable and costs one fail-closed stub in gl_stubs.c. */
#ifndef HOBBYOS_SYS_AUXV_H
#define HOBBYOS_SYS_AUXV_H 1

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_PLATFORM 15
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

#ifdef __cplusplus
extern "C" {
#endif
unsigned long getauxval(unsigned long type);
#ifdef __cplusplus
}
#endif

#endif
