/*
 * src/user/linux_probe_redzone.c — Linux-ABI probe: the 128-byte RED ZONE
 * below RSP must survive everything the kernel does to a thread.
 *
 * ORDINARY musl Linux program: no HobbyOS header, so the same static-PIE
 * binary runs on the workstation (reference kernel) and inside the intel
 * guest (HobbyOS's compat layer).  Rows are "[LP] ..." and the last is
 * "[LP] done rc=<fails>"; tools/gate/t3_linux_probe.sh redzone accepts rc=0
 * only.
 *
 * WHY THIS EXISTS
 * ---------------
 * The x86-64 psABI lets a leaf function keep LIVE data at [rsp-128, rsp)
 * without adjusting RSP.  HobbyOS's x64 user-resume path builds its iretq
 * frame by copying a 37-qword trap-frame image onto the RESUMED TASK'S OWN
 * USER STACK at [target_sp-320, target_sp) (src/kernel/arch/x64/trap.c,
 * enter_user_space).  That window covers the whole red zone, so every timer
 * preemption of a leaf function silently overwrote its locals.  Measured in
 * RimWorld: UnityPlayer+0x91b10e -> `mov -0x50(%rsp),%rdi` loaded the
 * thread's OWN RSP (0x17FFFFB3D8, the value the resume wrote at
 * dest+240 = tf->lr), `add %esi,%edi` turned it into a negative int and
 * `movdqu %xmm0,(%rax,%rdi,8)` wrote to VA 0x2882A10FF8 — 4 of 6 launch runs
 * died before the game's banner.
 *
 * WHAT EACH ROW PROVES
 * --------------------
 * Six qwords are planted in the red zone at -0x08..-0x120 (the iretq SS/RSP
 * slots land on the first two pre-fix, so the evidence names them), then:
 *
 *   control pass  iters = 0: store and load with NO loop between.  All six
 *                 must read back unchanged on any kernel — this proves the
 *                 measurement itself (store/load offsets, asm isolation) is
 *                 sound.  A control failure is reported as a PROBE DEFECT and
 *                 fails the gate, so a green row can never come from a broken
 *                 probe.
 *   spin pass     iters = 120,000,000: ~0.2-0.6 s of a 3-instruction loop,
 *                 so at 100 Hz several timer ticks MUST land inside it.  On
 *                 Linux all six survive.  Pre-fix HobbyOS overwrote them with
 *                 the resume image: -0x08 read back 0x23 (SS), -0x10 read back
 *                 the thread's own RSP, -0x50 the same, and the rest came back
 *                 as trap-frame words (0x0, 0x100000000, ...).
 *
 * The spin case runs twice on the leader and once on a pthread, because the
 * game's clobber was measured on a worker thread as well.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The psABI red zone is [rsp-128, rsp): these six slots are all inside it and
 * a correct kernel must leave every one alone.  -0x90/-0x120 are BELOW the red
 * zone (unallocated stack a leaf function may not use); they are reported as
 * `info` rows because HobbyOS's resume scratch lives there, and they are never
 * counted as failures. */
#define SLOTS 8
#define REQUIRED 6

static const int slot_off[SLOTS] = {-0x08, -0x10, -0x20, -0x30,
                                    -0x50, -0x78, -0x90, -0x120};

static int fails;
static int rows;

static void report(const char *tag, int spinning, uint64_t magic,
                   const uint64_t *vals, uint64_t sp) {
  for (int i = 0; i < SLOTS; i++) {
    uint64_t v = vals[i];
    int kept = (v == magic);
    char extra[96];
    extra[0] = 0;
    if (!kept) {
      if (v >= sp - 0x1000 && v <= sp + 0x1000)
        snprintf(extra, sizeof extra, " off=%+ld", (long)((int64_t)v - (int64_t)sp));
      else
        snprintf(extra, sizeof extra, " got=0x%llx", (unsigned long long)v);
    }
    printf("[LP] %s slot %+4d %-9s %-6s %-9s%s\n", tag, slot_off[i],
           spinning ? "spin" : "control",
           (i < REQUIRED) ? "redzone" : "info",
           kept ? "KEPT" : "CLOBBERED", extra);
    if (i >= REQUIRED) continue;
    rows++;
    if (!kept) {
      fails++;
      printf("[LP] %s slot %+4d %s\n", tag, slot_off[i],
             spinning ? "<-- REGRESSION: red zone did not survive preemption"
                      : "<-- PROBE DEFECT: control slot moved without a "
                        "preemption window");
    }
  }
}

