#include "lock.h"
#include <stdint.h>

static spinlock_t uart_lock;

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

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
}

/**
 * Helper to check if the transmit buffer is empty.
 */
static int is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

/**
 * Outputs a single character to the COM1 serial port.
 * Translates newline to carriage return + newline.
 */
void uart_putc(char c) {
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

/**
 * Outputs a null-terminated string to the serial port.
 * Uses a spinlock to ensure atomic serial printing from multiple cores.
 */
void uart_puts(const char *s) {
    uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
    spinlock_release_irqrestore(&uart_lock, flags);
}
