/* SEC-4 (GB-246): stack-canary panic report formatting and frame decoding.
 *
 * GB-244's 10-wave record showed the SEC-4 panic fires in 5 of 10 intel waves
 * but names nothing: `KERNEL PANIC: stack smashing detected (SEC-4 canary)`
 * and a halt.  The next occurrence has to name the smasher and show the
 * expected-vs-found canary, or every wave is another guess.
 *
 * The report carries the return address INSIDE the faulting function (`pc`),
 * which `llvm-addr2line -e hobbyos.elf <pc>` resolves to file:line:function
 * offline; `%rsp` and `%rbp`; the guard (expected); and the canary slot found
 * by decoding the function's prologue (found) with its arithmetic delta.
 *
 * The decoder relies on this kernel's codegen: every function whose canary
 * check compiles to `cmpq -X(%rbp), %rax` starts with
 *   push %rbp; mov %rsp,%rbp; push <callee-saved>...
 * and clang stores the canary immediately below the saved registers, so the
 * slot offset is 8 * (pushed-register count + 1).  ssp.c scans backwards from
 * the return address for `55 48 89 e5` to find the function start, then parses
 * the pushes.  The host falsifier drives this parser with a synthetic
 * prologue, so the layout rule is itself a check rather than an assumption.
 */
#include <stdint.h>

#include <ssp.h>

/* x86_64 prologue scan.  `len` bounds the parser; callers pass the bytes
 * that follow the prologue (the compiler emits the pushes immediately). */
int ssp_prologue_canary_offset(const unsigned char *code, int len) {
#ifdef __x86_64__
  int i, nregs = 0;
  if (len < 4)
    return 0;
  if (code[0] != 0x55 || code[1] != 0x48 || code[2] != 0x89 || code[3] != 0xe5)
    return 0;
  i = 4;
  while (i < len) {
    /* push %rbx / %r12-%r15 (the callee-saved set clang spills here). */
    if (code[i] == 0x53) {
      nregs++;
      i++;
    } else if (code[i] == 0x41 && i + 1 < len && code[i + 1] >= 0x54 &&
               code[i + 1] <= 0x57) {
      nregs++;
      i += 2;
    } else {
      break;
    }
  }
  return 8 * (nregs + 1);
#else
  (void)code;
  (void)len;
  return 0;
#endif
}

uint64_t ssp_pc_canary_offset(uint64_t pc) {
#ifdef __x86_64__
  uint64_t i;
  for (i = 1; i <= 8192; i++) {
    uint64_t a = pc - i;
    const unsigned char *p = (const unsigned char *)a;
    if (p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xe5) {
      int off = ssp_prologue_canary_offset(p, 32);
      if (off > 0)
        return (uint64_t)off;
    }
  }
#else
  (void)pc;
#endif
  return 0;
}

static int putc_buf(char *out, int cap, int n, char c) {
  if (n < cap - 1)
    out[n] = c;
  return n + 1;
}

static int puts_buf(char *out, int cap, int n, const char *s) {
  while (*s)
    n = putc_buf(out, cap, n, *s++);
  return n;
}

static int puthex_buf(char *out, int cap, int n, uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  int i;
  n = puts_buf(out, cap, n, "0x");
  for (i = 60; i >= 0; i -= 4)
    n = putc_buf(out, cap, n, digits[(v >> (unsigned)i) & 0xf]);
  return n;
}

int ssp_format_report(char *out, int cap, uint64_t pc, uint64_t sp, uint64_t fp,
                      uint64_t guard, uint64_t found, int have_found) {
  int n = 0;
  if (!out || cap < 16)
    return 0;
  n = puts_buf(out, cap, n, "KERNEL PANIC: stack smashing detected (SEC-4 canary)\n");
  n = puts_buf(out, cap, n, "  smasher pc=");
  n = puthex_buf(out, cap, n, pc);
  n = puts_buf(out, cap, n, " (resolve: llvm-addr2line -e hobbyos.elf <pc>)\n");
  n = puts_buf(out, cap, n, "  rsp=");
  n = puthex_buf(out, cap, n, sp);
  n = puts_buf(out, cap, n, " rbp=");
  n = puthex_buf(out, cap, n, fp);
  n = puts_buf(out, cap, n, " guard=");
  n = puthex_buf(out, cap, n, guard);
  n = putc_buf(out, cap, n, '\n');
  n = puts_buf(out, cap, n, "  found=");
  if (have_found) {
    n = puthex_buf(out, cap, n, found);
    n = puts_buf(out, cap, n, " delta=");
    n = puthex_buf(out, cap, n, found - guard);
  } else {
    n = puts_buf(out, cap, n, "UNRESOLVED delta=UNRESOLVED");
  }
  n = putc_buf(out, cap, n, '\n');
  if (n >= cap)
    n = cap - 1;
  out[n] = '\0';
  return n;
}
