#include "lock.h"
#include <stdint.h>

/** The single console lock, shared with the arch-independent print
    helpers in main.c (they serialize against uart_puts/uart_putc so the
    whole UART has exactly one lock — previously uart_puts used this lock
    while print_int used a second one, which let bytes interleave). */
spinlock_t uart_lock;

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

void com2_init(void);

/**
 * Initializes the PC COM1 serial port.
 */
void uart_init(void) {
  spinlock_init(&uart_lock);

  outb(COM1_PORT + 1, 0x00);    // Disable all interrupts
  outb(COM1_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
  outb(COM1_PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
  outb(COM1_PORT + 1, 0x00);    //                  (hi byte)
  outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
  outb(COM1_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
  outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
  com2_init();

  /* Print the lock's address once (raw MMIO — uart_puts itself needs the
     lock and isn't available this early): [LOCKFOREVER] screams report raw
     lock addresses, so matching them is trivial when debugging a pileup. */
  {
    static const char hx[] = "0123456789abcdef";
    static const char pre[] = "[UART] lock @ 0x";
    uint64_t v = (uint64_t)&uart_lock;
    for (const char *p = pre; *p; p++) outb(COM1_PORT, (uint8_t)*p);
    for (int i = 15; i >= 0; i--) outb(COM1_PORT, (uint8_t)hx[(v >> (i * 4)) & 0xF]);
    outb(COM1_PORT, '\r');
    outb(COM1_PORT, '\n');
  }
}

/**
 * Helper to check if the transmit buffer is empty.
 */
static int is_transmit_empty(void) {
  return inb(COM1_PORT + 5) & 0x20;
}

extern uint64_t timer_get_ms(void);

/* Milliseconds at the last console write.  The stall watchdog in trap.c
   reads this to detect silent hangs during the test wave. */
volatile uint64_t uart_last_activity_ms = 0;

/**
 * Lock-free UART write body.  This is the ONLY function that touches the
 * COM1 registers.  It deliberately takes no lock: deadlock diagnostics
 * (spinlock screams, stall watchdog) run while the console lock itself may
 * be wedged and MUST still reach the wire.
 *
 * uart_putc / uart_puts serialize with uart_lock around this body, which
 * gives the whole console ONE lock.  (Previously uart_puts took uart_lock
 * while print_int/uart_print_hex took a separate print_lock, so two cores
 * could interleave bytes inside one logical write — the byte-soup
 * "[CONSOLE] " / "I0nsi0de" interleaves seen under SMP.)
 */
static void uart_putc_body(char c) {
  uart_last_activity_ms = timer_get_ms();
  if (c == '\n') {
    while (!is_transmit_empty()) {
      // Spin
    }
    outb(COM1_PORT, '\r');
  }
  while (!is_transmit_empty()) {
    // Spin
  }
  outb(COM1_PORT, c);
}

/** Raw single character — lock-free, for deadlock diagnostics only. */
void uart_putc_raw(char c) {
  uart_putc_body(c);
}

/* ---- COM2: a second, uncontaminated diagnostic stream (x64 only).
   The console (COM1) interleaves every core's writes even though each
   logical write is atomic, so a 20s-stall watchdog dump gets shredded by
   the loader's console noise.  QEMU maps COM2 at 0x2F8; `-serial file:`
   captures it to its own log, byte-exact.  All writers here are lock-free
   by construction (nothing else uses COM2). */

static void com2_outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static uint8_t com2_inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}
static int com2_tx_empty(void) { return com2_inb(COM2_PORT + 5) & 0x20; }

void com2_init(void) {
  com2_outb(COM2_PORT + 1, 0x00);
  com2_outb(COM2_PORT + 3, 0x80);
  com2_outb(COM2_PORT + 0, 0x03);
  com2_outb(COM2_PORT + 1, 0x00);
  com2_outb(COM2_PORT + 3, 0x03);
  com2_outb(COM2_PORT + 2, 0xC7);
  com2_outb(COM2_PORT + 4, 0x0B);
}

void uart_putc_raw2(char c) {
  if (c == '\n') {
    while (!com2_tx_empty()) {}
    com2_outb(COM2_PORT, '\r');
  }
  while (!com2_tx_empty()) {}
  com2_outb(COM2_PORT, c);
}

void uart_puts_raw2(const char *s) {
  while (*s != '\0') {
    uart_putc_raw2(*s);
    s++;
  }
}

void uart_print_hex_raw2(uint64_t val) {
  static const char hex_chars[] = "0123456789ABCDEF";
  uart_putc_raw2('0');
  uart_putc_raw2('x');
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc_raw2(hex_chars[(val >> i) & 0xF]);
  }
}

void print_int_raw2(int val) {
  if (val < 0) {
    uart_putc_raw2('-');
    val = -val;
  }
  if (val == 0) {
    uart_putc_raw2('0');
    return;
  }
  char buf[16];
  int idx = 0;
  while (val > 0) {
    buf[idx++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (idx > 0)
    uart_putc_raw2(buf[--idx]);
}

/** Raw string — lock-free, for deadlock diagnostics only. */
void uart_puts_raw(const char *s) {
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
}

/**
 * Outputs a single character to the COM1 serial port, serialized against
 * all other console output.
 */
void uart_putc(char c) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  uart_putc_body(c);
  spinlock_release_irqrestore(&uart_lock, flags);
}

/**
 * Outputs a null-terminated string to the serial port.
 * Uses a spinlock to ensure atomic serial printing from multiple cores.
 */
void uart_puts(const char *s) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  while (*s != '\0') {
    uart_putc_body(*s);
    s++;
  }
  spinlock_release_irqrestore(&uart_lock, flags);
}
