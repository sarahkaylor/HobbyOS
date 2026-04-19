#include "fat16.h"
#include "gic.h"
#include "mmu.h"
#include "program_loader.h"
#include "trap.h"
#include "virtio_blk.h"
#include <stdint.h>
void virtio_blk_handle_irq(void);
extern int virtio_blk_irq;

void irq_handler_c(void) {
  uint32_t intid = gic_acknowledge_interrupt();

  // VirtIO Block IRQ for located slot on virt machine
  if (intid == (uint32_t)virtio_blk_irq) {
    virtio_blk_handle_irq();
  }

  gic_end_interrupt(intid);
}

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
volatile uint32_t *const UART0_DR = (uint32_t *)UART0_BASE;

void uart_putc(char c) {
  if (c == '\n') {
    *UART0_DR = (uint32_t)('\r');
  }
  *UART0_DR = (uint32_t)(c);
}

void uart_puts(const char *s) {
  while (*s != '\0') {
    uart_putc(*s);
    s++;
  }
}

void print_int(int val) {
  if (val < 0) {
    uart_putc('-');
    val = -val;
  }
  if (val == 0) {
    uart_putc('0');
    return;
  }
  char buf[16];
  int idx = 0;
  while (val > 0) {
    buf[idx++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (idx > 0)
    uart_putc(buf[--idx]);
}

void uart_print_hex(uint64_t val) {
  char hex_chars[] = "0123456789ABCDEF";
  uart_puts("0x");
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc(hex_chars[(val >> i) & 0xF]);
  }
}

void main(void) {
  uart_puts("Booting AArch64 OS...\n");

  // Virtual Memory Protection
  mmu_init();
  uart_puts("MMU Initialized: Page Tables setup securely.\n");

  gic_init();

  // Clears the standard I/F interrupt masking flags from the hardware PSTATE
  // enabling the CPU interface to natively accept GIC triggers dynamically.
  __asm__ volatile("msr daifclr, #2"); // Enable IRQ in PSTATE

  if (virtio_blk_init() != 0) {
    uart_puts("VirtIO Block initialization failed!\n");
    return;
  }

  // Using the dynamically harvested IRQ slot populated during `virtio_blk_init`
  // scanning, we instruct the GIC Distributor to unmask and forward the device
  // INTID specifically to this runtime.
  gic_enable_interrupt(virtio_blk_irq);
  uart_puts("VirtIO Block successfully initialized.\n");

  if (fat16_init() != 0) {
    uart_puts("FAT-16 initialization failed!\n");
    return;
  }
  uart_puts("FAT-16 filesystem successfully initialized.\n");

  // Load and execute the console tests program
  if (load_and_run_program("CONSOLE.BIN") != 0) {
    uart_puts("Failed to load and execute CONSOLE.BIN!\n");
  } else {
    uart_puts("CONSOLE.BIN executed successfully.\n");
  }

  // Load and execute the memory protection test program
  if (load_and_run_program("MEMTEST.BIN") != 0) {
    uart_puts("Failed to load and execute MEMTEST.BIN!\n");
  } else {
    uart_puts("MEMTEST.BIN executed successfully.\n");
  }

  // Load and execute the file i/o test program
  if (load_and_run_program("FILEIO.BIN") != 0) {
    uart_puts("Failed to load and execute FILEIO.BIN!\n");
  } else {
    uart_puts("FILEIO.BIN executed successfully.\n");
  }

  uart_puts("System halt.\n");
}
