#include "lock.h"
#include "console.h"
#include "process.h"
#include <stdint.h>

/** The single console lock, shared with the arch-independent print
    helpers in main.c (they serialize against uart_puts/uart_putc so the
    whole UART has exactly one lock). */
spinlock_t uart_lock;

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
static volatile uint32_t *const UART0_DR = (uint32_t *)UART0_BASE;

// Pointer to the flag register of the UART
static volatile uint32_t *const UART0_FR = (uint32_t *)(UART0_BASE + 0x18);

/**
 * Initializes the PL011 UART spinlock.
 */
void uart_init(void) {
  spinlock_init(&uart_lock);
  cons_init();
}

/** Lock-free PL011 write body — the only code that touches the UART
    registers.  Deadlock diagnostics use panic_putc/panic_puts and
    must not take uart_lock, which may itself be wedged. */
static void uart_putc_body(char c) {
  if (c == '\n') {
    while (*UART0_FR & (1 << 5)) {
    } // Wait until TXFF is clear
    *UART0_DR = (uint32_t)('\r');
  }
  while (*UART0_FR & (1 << 5)) {
  } // Wait until TXFF is clear
  *UART0_DR = (uint32_t)(c);
}

/* ---- C0-console arch hooks (src/kernel/kconsole.c) ---- */
void cons_hw_tx(const char *s, int n) {
  for (int i = 0; i < n; i++)
    uart_putc_body(s[i]);
}

uint64_t cons_hw_irqsave(void) {
  uint64_t f;
  __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(f) : : "memory");
  return f;
}

void cons_hw_irqrestore(uint64_t f) {
  __asm__ volatile("msr daif, %0" : : "r"(f) : "memory");
}

int cons_hw_irqs_on(uint64_t f) { return (f & 0x80) == 0; }

uint64_t cons_hw_ctx(uint32_t cpu) {
  return cpu < MAX_CPUS ? (uint64_t)(int64_t)(cpu_current_pids[cpu] + 1) : 0;
}

int cons_hw_in_panic(void) { return 0; }
int cons_hw_bootarg(char *buf, int cap) { (void)buf; (void)cap; return 0; }
void cons_hw_relax(void) { __asm__ volatile("yield"); }

/** Single character into this CPU's line slot (kconsole.c). */
void panic_putc(char c) {
  cons_kputc(c);
}

/** GB-097's NON-panic character sink (the x64 name); line-buffered since
    C0-console like every kernel writer. */
void uart_putc_dbg(char c) {
  cons_kputc(c);
}

/** String into this CPU's line slot (kconsole.c). */
void panic_puts(const char *s) {
  cons_kwrite(s, -1);
}

/* *_raw2 family: the deadlock-diagnostic console surface that
 * wd_dump_proc_table() (process.c) calls on BOTH architectures. Lock-free
 * and exception-safe (raw MMIO only), like the x64 panic_putc2 path. */
static void panic_putc2(char c) {
  cons_kputc(c); /* arm has one UART: the same line-atomic console */
}

void panic_puts2(const char *s) {
  while (*s != '\0') {
    panic_putc2(*s);
    s++;
  }
}

void panic_print_hex2(uint64_t val) {
  static const char hex_chars[] = "0123456789ABCDEF";
  panic_putc2('0');
  panic_putc2('x');
  for (int i = 60; i >= 0; i -= 4) {
    panic_putc2(hex_chars[(val >> i) & 0xF]);
  }
}

void print_int_raw2(int val) {
  if (val < 0) {
    panic_putc2('-');
    val = -val;
  }
  if (val == 0) {
    panic_putc2('0');
    return;
  }
  char buf[16];
  int idx = 0;
  while (val > 0) {
    buf[idx++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (idx > 0)
    panic_putc2(buf[--idx]);
}

/** Single character, serialized against all other console output. */
void uart_putc(char c) {
  cons_kputc(c);
}

/**
 * Outputs a null-terminated string to the UART.
 * Uses a spinlock to ensure atomic output from multiple CPUs.
 */
void uart_puts(const char *s) {
  cons_kwrite(s, -1);
}
