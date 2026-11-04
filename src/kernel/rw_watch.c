/*
 * src/kernel/rw_watch.c — x86 hardware write watchpoint (DR0/DR7) for one
 * user word.  Diagnostic; compiled out unless RW_WATCH_ADDR is non-zero.
 *
 * See src/include/rw_watch.h for why a write trap is the only instrument that
 * can name the holder of a glibc NORMAL mutex: the futex queue knows the
 * waiter, the interposed pthread entry points know PLT callers, and neither
 * sees the store that set __lock.
 *
 * Encoding (Intel SDM, DR7):
 *   L0   bit 0      = 1        local enable for DR0
 *   RW0  bits 16:17 = 01b      break on data writes only
 *   LEN0 bits 18:19 = 11b      -> 4 bytes (10b = 8 bytes)
 *   GD   bit 13     = 0        (GD=1 makes every MOV to DR raise #DB)
 * so 4-byte: DR7 = 0x000D0001, 8-byte: DR7 = 0x00090001.
 *
 * DR6.B0 is the hit flag; the handler must clear DR6 or the next #DB on this
 * CPU looks like the same hit forever.  Data breakpoints are traps: the store
 * has already retired when the handler runs, so the word holds the NEW value
 * and rip is the next instruction.
 */
#include <stdint.h>

#include "arch/cpu.h"
#include "arch/trap.h"
#include "kline.h"
#include "process.h"
#include "rw_watch.h"

#if RW_WATCH_ADDR

#ifndef RW_WATCH_LEN
#define RW_WATCH_LEN 4
#endif

#if RW_WATCH_LEN == 8
#define RW_DR7_WORD 0x00090001UL /* L0=1, RW0=01 (write), LEN0=10 (8 bytes) */
#else
#define RW_DR7_WORD 0x000D0001UL /* L0=1, RW0=01 (write), LEN0=11 (4 bytes) */
#endif

static int l_armed[MAX_CPUS]; /* per-CPU: DR0/DR7 programmed by this module */
static unsigned l_hits;       /* writes trapped */
static unsigned l_rows;       /* rows printed */
static int l_park_dumped;     /* the park-moment memory picture is one-shot */
#ifdef RW_WATCH_FIXUP
static unsigned l_fixups;     /* counterfactual forced acquisitions */
#endif

/* Boot-time proof that a data breakpoint actually traps on this kernel before
 * any conclusion is drawn from "no rows": arm DR0 on a scratch word, write it,
 * and report whether #DB arrived.  A silent mechanism and a never-written word
 * look identical otherwise. */
static volatile uint32_t l_selftest_word;
static volatile int l_selftest_hit;
static volatile int l_in_selftest;

static inline uint64_t rd_dr6(void) {
  uint64_t v;
  __asm__ volatile("mov %%dr6, %0" : "=r"(v));
  return v;
}
static inline void wr_dr6(uint64_t v) {
  __asm__ volatile("mov %0, %%dr6" : : "r"(v));
}
static inline void wr_dr0(uint64_t v) {
  __asm__ volatile("mov %0, %%dr0" : : "r"(v));
}
static inline void wr_dr7(uint64_t v) {
  __asm__ volatile("mov %0, %%dr7" : : "r"(v));
}

