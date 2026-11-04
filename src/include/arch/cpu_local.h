#ifndef ARCH_CPU_LOCAL_H
#define ARCH_CPU_LOCAL_H

/*
 * x86_64 per-CPU block — ONE definition, shared by every TU.
 *
 * Why this header exists: the struct used to be defined twice, and the two
 * copies disagreed.  trap.c had the full block (the GB-147 frame snapshot
 * fields, 104 bytes); process.c had a six-field mirror (48 bytes) for its
 * `extern struct cpu_local cpu_locals[]`.  `cpu_locals` itself is defined in
 * trap.c, so process.c indexed it with the WRONG STRIDE: for cpu >= 1,
 * set_current_process_pid(cpu, pid) wrote base + cpu*48 + 40 while the CPU's
 * GS base pointed at base + cpu*104.  Consequence measured on the
 * dynamic-link pilot: a boot program claimed by an AP ran with
 * current_process() == NULL, so open()/console/exit all failed, while the
 * same program on CPU 0 worked.  Any code that indexes cpu_locals[] must
 * include THIS header, never a private mirror.
 *
 * The FRAMESNAP stub asm addresses these fields numerically (gs:[48]..
 * gs:[104]), so the offsets are load-bearing and pinned below.
 */

#include <stdint.h>

#ifdef __x86_64__

struct process;

struct cpu_local {
  uint64_t kernel_stack;
  uint64_t user_rsp;
  uint64_t temp_rax;
  uint64_t user_sp_temp;
  uint64_t cpu_id;
  struct process *current_proc;
  /* GB-147: the frame the CPU pushed, copied by the FIRST instruction of
     every exception stub (before any prologue).  The offsets are
     load-bearing: FRAMESNAP_ASM addresses them as gs:[48]..gs:[104]. */
  uint64_t snap_rip;     /* 48 */
  uint64_t snap_cs;      /* 56 */
  uint64_t snap_rflags;  /* 64 */
  uint64_t snap_rsp;     /* 72  (CPL3 only; 0 for a kernel-mode entry) */
  uint64_t snap_ss;      /* 80  (CPL3 only; 0 for a kernel-mode entry) */
  uint64_t snap_valid;   /* 88 */
  uint64_t snap_count;   /* 96 */
  uint64_t fault_count;  /* 104 */
} __attribute__((packed));

/* The stub asm cannot see sizeof(): pin the offsets it uses. */
typedef char gb147_off48[(__builtin_offsetof(struct cpu_local, snap_rip) == 48) ? 1 : -1];
typedef char gb147_off56[(__builtin_offsetof(struct cpu_local, snap_cs) == 56) ? 1 : -1];
typedef char gb147_off64[(__builtin_offsetof(struct cpu_local, snap_rflags) == 64) ? 1 : -1];
typedef char gb147_off72[(__builtin_offsetof(struct cpu_local, snap_rsp) == 72) ? 1 : -1];
typedef char gb147_off80[(__builtin_offsetof(struct cpu_local, snap_ss) == 80) ? 1 : -1];
typedef char gb147_off88[(__builtin_offsetof(struct cpu_local, snap_valid) == 88) ? 1 : -1];
typedef char gb147_off96[(__builtin_offsetof(struct cpu_local, snap_count) == 96) ? 1 : -1];
typedef char gb147_off104[(__builtin_offsetof(struct cpu_local, fault_count) == 104) ? 1 : -1];
/* current_proc is read as gs:40 by current_process(); pin it too. */
typedef char cpu_local_cur40[(__builtin_offsetof(struct cpu_local, current_proc) == 40) ? 1 : -1];

extern struct cpu_local cpu_locals[];

#endif /* __x86_64__ */

#endif /* ARCH_CPU_LOCAL_H */
