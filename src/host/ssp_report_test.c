/*
 * ssp_report_test.c - Host unit tests for the SEC-4 cannot report (GB-246).
 *
 * The kernel's __stack_chk_fail halts, so the falsifier drives the two pure
 * pieces the panic path uses:
 *   - ssp_prologue_canary_offset(): the x86_64 prologue -> canary-slot rule,
 *     parsed from synthetic bytes (push %rbp; mov %rsp,%rbp; pushes...);
 *   - ssp_format_report(): the panic text, including expected (guard) vs
 *     found and the arithmetic delta, and the UNRESOLVED form.
 *
 * A synthetic smashed frame (found != guard) must produce the pc, guard,
 * found and delta fields -- proving the extension reports rather than
 * crashes silently.
 */
#include <stdio.h>
#include <string.h>

#include "ssp.h"

static int checks_run = 0;
static int checks_failed = 0;

static void check(int cond, const char *name) {
  checks_run++;
  if (!cond) {
    checks_failed++;
    printf("FAIL %s\n", name);
  } else {
    printf("ok %s\n", name);
  }
}

int main(void) {
  /* push %rbp; mov %rsp,%rbp; push %rbx; push %r12; push %r13; push %r14;
   * push %r15 -> five saved registers, canary at 8*(5+1) = 0x30. */
  unsigned char five[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x41, 0x54,
                          0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48};
  /* No saved registers -> canary at 0x08. */
  unsigned char none[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x10};
  /* Not a frame-pointer prologue -> 0 (the report then says UNRESOLVED). */
  unsigned char other[] = {0x48, 0x83, 0xec, 0x18, 0x90, 0x90, 0x90, 0x90};

  check(ssp_prologue_canary_offset(five, (int)sizeof five) == 0x30,
        "prologue: five saved regs -> canary at rbp-0x30");
  check(ssp_prologue_canary_offset(none, (int)sizeof none) == 0x08,
        "prologue: no saved regs -> canary at rbp-0x08");
  check(ssp_prologue_canary_offset(other, (int)sizeof other) == 0,
        "prologue: non-fp prologue -> unresolved");

  /* A synthetic smashed frame: guard 0x1111... and found 0x9090... */
  char buf[384];
  int n = ssp_format_report(buf, (int)sizeof buf, 0x0000000070002e6bULL,
                            0x0000000025ffb600ULL, 0x0000000025ffb700ULL,
                            0x123456789abcdef0ULL, 0x9090909090909090ULL, 1);
  check(n > 0 && strlen(buf) == (size_t)n, "format: returns the written length");
  check(strstr(buf, "KERNEL PANIC: stack smashing detected (SEC-4 canary)") != NULL,
        "format: panic line");
  check(strstr(buf, "smasher pc=0x0000000070002e6b") != NULL,
        "format: faulting return address printed");
  check(strstr(buf, "call_site=0x0000000070002e66") != NULL,
        "format: call-site address printed (pc - 5)");
  check(strstr(buf, "guard=0x123456789abcdef0") != NULL,
        "format: expected canary printed");
  check(strstr(buf, "found=0x9090909090909090") != NULL,
        "format: found canary printed");
  check(strstr(buf, "delta=") != NULL && strstr(buf, "UNRESOLVED") == NULL,
        "format: delta computed for a smashed slot");

  n = ssp_format_report(buf, (int)sizeof buf, 0x70000000ULL, 0x1000ULL, 0ULL,
                        0x1234ULL, 0ULL, 0);
  check(strstr(buf, "found=UNRESOLVED delta=UNRESOLVED") != NULL,
        "format: unresolved slot is explicit, not silent");

  printf("Checks run: %d / Checks failed: %d\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
