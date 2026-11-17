#ifndef CONSOLE_H
#define CONSOLE_H

/*
 * console.h — the line-atomic serial console (C0-console, docs/cp-console.md).
 *
 * Every kernel writer (uart_puts, uart_putc, print_int, uart_print_hex,
 * uart_putc_dbg, the fatal-path raw sinks and the helpers built on them) ends in
 * cons_kwrite().  Bytes are collected per CPU and per context into a line
 * slot and reach the wire only as whole lines, each written under ONE console
 * lock hold.  The lock is a private word with a bounded wait (never a
 * spinlock_t, so the lock screams can print through it), broken at once when
 * its holder is a halted CPU; past the bound a writer prints anyway, tagged.
 *
 * User console writes (console_write_user) go through cons_user_write(): a
 * whole write() up to the caller's window is written under one lock hold, and
 * a trailing partial line is held per thread until its newline arrives (or
 * CONS_USER_STALE_US passes), so a line written in pieces is still one line.
 *
 * The diagnostic filter (cons_diag) drops the high-volume rows ([PUB], [MR],
 * [FUTEX], [ALIVE-SET] slot rows, [RESUME-*], [BLKD] cc=, [PECOW], [ANONF])
 * unless they are switched on with the build flag CP_DIAG=... or the QEMU
 * fw_cfg file opt/hobbyos/diag.  Fault and crash dumps are never filtered.
 */

#include <stdint.h>

/* ---- kernel side -------------------------------------------------------- */
void cons_kwrite(const char *s, int n); /* n < 0: NUL-terminated */
void cons_kputc(char c);
/* Push this CPU's partial lines out now (fatal paths, before a halt). */
void cons_flush_local(void);
/* Timer interrupt (IRQs off): flush stale partial lines; CPU 0 also flushes
   stale user partial lines and prints the periodic [CONS] summary. */
void cons_tick(uint32_t cpu);
/* Read the boot-time knob (fw_cfg); called once from uart_init. */
void cons_init(void);

/* ---- user side ---------------------------------------------------------- */
/* `who` identifies the writing thread (pid + 1); `s` is a KERNEL buffer. */
void cons_user_write(uint64_t who, const char *s, int n);

/* ---- diagnostic classes (cons_diag_mask bits) --------------------------- */
#define CONS_D_PUB    (1u << 0)  /* [PUB] page publications             */
#define CONS_D_MR     (1u << 1)  /* [MR] s= region rows                 */
#define CONS_D_FUTEX  (1u << 2)  /* [FUTEX] trace and map dumps         */
#define CONS_D_ALIVE  (1u << 3)  /* [ALIVE-SET] slot= dump rows         */
#define CONS_D_RESUME (1u << 4)  /* [RESUME-RZ/-SPSR/-DIAG]             */
#define CONS_D_BLKD   (1u << 5)  /* [BLKD] cc= completion rows          */
#define CONS_D_PECOW  (1u << 6)  /* [PECOW]                             */
#define CONS_D_ANONF  (1u << 7)  /* [ANONF]                             */
#define CONS_D_NCLASS 8
#define CONS_D_ALL    0xFFFFFFFFu
extern volatile uint32_t cons_diag_mask; /* bit set = class printed */
/* Parse "all", "none"/"0", or a comma list of class names ("pub,mr"). */
uint32_t cons_diag_parse(const char *s, int n);

/* ---- counters (read by the [CONS] summary row and the probes) ----------- */
struct cons_stats {
  uint64_t lines;       /* kernel lines written whole                   */
  uint64_t user_writes; /* user write() windows                         */
  uint64_t forced;      /* written past the lock bound (tagged [CONS!]) */
  uint64_t broken;      /* lock taken over from a halted CPU            */
  uint64_t reentered;   /* written raw: console re-entered on this CPU  */
  uint64_t stale;       /* partial line pushed out by age               */
  uint64_t evicted;     /* partial line pushed out for a slot           */
  uint64_t split;       /* line longer than a slot, written in pieces   */
  uint64_t dropped[CONS_D_NCLASS]; /* rows held back by the filter      */
};
extern struct cons_stats cons_stats;

/* ---- arch hooks (src/kernel/arch/<arch>/uart.c) -------------------------- */
void cons_hw_tx(const char *s, int n);   /* raw bytes, '\n' -> "\r\n"   */
uint64_t cons_hw_irqsave(void);          /* mask IRQs, return old state */
void cons_hw_irqrestore(uint64_t flags);
int cons_hw_irqs_on(uint64_t flags);     /* were IRQs enabled in flags  */
const void *cons_hw_ctx(uint32_t cpu);   /* the running thread, or 0    */
int cons_hw_in_panic(void);              /* a fatal record is printing  */
int cons_hw_bootarg(char *buf, int cap); /* fw_cfg opt/hobbyos/diag     */
void cons_hw_relax(void);

#endif /* CONSOLE_H */