/* Plant six magics in the red zone, optionally spin, read them back.  One asm
 * block so nothing the compiler emits can sit between the store and the load
 * (with iters == 0 the loop body is skipped by the initial test). */
static __attribute__((noinline)) void rz_hold(uint64_t magic, uint64_t iters,
                                              uint64_t *vals, uint64_t *sp_out) {
  __asm__ __volatile__(
      "movq %[m], -0x08(%%rsp)\n\t"
      "movq %[m], -0x10(%%rsp)\n\t"
      "movq %[m], -0x30(%%rsp)\n\t"
      "movq %[m], -0x50(%%rsp)\n\t"
      "movq %[m], -0x20(%%rsp)\n\t"
      "movq %[m], -0x78(%%rsp)\n\t"
      "movq %[m], -0x90(%%rsp)\n\t"
      "movq %[m], -0x120(%%rsp)\n\t"
      "movq %%rsp, %[sp]\n\t"
      "xorl %%eax, %%eax\n\t"
      "testq %[n], %[n]\n\t"
      "jz 2f\n"
      "1:\n\t"
      "addq $1, %%rax\n\t"
      "cmpq %[n], %%rax\n\t"
      "jb 1b\n\t"
      "2:\n\t"
      "movq %[o], %%r10\n\t"
      "movq -0x08(%%rsp), %%rax\n\t"
      "movq %%rax, 0(%%r10)\n\t"
      "movq -0x10(%%rsp), %%rax\n\t"
      "movq %%rax, 8(%%r10)\n\t"
      "movq -0x20(%%rsp), %%rax\n\t"
      "movq %%rax, 16(%%r10)\n\t"
      "movq -0x30(%%rsp), %%rax\n\t"
      "movq %%rax, 24(%%r10)\n\t"
      "movq -0x50(%%rsp), %%rax\n\t"
      "movq %%rax, 32(%%r10)\n\t"
      "movq -0x78(%%rsp), %%rax\n\t"
      "movq %%rax, 40(%%r10)\n\t"
      "movq -0x90(%%rsp), %%rax\n\t"
      "movq %%rax, 48(%%r10)\n\t"
      "movq -0x120(%%rsp), %%rax\n\t"
      "movq %%rax, 56(%%r10)\n\t"
      : [sp] "=&r"(*sp_out)
      : [m] "r"(magic), [n] "r"(iters), [o] "r"(vals)
      : "rax", "r10", "cc", "memory");
}

static void *thread_main(void *arg) {
  (void)arg;
  uint64_t vals[SLOTS], sp = 0;
  /* A worker thread takes its own timer preemptions; the game's clobber was
   * measured on a worker (pid 4), not the leader. */
  rz_hold(0xA5A5F00DCAFEBABEull, 120000000ull, vals, &sp);
  report("thread", 1, 0xA5A5F00DCAFEBABEull, vals, sp);
  return NULL;
}

int main(void) {
  uint64_t vals[SLOTS], sp = 0;
  printf("[LP] redzone: psABI [rsp-128,rsp) must survive timer preemption\n");
  printf("[LP] pre-fix resume window [target_sp-320,target_sp) covers all six "
         "slots; after the fix the window ends at target_sp-128\n");

  /* Control: no preemption window between the store and the load. */
  rz_hold(0xC0C0C0C000000001ull, 0, vals, &sp);
  report("ctl", 0, 0xC0C0C0C000000001ull, vals, sp);

  /* Regression rows: the spin is long enough that several 100 Hz ticks land
   * inside it, so the old kernel loses the slots deterministically. */
  for (int pass = 0; pass < 2; pass++) {
    char tag[16];
    uint64_t magic = 0x5A5A5A5A00000000ull | (uint64_t)(pass + 1);
    snprintf(tag, sizeof tag, "main%d", pass + 1);
    rz_hold(magic, 120000000ull, vals, &sp);
    report(tag, 1, magic, vals, sp);
  }

  pthread_t th;
  if (pthread_create(&th, NULL, thread_main, NULL) == 0)
    pthread_join(th, NULL);
  else
    printf("[LP] thread: pthread_create failed (skipped)\n");

  printf("[LP] rows=%d fails=%d\n", rows, fails);
  printf("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