void rw_watch_arm_cpu(void) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS) return;
  if (l_armed[c]) return;
  wr_dr0((uint64_t)RW_WATCH_ADDR);
  wr_dr6(0);
  wr_dr7(RW_DR7_WORD);
  l_armed[c] = 1;
  {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[WATCH] armed cpu=");
    kl_i(&kl, (long)c);
    kl_s(&kl, " addr=");
    kl_hex(&kl, (unsigned long)RW_WATCH_ADDR);
    kl_s(&kl, " len=");
    kl_i(&kl, (long)RW_WATCH_LEN);
    kl_s(&kl, " dr7=");
    kl_hex(&kl, (unsigned long)RW_DR7_WORD);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  /* Self-test on the boot CPU only: point DR0 at a kernel scratch word, write
     it, and see whether the #DB path runs.  Restores DR0 afterwards. */
  if (c == 0 && !l_selftest_hit && l_hits == 0) {
    l_in_selftest = 1;
    wr_dr0((uint64_t)&l_selftest_word);
    wr_dr6(0);
    l_selftest_word = 0x5A5A5A5AU;
    (void)l_selftest_word;
    l_in_selftest = 0;
    wr_dr0((uint64_t)RW_WATCH_ADDR);
    wr_dr6(0);
    {
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[WATCH] selftest #DB=");
      kl_s(&kl, l_selftest_hit ? "FIRED" : "MISSED");
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
}

int rw_watch_db(struct trap_frame *tf) {
  uint64_t dr6 = rd_dr6();
  if (!(dr6 & 1u)) return 0; /* not DR0: not ours */
  wr_dr6(0);

  if (l_in_selftest) {
    l_selftest_hit = 1;
    return 1;
  }

  l_hits++;

#ifdef RW_WATCH_FIXUP
  /* Counterfactual (diagnostic only): the word holds neither 0 (free) nor 1/2
   * (locked) but a pool link.  If a *free* mutex is the only thing between the
   * game and window creation, pretending this cmpxchg succeeded answers that
   * in one boot: zero the word and set ZF in the saved rflags so
   * __pthread_mutex_lock's fast path continues as if it had acquired it.
   * Restricted to the 4-byte `lock cmpxchg %edx,(%rdi)` (f0 0f b1 17) that
   * precedes the rip, so the allocator's ordinary stores are never touched. */
  if ((tf->cs & 3) == 3 && l_fixups < 8 &&
      *(volatile uint32_t *)(unsigned long)(tf->elr - 4) == 0x17B10FF0u) {
    unsigned long v = (unsigned long)*(volatile uint32_t *)(unsigned long)RW_WATCH_ADDR;
    if (v > 2 && v != 0xFFFFFFFFUL) {
      l_fixups++;
      *(volatile uint32_t *)(unsigned long)RW_WATCH_ADDR = 0;
      tf->spsr |= 0x40u; /* ZF: "the cmpxchg compared equal" */
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[WATCH] FIXUP n=");
      kl_i(&kl, (long)l_fixups);
      kl_s(&kl, " was=");
      kl_hex(&kl, v);
      kl_s(&kl, " rip=");
      kl_hex(&kl, (unsigned long)tf->elr);
      kl_s(&kl, " -> word=0, ZF forced (counterfactual, not a fix)\n");
      kl_emit(&kl);
    }
  }
#endif

  /* Park-moment memory picture: the store of 2 is glibc's waiter-bit xchg
   * inside __lll_lock_wait, i.e. the last write before the park.  Dump the
   * block around the word so the report can say what the "mutex" memory
   * actually is (0x30-stride pool chain vs a live object) without a second
   * boot. */
  if ((tf->cs & 3) == 3 && l_park_dumped == 0) {
    unsigned long w = 0;
    if ((tf->cs & 3) == 3) w = (unsigned long)*(volatile uint32_t *)(unsigned long)RW_WATCH_ADDR;
    if (w == 2) {
      l_park_dumped = 1;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[WATCH] parkmem addr=");
      kl_hex(&kl, (unsigned long)RW_WATCH_ADDR);
      kl_s(&kl, " -0x30..+0x50:");
      for (int i = -6; i < 10; i++) {
        unsigned long v = 0;
        v = (unsigned long)*(volatile uint64_t *)((unsigned long)RW_WATCH_ADDR + (unsigned long)(i * 8));
        kl_c(&kl, ' ');
        kl_hex(&kl, v);
      }
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }

  if (l_rows < RW_WATCH_ROWS) {
    struct process *cur = current_process();
    /* The store already retired and the faulting AS is still loaded, so this
       read is of a page the CPU just wrote.  Only read it for a user-mode
       trap: a kernel-mode write to this VA in another AS is not the game. */
    unsigned long val = 0;
    if ((tf->cs & 3) == 3) val = (unsigned long)*(volatile uint32_t *)(unsigned long)RW_WATCH_ADDR;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[WATCH] wr n=");
    kl_i(&kl, (long)l_hits);
    kl_s(&kl, " pid=");
    kl_i(&kl, cur ? (long)cur->pid : -1L);
    kl_s(&kl, " name=");
    kl_s(&kl, cur ? cur->name : "?");
    kl_s(&kl, " cpu=");
    kl_i(&kl, (long)get_cpuid());
    kl_s(&kl, " cs=");
    kl_i(&kl, (long)(tf->cs & 3));
    kl_s(&kl, " rip=");
    kl_hex(&kl, (unsigned long)tf->elr);
    kl_s(&kl, " val=");
    kl_hex(&kl, val);
    kl_s(&kl, " rax=");
    kl_hex(&kl, (unsigned long)tf->regs[0]);
    kl_s(&kl, " rbx=");
    kl_hex(&kl, (unsigned long)tf->regs[1]);
    kl_s(&kl, " rcx=");
    kl_hex(&kl, (unsigned long)tf->regs[2]);
    kl_s(&kl, " rdx=");
    kl_hex(&kl, (unsigned long)tf->regs[3]);
    kl_s(&kl, " rsi=");
    kl_hex(&kl, (unsigned long)tf->regs[4]);
    kl_s(&kl, " rdi=");
    kl_hex(&kl, (unsigned long)tf->regs[5]);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    l_rows++;
  } else if (l_rows == RW_WATCH_ROWS) {
    l_rows++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[WATCH] suppressing further rows after ");
    kl_i(&kl, (long)RW_WATCH_ROWS);
    kl_s(&kl, " hits=");
    kl_i(&kl, (long)l_hits);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  return 1;
}

#else /* !RW_WATCH_ADDR */

void rw_watch_arm_cpu(void) {}
int rw_watch_db(struct trap_frame *tf) {
  (void)tf;
  return 0;
}

#endif
