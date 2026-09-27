#include "lock.h"
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
}

/** Lock-free PL011 write body — the only code that touches the UART
    registers.  Deadlock diagnostics use uart_putc_raw/uart_puts_raw and
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

/** Raw single character — lock-free, for deadlock diagnostics only. */
void uart_putc_raw(char c) {
  uart_putc_body(c);
}

/** Raw string — lock-free, for deadlock diagnostics only. */
void uart_puts_raw(const char *s) {
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
}

/** Single character, serialized against all other console output. */
void uart_putc(char c) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  uart_putc_body(c);
  spinlock_release_irqrestore(&uart_lock, flags);
}

/**
 * Outputs a null-terminated string to the UART.
 * Uses a spinlock to ensure atomic output from multiple CPUs.
 */
void uart_puts(const char *s) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
  spinlock_release_irqrestore(&uart_lock, flags);
}
