/*
 * linux_compat_cons.c — C0-console's kernel-side console stress (printing only).
 *
 * One private Linux-ABI number, L_SYS_hb_cons_stress (0x4843, far above every
 * real x86_64 syscall), used only by src/user/linux_probe_console.c.  Each call
 * prints `rows` kernel rows from the CPU it runs on, in the shapes the kernel
 * really uses, so the probe can count what the console does to them:
 *
 *   k=0  a row built from several uart_puts/print_int calls (the [BLKD] cc=,
 *        [FRAME] shape)
 *   k=1  a row written one character at a time through uart_putc_dbg (the
 *        stall watchdog's wd_puts shape, lock-free before C0-console)
 *   k=2  one kline, one uart_puts (the [PUB] shape)
 *   k=3  a row written one character at a time through uart_putc
 *   k=4  k=0 with interrupts masked for the middle pieces (a row printed
 *        across an irqsave lock)
 *
 * Row:  [CSTR] c=<cpu> k=<kind> s=<seq> p=<payload> e=<len>
 * The payload is a pure function of (cpu, seq) (tools/gate/lp_check_console.py
 * regenerates it), so a torn, merged or truncated row is detectable.
 * No state, no locks, no side effect except console output.
 */
#ifdef __x86_64__
#include <stdint.h>

#include "kline.h"
#include "linux_compat.h"
#include "process.h"

extern uint32_t get_cpuid(void);
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void uart_putc_dbg(char c);
extern void print_int(int v);

#define L_SYS_hb_cons_stress 0x4843

static const char cstr_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static int cstr_len(uint32_t cpu, uint32_t seq) {
  return 40 + (int)((seq * 7u + cpu * 13u) % 160u);
}

static char cstr_ch(uint32_t cpu, uint32_t seq, int i) {
  return cstr_alpha[(seq + (uint32_t)i * 7u + cpu) % 62u];
}

static void cstr_fmt(struct kline *k, uint32_t cpu, int kind, uint32_t seq) {
  int len = cstr_len(cpu, seq);
  kl_s(k, "[CSTR] c=");
  kl_i(k, (long)cpu);
  kl_s(k, " k=");
  kl_i(k, kind);
  kl_s(k, " s=");
  kl_i(k, (long)seq);
  kl_s(k, " p=");
  for (int i = 0; i < len; i++)
    kl_c(k, cstr_ch(cpu, seq, i));
  kl_s(k, " e=");
  kl_i(k, len);
  kl_c(k, '\n');
}

static void cstr_pieces(uint32_t cpu, int kind, uint32_t seq, int mask_mid) {
  int len = cstr_len(cpu, seq);
  uart_puts("[CSTR] c=");
  print_int((int)cpu);
  uart_puts(" k=");
  print_int(kind);
  uart_puts(" s=");
  print_int((int)seq);
  uint64_t fl = 0;
  if (mask_mid)
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
  uart_puts(" p=");
  char piece[9];
  for (int i = 0; i < len; i += 8) {
    int m = 0;
    for (; m < 8 && i + m < len; m++)
      piece[m] = cstr_ch(cpu, seq, i + m);
    piece[m] = 0;
    uart_puts(piece);
  }
  if (mask_mid && (fl & 0x200))
    __asm__ volatile("sti" : : : "memory");
  uart_puts(" e=");
  print_int(len);
  uart_puts("\n");
}

static int l_cons_stress(struct process *p, struct trap_frame *tf) {
  (void)p;
  int64_t rows = (int64_t)l_arg(tf, 0);
  uint32_t seq = (uint32_t)l_arg(tf, 1);
  uint32_t kinds = (uint32_t)l_arg(tf, 2);
  if (rows < 0 || rows > 64 || (kinds & 0x1F) == 0) {
    tf->regs[0] = (uint64_t)(int64_t)-22; /* EINVAL */
    return 1;
  }
  uint32_t cpu = get_cpuid();
  int done = 0;
  for (int r = 0; r < rows; r++, seq++) {
    int kind = (int)(seq % 5u);
    while (!(kinds & (1u << kind)))
      kind = (kind + 1) % 5;
    if (kind == 0 || kind == 4) {
      cstr_pieces(cpu, kind, seq, kind == 4);
    } else {
      struct kline k;
      kl_init(&k);
      cstr_fmt(&k, cpu, kind, seq);
      if (kind == 2) {
        kl_emit(&k);
      } else {
        for (int i = 0; i < k.n; i++) {
          if (kind == 1)
            uart_putc_dbg(k.b[i]);
          else
            uart_putc(k.b[i]);
        }
      }
    }
    done++;
  }
  /* rows printed, and the CPU in bits 32.. so the probe can count CPUs */
  tf->regs[0] = (uint64_t)done | ((uint64_t)cpu << 32);
  return 1;
}

const struct l_sysent l_tab_cons[] = {
  {L_SYS_hb_cons_stress, l_cons_stress, "hb_cons_stress"},
  L_SYSENT_END
};

#endif /* __x86_64__ */
