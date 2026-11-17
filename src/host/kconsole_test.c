/*
 * kconsole_test.c -- host golden test for the line-atomic console
 * (src/kernel/kconsole.c, C0-console, docs/cp-console.md).
 *
 * The arch hooks are stubs: the wire is a buffer, the CPU number, running
 * thread, interrupt state, stack pointer and clock are test variables, so the
 * interleavings that shredded integ7's console are replayed exactly.
 */
#include <stdio.h>
#include <string.h>

#include "console.h"
#include "cons_host_shim.h"

static char wire[1 << 16];
static int wn;
static uint32_t cur_cpu;
static const void *cur_ctx[MAX_CPUS];
static char ctxs[1024];
static char stackbuf[0x20000];
static int cur_irqon = 1;
static uint64_t now_us = 1000;
static int halted[MAX_CPUS];
const char *cons_host_sp = stackbuf + 0x10000;
static int fails;

uint32_t get_cpuid(void) { return cur_cpu; }
uint64_t timer_get_us(void) { return now_us += 1; }
int cpu_is_halted(uint32_t c) { return c < MAX_CPUS && halted[c]; }

void cons_hw_tx(const char *s, int n) {
  for (int i = 0; i < n && wn < (int)sizeof wire - 1; i++)
    wire[wn++] = s[i];
  wire[wn] = 0;
}
uint64_t cons_hw_irqsave(void) { int was = cur_irqon; cur_irqon = 0; return (uint64_t)was; }
void cons_hw_irqrestore(uint64_t f) { cur_irqon = (int)f; }
int cons_hw_irqs_on(uint64_t f) { return f != 0; }
const void *cons_hw_ctx(uint32_t cpu) { return cur_ctx[cpu]; }
int cons_hw_in_panic(void) { return 0; }
int cons_hw_bootarg(char *b, int cap) { (void)b; (void)cap; return 0; }
void cons_hw_relax(void) { now_us += 50; }

static void reset(void) { wn = 0; wire[0] = 0; }
static void on(uint32_t cpu, int ctx) { cur_cpu = cpu; cur_ctx[cpu] = ctx ? (const void *)&ctxs[ctx] : 0; }
static void kw(const char *s) { cons_kwrite(s, -1); }

#define EXPECT(name, want)                                                  \
  do {                                                                      \
    if (strcmp(wire, want) != 0) {                                          \
      printf("FAIL %s\n  want: %s\n  got:  %s\n", name, want, wire);        \
      fails++;                                                              \
    } else {                                                                \
      printf("ok   %s\n", name);                                            \
    }                                                                       \
  } while (0)

