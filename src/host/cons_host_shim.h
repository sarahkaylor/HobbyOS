/* Host shim for src/kernel/kconsole.c (src/host/kconsole_test.c). */
#ifndef CONS_HOST_SHIM_H
#define CONS_HOST_SHIM_H
#include <stdint.h>
#define MAX_CPUS 8
extern uint32_t get_cpuid(void);
extern uint64_t timer_get_us(void);
extern int cpu_is_halted(uint32_t cpu);
/* Thread-local so the threaded test (kconsole_mt_test.c) can run one "CPU"
   per thread; the single-threaded test is unaffected. */
extern _Thread_local const char *cons_host_sp;
void cons_test_set_owner(uint32_t owner);
#endif
