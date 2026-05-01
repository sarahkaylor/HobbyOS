#include <stdint.h>

extern void uart_puts(const char* s);
extern void print_hex(uint64_t val);

/**
 * Tests the QEMU fw_cfg (Firmware Configuration) interface.
 * Reads and prints the "QEMU" signature from the fw_cfg port.
 */
void test_fw_cfg() {
    volatile uint16_t* selector = (volatile uint16_t*)0x09020008;
    volatile uint8_t* data = (volatile uint8_t*)0x09020000;
    
    // Select 0x0000 (signature)
    *selector = 0x0000;
    
    uint8_t sig[5] = {0};
    sig[0] = *data;
    sig[1] = *data;
    sig[2] = *data;
    sig[3] = *data;
    
    uart_puts("FW_CFG Signature: ");
    uart_puts((const char*)sig);
    uart_puts("\n");
}
