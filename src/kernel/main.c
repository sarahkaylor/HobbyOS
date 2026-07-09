#include "fat16.h"
#include "fs.h"
#include "gic.h"
#include "mmu.h"
#include "process.h"
#include "program_loader.h"
#include "timer.h"
#include "trap.h"
#include "virtio_blk.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include <stdint.h>
#include "net.h"
#include "virtio_net.h"
#include "dhcp.h"
#include "arch/cpu.h"
#include "lock.h"

void virtio_blk_handle_irq(void);
extern int virtio_blk_irq;
extern int virtio_net_irq;
extern void smp_init(void);
extern void mmu_init_core(void);
extern void gic_init_cpu(void);

extern void uart_init(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);

static spinlock_t print_lock;

/**
 * High-level handler for hardware interrupts (IRQs) occurring in the kernel
 * (EL1). Specifically handles VirtIO block interrupts and timer ticks.
 */
void irq_handler_c(struct trap_frame *tf) {
  uint32_t intid = gic_acknowledge_interrupt();

  // VirtIO Block IRQ for located slot on virt machine
  if (intid == (uint32_t)virtio_blk_irq) {
    virtio_blk_handle_irq();
  } else if (intid == (uint32_t)virtio_net_irq) {
    virtio_net_handle_irq();
  } else if (intid >= 48 && intid <= 79) {
    extern void virtio_input_handle_irq(int irq);
    virtio_input_handle_irq(intid);
  } else if (intid == 30) {
    // Timer PPI
    timer_reload();
  }

  gic_end_interrupt(intid);
}

/**
 * Prints a signed integer to the UART in decimal format.
 */
void print_int(int val) {
  uint64_t flags = spinlock_acquire_irqsave(&print_lock);
  if (val < 0) {
    uart_putc('-');
    val = -val;
  }
  if (val == 0) {
    uart_putc('0');
    spinlock_release_irqrestore(&print_lock, flags);
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
  spinlock_release_irqrestore(&print_lock, flags);
}

/**
 * Prints a 64-bit value to the UART in hexadecimal format (e.g., 0xABC123).
 */
void uart_print_hex(uint64_t val) {
  uint64_t flags = spinlock_acquire_irqsave(&print_lock);
  char hex_chars[] = "0123456789ABCDEF";
  uart_putc('0');
  uart_putc('x');
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc(hex_chars[(val >> i) & 0xF]);
  }
  spinlock_release_irqrestore(&print_lock, flags);
}

/**
 * Primary kernel entry point for CPU 0.
 * Initializes all hardware subsystems, filesystems, and the scheduler.
 */
void main(void) {
  uart_init();
  spinlock_init(&print_lock);
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

  if (virtio_input_init() == 0) {
    uart_puts("VirtIO Input devices successfully initialized.\n");
  } else {
    uart_puts("No VirtIO Input devices found.\n");
  }

  // Initialize multitasking and secondary cores early
  process_init();
  fs_init();
  extern void pipes_init(void);
  pipes_init();

  // Initialize and enable the timer
  timer_init();

  // Wake up secondary cores (PSCI/IPI)
#if !defined(KERNEL_MODE_UNIT_TEST) || defined(__x86_64__)
  smp_init();
#endif

  // Enable interrupts on the boot core
  interrupts_enable();

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

  net_init();
  if (virtio_net_init() == 0) {
    uart_puts("VirtIO Network successfully initialized.\n");
    net_refresh_mac();
    gic_enable_interrupt(virtio_net_irq);
#ifndef KERNEL_MODE_UNIT_TEST
    dhcp_init();
#endif
  } else {
    uart_puts("VirtIO Network initialization failed!\n");
  }

#ifdef __x86_64__
  // Start the Remote PCIe sharing subsystem (RDMA over UDP).
  // Reads opt/pcishare from QEMU fw_cfg; if not present returns immediately.
  // If configured as host, spawns the provider loop as a kernel process so
  // the desktop (or other modes) continue loading without blocking.
  {
    extern void net_rdma_init(void);
    net_rdma_init();
  }
#endif

  // -----------------------------------------------------------------------
  // Parallel Boot: Load programs into the scheduler based on the mode.
  // Secondary cores are already spinning in start_scheduler() and will
  // pick these up as soon as they are marked READY.
  // -----------------------------------------------------------------------
  uart_puts("\n--- Parallel Program Loading ---\n");

#ifdef KERNEL_MODE_UNIT_TEST
  uart_puts("Mode: UNIT_TEST - Running Kernel Unit Tests...\n");
  extern void run_all_unit_tests(void);
  run_all_unit_tests();
#elif defined(KERNEL_MODE_TEST)
  uart_puts("Mode: TEST - Running automated tests...\n");
  load_and_run_program_in_scheduler("SHTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("SHTEST2.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("SHTEST3.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("CONSOLE.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("MEMTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("FILEIO.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("HEAPTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("SPAWN.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("FORKTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("SMPTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("PIPETEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("GRAPHICS.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("NETTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("TIMEOUT.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("STRESS.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_DESKTOP_TEST)
  uart_puts("Mode: DESKTOP_TEST - Launching desktop in test mode...\n");
  load_and_run_program_in_scheduler("EDITOR_T.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_PONG_TEST)
  uart_puts("Mode: PONG_TEST - Launching Pong test wrapper...\n");
  load_and_run_program_in_scheduler("PONG_T.BIN", -1, -1, -1, -1);
#else
  uart_puts("Mode: DESKTOP - Launching desktop...\n");
  load_and_run_program_in_scheduler("DESKTOP.BIN", -1, -1, -1, -1);
#endif

  // Join the other cores in the scheduler
  extern volatile int scheduler_started;
  scheduler_started = 1;
  start_scheduler();

  uart_puts("System halt.\n");
  extern void halt(void);
  halt();
}

/**
 * Entry point for secondary CPU cores.
 * Sets up core-local MMU, GIC, and timer, then enters the scheduler.
 */
#ifdef __x86_64__
void secondary_main(uint32_t cpu) {
  // Acknowledge to boot core that we have started and read our parameters
  extern volatile int smp_core_ready;
  smp_core_ready = 1;

  // 1. Initialize local MMU
  extern void mmu_init_core_with_id(uint32_t cpu);
  mmu_init_core_with_id(cpu);
#else
void secondary_main(void) {
  // 1. Initialize local MMU
  mmu_init_core();
#endif

  // 2. Initialize local GIC CPU interface
  gic_init_cpu();

  // 3. Enable local timer
  timer_init();

  // 4. Enable interrupts
  interrupts_enable();

  // 5. Enter scheduler
  start_scheduler();
}
