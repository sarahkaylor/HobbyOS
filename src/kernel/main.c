#include "fat16.h"
#include "gic.h"
#include "mmu.h"
#include "process.h"
#include "program_loader.h"
#include "timer.h"
#include "trap.h"
#include "virtio_blk.h"
#include "virtio_gpu.h"
#include "fs.h"
#include <stdint.h>
void virtio_blk_handle_irq(void);
extern int virtio_blk_irq;
extern void smp_init(void);
extern void mmu_init_core(void);
extern void gic_init_cpu(void);
extern uint32_t get_cpuid(void);
#include "lock.h"

static spinlock_t uart_lock;

void irq_handler_c(struct trap_frame *tf) {
  uint32_t intid = gic_acknowledge_interrupt();

  // VirtIO Block IRQ for located slot on virt machine
  if (intid == (uint32_t)virtio_blk_irq) {
    virtio_blk_handle_irq();
  } else if (intid == 30) {
    // Timer PPI
    timer_reload();
    struct process *cur = current_process();
    if (cur) {
        schedule(tf);
    }
  }

  gic_end_interrupt(intid);
}

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
volatile uint32_t *const UART0_DR = (uint32_t *)UART0_BASE;

// Pointer to the flag register of the UART
volatile uint32_t *const UART0_FR = (uint32_t *)(UART0_BASE + 0x18);

void uart_putc(char c) {
  if (c == '\n') {
    while (*UART0_FR & (1 << 5)) { } // Wait until TXFF is clear
    *UART0_DR = (uint32_t)('\r');
  }
  while (*UART0_FR & (1 << 5)) { } // Wait until TXFF is clear
  *UART0_DR = (uint32_t)(c);
}

void uart_puts(const char *s) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  while (*s != '\0') {
    uart_putc(*s);
    s++;
  }
  spinlock_release_irqrestore(&uart_lock, flags);
}

void print_int(int val) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  if (val < 0) {
    uart_putc('-');
    val = -val;
  }
  if (val == 0) {
    uart_putc('0');
    spinlock_release_irqrestore(&uart_lock, flags);
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
  spinlock_release_irqrestore(&uart_lock, flags);
}

void uart_print_hex(uint64_t val) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  char hex_chars[] = "0123456789ABCDEF";
  uart_putc('0');
  uart_putc('x');
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc(hex_chars[(val >> i) & 0xF]);
  }
  spinlock_release_irqrestore(&uart_lock, flags);
}

void main(void) {
  spinlock_init(&uart_lock);
  uart_puts("Booting AArch64 OS...\n");

  // Virtual Memory Protection
  mmu_init();
  uart_puts("MMU Initialized: Page Tables setup securely.\n");

  if (virtio_gpu_init() == 0) {
    uart_puts("VirtIO GPU successfully initialized.\n");
  } else {
    uart_puts("VirtIO GPU initialization failed!\n");
  }

  gic_init();

  // Initialize multitasking and secondary cores early
  process_init();
  fs_init();
  extern void pipes_init(void);
  pipes_init();

  // Enable the timer PPI (INTID 30) for preemptive scheduling
  gic_enable_interrupt(30);
  timer_init();

  // Wake up secondary cores via PSCI
  smp_init();

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

  // -----------------------------------------------------------------------
  // Parallel Boot: Load all programs into the scheduler.
  // Secondary cores are already spinning in start_scheduler() and will
  // pick these up as soon as they are marked READY.
  // -----------------------------------------------------------------------
  uart_puts("\n--- Parallel Program Loading ---\n");

  // load_and_run_program_in_scheduler("CONSOLE.BIN");
  // load_and_run_program_in_scheduler("MEMTEST.BIN");
  // load_and_run_program_in_scheduler("FILEIO.BIN");
  // load_and_run_program_in_scheduler("HEAPTEST.BIN");
  // load_and_run_program_in_scheduler("SPAWN.BIN");
  // load_and_run_program_in_scheduler("FORKTEST.BIN");
  // load_and_run_program_in_scheduler("GRAPHICS.BIN");
  // load_and_run_program_in_scheduler("SMPTEST.BIN");
  load_and_run_program_in_scheduler("PIPETEST.BIN");

  // Join the other cores in the scheduler
  start_scheduler();

  uart_puts("System halt.\n");
}

void secondary_main(void) {
  uint32_t cpu = get_cpuid();

  // 1. Initialize local MMU
  mmu_init_core();

  // 2. Initialize local GIC CPU interface
  gic_init_cpu();

  // 3. Enable local timer
  timer_init();
  gic_enable_interrupt(30);

  // 4. Enable IRQ
  __asm__ volatile("msr daifclr, #2");

  // 5. Enter scheduler
  start_scheduler();
}
