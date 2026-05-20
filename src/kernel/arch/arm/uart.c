#include "lock.h"
#include <stdint.h>

static spinlock_t uart_lock;

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

/**
 * Outputs a single character to the PL011 UART.
 * Handles newline translation (\n -> \r\n).
 */
void uart_putc(char c) {
  if (c == '\n') {
    while (*UART0_FR & (1 << 5)) {
    } // Wait until TXFF is clear
    *UART0_DR = (uint32_t)('\r');
  }
  while (*UART0_FR & (1 << 5)) {
  } // Wait until TXFF is clear
  *UART0_DR = (uint32_t)(c);
}

/**
 * Outputs a null-terminated string to the UART.
 * Uses a spinlock to ensure atomic output from multiple CPUs.
 */
void uart_puts(const char *s) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  while (*s != '\0') {
    uart_putc(*s);
    s++;
  }
  spinlock_release_irqrestore(&uart_lock, flags);
}