int main(void) {
  cons_diag_mask = 0; /* the default build: every diagnostic class off */

  /* T1 the integ7 shape: a multi-call row on CPU0, whole rows from CPU1 in
     between its pieces.  Before: "[BLKD] cc=[PUB] ..." welded. */
  reset();
  on(0, 100); kw("[A] begin x=");
  on(1, 200); kw("[B] whole row\n");
  on(0, 100); kw("1"); kw(" y=2\n");
  EXPECT("T1 multi-call row stays whole", "[B] whole row\n[A] begin x=1 y=2\n");

  /* T2 an interrupt on the same CPU, mid-row: masked IRQs AND a far stack */
  reset();
  on(0, 100); cur_irqon = 1; cons_host_sp = stackbuf + 0x10000; kw("[A] part1 ");
  cur_irqon = 0; cons_host_sp = stackbuf + 0x10000 - 900; kw("  CSTK cpu4: 0x1 0x2\n");
  cur_irqon = 1; cons_host_sp = stackbuf + 0x10000; kw("part2\n");
  EXPECT("T2 IRQ row on the same CPU gets its own line", "  CSTK cpu4: 0x1 0x2\n[A] part1 part2\n");

  /* T3 a row printed across an irqsave lock: IRQ state flips, stack does not */
  reset();
  on(0, 100); cur_irqon = 1; cons_host_sp = stackbuf + 0x10000; kw("[A] locked=");
  cur_irqon = 0; cons_host_sp = stackbuf + 0x10000 - 64; kw("7");
  cur_irqon = 1; cons_host_sp = stackbuf + 0x10000; kw("\n");
  EXPECT("T3 row across an irqsave lock stays one line", "[A] locked=7\n");

  /* T4 another thread on the same CPU (kernel thread preempted mid-row) */
  reset();
  on(0, 100); kw("[A] first ");
  on(0, 300); kw("[C] other thread\n");
  on(0, 100); kw("half\n");
  EXPECT("T4 preempted thread's row is not merged", "[C] other thread\n[A] first half\n");

  /* T5 the diagnostic filter */
  reset();
  on(2, 0);
  kw("[PUB] who=fault_obj va=0x1\n");
  kw("[PUB!] who=img alarm\n");
  kw("[PUBS] n=4096\n");
  kw("[MR] s=1 pid=2\n");
  kw("[MR] FAIL pid=2\n");
  kw("  [ALIVE-SET] slot=3 pid=3\n");
  kw("[ALIVE-SET] no READY work\n");
  kw("[FUTEX] map base=0x1\n");
  kw("[BLKD] cc=0x1 csts=0x0\n");
  kw("[BLKD] data disk nsid=2\n");
  kw("[RESUME-RZ] pid=1\n");
  kw("[KERNEL] FATAL: kept\n");
  EXPECT("T5 filter drops only the diagnostic rows",
         "[PUB!] who=img alarm\n[PUBS] n=4096\n[MR] FAIL pid=2\n[ALIVE-SET] no READY work\n"
         "[BLKD] data disk nsid=2\n[KERNEL] FATAL: kept\n");
  if (cons_stats.dropped[0] != 1 || cons_stats.dropped[1] != 1 || cons_stats.dropped[3] != 1) {
    printf("FAIL T5 dropped counters pub=%llu mr=%llu alive=%llu\n",
           (unsigned long long)cons_stats.dropped[0], (unsigned long long)cons_stats.dropped[1],
           (unsigned long long)cons_stats.dropped[3]);
    fails++;
  }
  cons_diag_mask = cons_diag_parse("pub,mr", -1);
  reset();
  kw("[PUB] who=x\n");
  kw("[MR] s=1\n");
  kw("[FUTEX] map\n");
  EXPECT("T5b CP_DIAG=pub,mr", "[PUB] who=x\n[MR] s=1\n");
  cons_diag_mask = cons_diag_parse("all", -1);
  reset();
  kw("[FUTEX] map\n");
  EXPECT("T5c CP_DIAG=all", "[FUTEX] map\n");
  cons_diag_mask = 0;

  /* T6 a user line written in three write() calls, a kernel row between */
  reset();
  on(3, 0);
  cons_user_write(6, "[X] ", 4);
  cons_user_write(6, "[P5] end d3d11 status=0", 23);
  on(1, 200); kw("[SIGR] pid=228 sig=3\n");
  on(3, 0); cons_user_write(6, " ms=5\n", 6);
  EXPECT("T6 user line in pieces stays whole", "[SIGR] pid=228 sig=3\n[X] [P5] end d3d11 status=0 ms=5\n");

  /* T7 a stale user prompt goes out; the next kernel row starts a new line */
  reset();
  on(0, 0);
  cons_user_write(9, "$ ", 2);
  now_us += 40000;
  cons_tick(0);
  kw("[K] row\n");
  cons_user_write(9, "ls\n", 3);
  EXPECT("T7 stale prompt flushed, kernel row on its own line", "$ \n[K] row\nls\n");

  /* T8 several lines in one user write: one hold, nothing between them */
  reset();
  on(4, 0);
  cons_user_write(11, "[LPC] a\n[LPC] b\n[LPC] c\n", 24);
  EXPECT("T8 multi-line write", "[LPC] a\n[LPC] b\n[LPC] c\n");

  /* T9 the lock held by a halted CPU is taken over at once */
  reset();
  halted[5] = 1;
  cons_test_set_owner(6);
  on(0, 0); kw("[K] after halt\n");
  EXPECT("T9 halted holder broken", "\n[K] after halt\n");
  halted[5] = 0;

  /* T10 a live holder past the bound: write anyway, tagged */
  reset();
  cons_test_set_owner(3);
  on(0, 0); kw("[K] forced\n");
  EXPECT("T10 live holder past the bound -> [CONS!]", "[CONS!] [K] forced\n");
  cons_test_set_owner(0);

  /* T11 a line longer than a slot goes out in pieces, and still in order */
  reset();
  on(2, 0);
  {
    static char big[1500];
    memset(big, 'z', sizeof big - 2);
    big[sizeof big - 2] = '\n';
    big[sizeof big - 1] = 0;
    kw(big);
    if (wn != (int)sizeof big - 1 || wire[wn - 1] != '\n') {
      printf("FAIL T11 long line: %d bytes on the wire, want %d\n", wn, (int)sizeof big - 1);
      fails++;
    } else {
      printf("ok   T11 long line\n");
    }
  }

  /* T12 a stale kernel partial (a row without its newline) is pushed out */
  reset();
  on(6, 70); cur_irqon = 1; kw("Booting...");
  now_us += 60000;
  on(6, 71); /* its thread is not the one running when the tick comes */
  cons_tick(6);
  EXPECT("T12 stale kernel partial flushed", "Booting...");
  on(6, 70); kw(" done\n");
  EXPECT("T12b continuation joins it", "Booting... done\n");

  /* T13 the tick interrupts the very thread that is mid-row, after a quiet
     60 ms (a host stall of the whole guest): the row is left alone */
  reset();
  on(5, 80); cur_irqon = 1; kw("[CSTR] c=5 k=1 s=1 p=abc");
  now_us += 60000;
  cons_tick(5);
  EXPECT("T13 interrupted thread's row not pushed", "");
  kw(" e=3\n");
  EXPECT("T13b and it ends whole", "[CSTR] c=5 k=1 s=1 p=abc e=3\n");

  printf("kconsole_test: %s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
