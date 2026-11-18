/*
 * src/user/linux_probe_sigret.c — rt_sigreturn into an unmapped (but legal,
 * canonical user) RIP must resume there and fault normally (H4-kglitch,
 * docs/cp-kglitch.md §6; H3-winefix's log mine: "[LCOMPAT] rt_sigreturn: bad
 * rip/rsp -> SIGSEGV", then Wine's "err:seh:segv_handler Got unexpected trap
 * 128" and "nested exception on signal stack" in 8 game Venus boots).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 * Linux does not check the restored RIP: a handler that returns into an
 * unmapped address takes a page fault there (SIGSEGV, si_addr = that address,
 * uc_mcontext.gregs[REG_TRAPNO] = 14).  Wine's segv_handler switches on
 * REG_TRAPNO; the kernel used to refuse the frame and raise SIGSEGV from the
 * rt_sigreturn syscall itself, with the SYSCALL frame's vector (0x80 = 128)
 * as the trap number.
 *
 *   S1  the first fault (call through a NULL pointer): trapno 14
 *   S2  the handler returns into BAD_RIP (unmapped): a second SIGSEGV arrives
 *       with si_addr == BAD_RIP and trapno 14 (not 128)
 *   S3  the handler then returns into a recovery function on a fresh stack,
 *       which longjmps back: the process survives
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define BAD_RIP 0x0000500000001000ULL

static jmp_buf back;
static volatile int entries;
static volatile long trap1 = -1, trap2 = -1;
static volatile uint64_t addr2;
static unsigned char altstk[65536] __attribute__((aligned(16)));

static void recover(void) { longjmp(back, 1); }

static void on_segv(int sig, siginfo_t *si, void *ctx) {
  (void)sig;
  ucontext_t *uc = ctx;
  int n = ++entries;
  if (n == 1) {
    trap1 = (long)uc->uc_mcontext.gregs[REG_TRAPNO];
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)BAD_RIP; /* return into a hole */
  } else {
    trap2 = (long)uc->uc_mcontext.gregs[REG_TRAPNO];
    addr2 = (uint64_t)si->si_addr;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)recover;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(uintptr_t)(altstk + sizeof altstk - 8);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGSEGV, &sa, 0);
  int fails = 0;
  if (!setjmp(back)) {
    void (*volatile nullfn)(void) = 0;
    nullfn();
    printf("[LP] S3 returned past the fault -> FAIL\n");
    fails++;
  }
  int ok1 = entries >= 1 && trap1 == 14;
  int ok2 = entries == 2 && trap2 == 14 && addr2 == BAD_RIP;
  printf("[LP] S1 %-44s trapno=%ld -> %s\n", "fault through a NULL pointer", trap1, ok1 ? "PASS" : "FAIL");
  printf("[LP] S2 %-44s entries=%d trapno=%ld si_addr=0x%llx -> %s\n", "sigreturn into an unmapped RIP faults there",
         entries, trap2, (unsigned long long)addr2, ok2 ? "PASS" : "FAIL");
  printf("[LP] S3 %-44s -> PASS\n", "recovered through the second frame");
  fails += !ok1 + !ok2;
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
