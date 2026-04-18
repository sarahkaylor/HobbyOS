#include <stdint.h>

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
volatile uint32_t* const UART0_DR = (uint32_t*)UART0_BASE;

void uart_putc(char c) {
    // By default on QEMU's virt machine, the UART is always ready to receive.
    // In real hardware, we would poll the UARTFR (Flag Register) to check the TXFF (Transmit FIFO Full) bit.
    if (c == '\n') {
        *UART0_DR = (uint32_t)('\r');
    }
    *UART0_DR = (uint32_t)(c);
}

void uart_puts(const char* s) {
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}

void main(void) {
    uart_puts("Hello world\n");
}
