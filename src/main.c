#include <stdint.h>
#include "virtio_blk.h"
#include "fat16.h"

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
volatile uint32_t* const UART0_DR = (uint32_t*)UART0_BASE;

void uart_putc(char c) {
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

void print_int(int val) {
    if (val < 0) { uart_putc('-'); val = -val; }
    if (val == 0) { uart_putc('0'); return; }
    char buf[16];
    int idx = 0;
    while(val > 0) { buf[idx++] = (char)('0' + (val % 10)); val /= 10; }
    while(idx > 0) uart_putc(buf[--idx]);
}

void main(void) {
    uart_puts("Booting AArch64 OS...\n");

    if (virtio_blk_init() != 0) {
        uart_puts("VirtIO Block initialization failed!\n");
        return;
    }
    uart_puts("VirtIO Block successfully initialized.\n");

    if (fat16_init() != 0) {
        uart_puts("FAT-16 initialization failed!\n");
        return;
    }
    uart_puts("FAT-16 filesystem successfully initialized.\n");

    int fd = file_open("TEST.TXT");
    if (fd >= 0) {
        uart_puts("Opened TEST.TXT successfully.\n");
        const char* msg = "Hello from FAT16 disk!\n";
        
        // Write test
        int w = file_write(fd, msg, 23);
        uart_puts("Wrote "); print_int(w); uart_puts(" bytes to disk.\n");
        
        // Seek & Read Test
        file_seek(fd, 0);
        char readbuf[32];
        for (int i=0; i<32; i++) readbuf[i] = 0;
        
        int r = file_read(fd, readbuf, 23);
        uart_puts("Read "); print_int(r); uart_puts(" bytes from disk: ");
        uart_puts(readbuf);
        
        file_close(fd);
    } else {
        uart_puts("Failed to open or create TEST.TXT!\n");
    }

    uart_puts("System halt.\n");
}
