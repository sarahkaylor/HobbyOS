#include "uart.h"
#include <stdint.h>
void test_alias() {
    volatile uint64_t *a = (volatile uint64_t *)0x4093CD98;
    volatile uint64_t *b = (volatile uint64_t *)0x5033CD98;
    *a = 0x11223344;
    *b = 0x55667788;
    if (*a == 0x55667788) {
        uart_puts("ALIASED!\n");
    } else {
        uart_puts("NOT ALIASED!\n");
    }
}
