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

/* The single console lock lives in the per-arch uart.c; the print helpers
   serialize against uart_puts/uart_putc with it so UART output has exactly
   one lock (previously uart_puts used uart_lock while print_int used a
   second, print_lock — bytes interleaved under real SMP). */
extern spinlock_t uart_lock;
extern void uart_putc_raw(char c);
void print_int_raw(int val);
void uart_print_hex_raw(uint64_t val);

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
 *
 * Takes the ONE console lock (uart_lock, shared with uart_puts/uart_putc)
 * for the whole write so multi-char output cannot interleave with other
 * console writers.  The per-char writes go through the lock-free
 * uart_putc_raw because we already hold the lock.
 */
void print_int(int val) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  print_int_raw(val);
  spinlock_release_irqrestore(&uart_lock, flags);
}

/** Lock-free decimal print — deadlock diagnostics only. */
void print_int_raw(int val) {
  if (val < 0) {
    uart_putc_raw('-');
    val = -val;
  }
  if (val == 0) {
    uart_putc_raw('0');
    return;
  }
  char buf[16];
  int idx = 0;
  while (val > 0) {
    buf[idx++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (idx > 0)
    uart_putc_raw(buf[--idx]);
}

/**
 * Prints a 64-bit value to the UART in hexadecimal format (e.g., 0xABC123).
 */
void uart_print_hex(uint64_t val) {
  uint64_t flags = spinlock_acquire_irqsave(&uart_lock);
  uart_print_hex_raw(val);
  spinlock_release_irqrestore(&uart_lock, flags);
}

/** Lock-free hex print — deadlock diagnostics only. */
void uart_print_hex_raw(uint64_t val) {
  static const char hex_chars[] = "0123456789ABCDEF";
  uart_putc_raw('0');
  uart_putc_raw('x');
  for (int i = 60; i >= 0; i -= 4) {
    uart_putc_raw(hex_chars[(val >> i) & 0xF]);
  }
}

/**
 * Primary kernel entry point for CPU 0.
 * Initializes all hardware subsystems, filesystems, and the scheduler.
 */
#ifdef KERNEL_MODE_TEST
/* Boot-wave test loader.  Runs as a kernel thread so the boot core can
 * start the scheduler immediately; see the KERNEL_MODE_TEST branch below.
 * Load order is preserved: entries that rely on earlier programs having
 * exited to free process-table slots (WCTEST/HEDTEST/SEDTEST and the
 * spawn-heavy acceptance tests) still come last.  Every load keeps
 * WAVE_LOAD_RESERVE physical blocks free for child spawns; when the pool
 * is tighter than that, the load waits (this thread is preemptible, so
 * waiting cannot wedge the suite the way the old inline loop did). */
static void test_wave_loader(void *arg) {
  (void)arg;
  /* Diagnostic probe for the subdirectory create/spawn/cat flow that
     shell_test3 exercises.  Runs first so its console output is intact. */
  load_and_run_program_in_scheduler("SUBPRB.BIN", -1, -1, -1, -1);
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
  load_and_run_program_in_scheduler("MONITORT.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("NFSTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("ERRTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("HELLO.BIN", -1, -1, -1, -1);
  /* Phase 3: lseek/stat/fstat exercise the new syscalls directly. */
  load_and_run_program_in_scheduler("LKSTEST.BIN", -1, -1, -1, -1);
  /* WCTEST/HEDTEST spawn WC.BIN/HEDGNU.BIN through the real spawn2/pipe
     path; both run at the END so earlier processes have exited and freed
     process-table slots (the pid masks are 64-bit, so MAX_PROCESSES
     must stay <= 64). */
  load_and_run_program_in_scheduler("HEDTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("TAILTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("PROCTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("WCTEST.BIN", -1, -1, -1, -1);
  /* REGTEST.BIN asserts the sysroot GNU regex in-OS against the same
     expectation table the host suite verifies against glibc.  It spawns
     nothing, so it can run alongside the pipe tests. */
  load_and_run_program_in_scheduler("REGTEST.BIN", -1, -1, -1, -1);
  /* NOTE: PIPEPRB.BIN / SEDPROBE.BIN remain staged on the image as manual
     diagnostics only - they must NOT run here, because their SEDT.IN
     fixture writes race SEDTEST.BIN and corrupt its golden cases. */

  /* SEDTEST.BIN demands byte-exact GNU sed 4.8 parity from SED.BIN across
     59 golden cases (tools/gen_sed_tests.py).  It spawns SED.BIN once per
     case, so it also runs at the END. */
  load_and_run_program_in_scheduler("SEDTEST.BIN", -1, -1, -1, -1);

  /* GREPTEST.BIN likewise demands byte-exact GNU grep 2.5.4 parity from
     GREP.BIN across its golden cases (tools/gen_grep_tests.py), spawning
     GREP.BIN once per case. */
  load_and_run_program_in_scheduler("GREPTEST.BIN", -1, -1, -1, -1);

  /* CUTTEST.BIN exercises the ported GNU cut (CUT.BIN) end-to-end:
     byte/field modes, delimiters, -s, --output-delimiter, stdin,
     multi-file and error exit codes. */
  load_and_run_program_in_scheduler("CUTTEST.BIN", -1, -1, -1, -1);

  /* Batch-1 textutils ports: tr, paste, fold, nl — each <TOOL>TEST.BIN
     spawns its <TOOL>.BIN through the spawn2/pipe path and checks
     byte-exact behavior in-OS. */
  load_and_run_program_in_scheduler("TRTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("PASTE_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("FOLDTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("NLTEST.BIN", -1, -1, -1, -1);

  /* Batch-2 textutils ports: comm, tsort, expand, unexpand, cksum,
     md5sum — same spawn2/pipe in-OS acceptance pattern. */
  load_and_run_program_in_scheduler("COMMTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("TSORT_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("EXPAND_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("UNEXPAND_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("CKSUM_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("MD5SUM_T.BIN", -1, -1, -1, -1);

  /* Batch 3: tac — last-line-first reversal acceptance (seeked path,
     /tmp temp-file path, -b/-s/-r) via the same spawn2/pipe pattern. */
  load_and_run_program_in_scheduler("TACTEST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("CMPTEST.BIN", -1, -1, -1, -1);

  /* Phase F1 (browser.md): socket/select/entropy acceptance.  POLLTST and
     RANDTST are deterministic and offline; SOCK2TST's network section is
     SKIP-able, its offline section is not. */
  load_and_run_program_in_scheduler("POLLTST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("RANDTST.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("SOCK2TST.BIN", -1, -1, -1, -1);

  /* F1.5 (FPU): floating-point bring-up — exact-value asserts across
     preemption, a two-process fork torture over pipes, and fork FP
     isolation (src/user/fpu_test.c).  Last in the list: its fork/pipe
     round trips need free process-table slots, which earlier tests have
     released by the time it runs. */
  load_and_run_program_in_scheduler("FPU_T.BIN", -1, -1, -1, -1);

  /* P3.2 (browser.md): sysroot transcendental acceptance — sin/cos/tan/
     asin/atan/atan2/log/pow/sqrt/modf and the float expf/tanhf/sqrtf,
     checked in ulps against glibc references and exact specials
     (src/user/math_test.c).  Links crt0 + libc.a, so it also proves the
     public symbols resolve from the archive.  Spawns nothing. */
  load_and_run_program_in_scheduler("MATH_T.BIN", -1, -1, -1, -1);

  /* F2.4 (browser.md): minimal C++ runtime acceptance.  CXXSMOKE.BIN runs
     static constructors via crt0's .init_array walk, operator new/delete,
     the __cxa_guard_* init guards, virtual dispatch, a class template and
     operator overloading — the whole clang-C++-to-libc.a link path.  It
     spawns nothing, so it needs no free process slots. */
  load_and_run_program_in_scheduler("CXXSMOKE.BIN", -1, -1, -1, -1);

  /* F2.2/F2.3: real-network DNS resolution via the DHCP-learned server;
     exits 0 (SKIP) when the network path is unavailable.  Spawns nothing,
     so it needs no free process slots after FPU_T's fork torture. */
  load_and_run_program_in_scheduler("DNSTST.BIN", -1, -1, -1, -1);

  /* P1 (docs/browser/p1-threads-design.md section 7): kernel threads +
     futex-lite + libpthread acceptance, then per-thread TLS + errno
     isolation.  Late positions: both fork children (exit_group cases) and
     THRD_T's 72 create/join cycles need process-table churn, which is
     cheapest once the earlier tests have exited. */
  load_and_run_program_in_scheduler("THRD_T.BIN", -1, -1, -1, -1);
  load_and_run_program_in_scheduler("TLS_T.BIN", -1, -1, -1, -1);

  /* P3 (browser.md §6): libc++ acceptance — CXX_T.BIN exercises the
     ported <vector>/<string>/<map>/<unordered_map>/<algorithm>/<memory>/
     <atomic>/<thread>/<mutex>/<condition_variable>/<chrono>/<sstream>
     over libcxx.a + libc.a.  It creates threads and waits on condition
     variables, so it runs after THRD_T/TLS_T (last in the wave). */
  load_and_run_program_in_scheduler("CXX_T.BIN", -1, -1, -1, -1);

  /* l3-rtti (browser.md §6/L6): libc++abi RTTI acceptance — RTTI_T.BIN
     exercises dynamic_cast up/down/cross/virtual-base and typeid over
     libcxx.a's RTTI closure (the four symbols the ICU link probe needed),
     compiled -frtti like the ICU cross build.  Spawns nothing; one slot,
     right after CXX_T. */
  load_and_run_program_in_scheduler("RTTI_T.BIN", -1, -1, -1, -1);

  /* P4 (docs/browser/p4-ipc-design.md §6.6): AF_UNIX IPC acceptance —
     socketpair + poll + SCM_RIGHTS fd passing, forks children.  LATE for
     the same reason as THRD_T (process-table churn). */
  load_and_run_program_in_scheduler("IPC_T.BIN", -1, -1, -1, -1);

  /* P2.2 (S2): v2 address-space acceptance.  MMTEST.BIN is the first
     AS_V2 program in the wave: loader v2 maps its image into fresh 4 KiB
     frames at USER_IMG_BASE (64 GiB) and commits the top of the v2 main
     stack; the syscall/teardown paths it exercises are the v2-aware ones.
     It spawns nothing and takes one slot, so a late position before the
     thread-torture suites is cheapest. */
  load_and_run_program_v2("MMTEST.BIN", -1, -1, -1, -1, 0);

  /* l3-icu-wire (browser L6): ICU 78.3 target acceptance — ICUSMK.BIN calls
     the data-free ICU C API subset (u_strlen, U8/U16 iteration macros, ASCII
     u_tolower, u_errorName, u_getVersion) over the target libicuuc.a +
     libcxx.a + libc.a closure; no libicudata.a is linked (see
     docs/browser/icu-usage-note.md).  Spawns nothing; one slot, positioned
     last so the process-table churn of the earlier suites has drained. */
  load_and_run_program_in_scheduler("ICUSMK.BIN", -1, -1, -1, -1);

  /* P6.2 (browser.md §6): SQLite acceptance — SQLTEST.BIN links the pinned
     3.49.2.0 amalgamation + the HobbyOS VFS/mutex port (SQLITE_OS_OTHER=1,
     THREADSAFE=1) over the P6.1 file API and record locks: create/insert/
     select on disk.img, journal lifecycle, a RESERVED lock handed across
     fork, VACUUM + integrity_check, reopen persistence.  Spawns a child
     (the cross-process lock check), so a late slot keeps it cheap. */
  load_and_run_program_in_scheduler("SQLTEST.BIN", -1, -1, -1, -1);

  /* P5 (docs/browser/p5-exec-signals-design.md): S3 signal-delivery
     acceptance (frame engine + SIGRETURN + SIGCHLD via the reap path +
     the D13 SIGUSR1 boundary) and S5 spawn_ex fd semantics (row 86,
     fdmap/CLOEXEC/copy).  Forks children and spawns SPAWNEX.BIN, so it
     runs late, next to the other churn suites. */
  load_and_run_program_in_scheduler("SIG_T.BIN", -1, -1, -1, -1);

  /* L8 WK-1 (browser.md §6 / docs/browser/wk1-feasibility.md OQ-9): libc/OS
     prerequisites for the WebKit JSC shell — clock_gettime (MONOTONIC +
     REALTIME via the SYS_GETTIME row 87 boot-anchored clock), gettimeofday,
     getentropy (SYS_GETRANDOM), sched_yield, sched_get_priority_min/max,
     isatty, and the pthread stack-bounds exposure (pthread_getattr_np over
     SYS_GETSTACK row 88) WebKit's ThreadingPOSIX/StackBounds need.  It
     creates a few threads (each thread consumes a process-table slot), so
     per the 'a launch-hungry test is loaded LAST' rule it sits at the END
     of the churn group, right before the wave's terminal TORTURE, so the
     fork/spawn-heavy suites ahead of it (IPC_T/MMTEST/SQLTEST/SIG_T) get
     the maximum drain before their children spawn. */
  load_and_run_program_in_scheduler("WK1C_T.BIN", -1, -1, -1, -1);

  /* P8.1 (browser.md §6): POSIX torture acceptance — 64-thread
     mutex/condvar churn, socketpair fd-pass loops, mmap/fault/free
     storms, exec/wait cycles, poll on many fds and memory high-water.
     LAST in the wave: its 64-wide churn wants the process-table churn of
     the earlier suites to have drained first (it tolerates and notes
     slot-pressure launch rejections).  Under MODE=soak
     (KERNEL_SOAK_WAVE) the same binary runs its bounded soak loop and
     prints the [SOAK] numbers (P8.2). */
#ifdef KERNEL_SOAK_WAVE
  load_and_run_program_in_scheduler_args("TORTURE.BIN", -1, -1, -1, -1,
                                         "soak");
#else
  load_and_run_program_in_scheduler("TORTURE.BIN", -1, -1, -1, -1);
#endif

  extern void kernel_exit(void);
  kernel_exit();
}
#endif

void main(void) {
  uart_init();
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
  extern void unix_init(void);
  unix_init(); /* P4: AF_UNIX pair pool (docs/browser/p4-ipc-design.md §2) */

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

  /* VFS routing table: NFS mounts live on top of the FAT volume. */
  extern void vfs_init(void);
  vfs_init();

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
  /* The test wave loads in a kernel thread (test_wave_loader).  Loading
     inline would deadlock: a load waits for a physical block when the
     pool is full (and always keeps WAVE_LOAD_RESERVE blocks free for
     child spawns), but blocks are only freed by tests exiting, and tests
     only run once the scheduler is started — which used to happen after
     the inline loads.  As a thread, the loader waits while the rest of
     the suite runs. */
  process_create_kernel_nowait(test_wave_loader, 0);
#elif defined(KERNEL_MODE_DESKTOP_TEST)
  uart_puts("Mode: DESKTOP_TEST - Launching desktop in test mode...\n");
  load_and_run_program_in_scheduler("EDITOR_T.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_APPS_TEST)
  uart_puts("Mode: APPS_TEST - Launching desktop app harness...\n");
  load_and_run_program_in_scheduler("APPS_T.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_PONG_TEST)
  uart_puts("Mode: PONG_TEST - Launching Pong test wrapper...\n");
  load_and_run_program_in_scheduler("PONG_T.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_FILEDIALOG_TEST)
  uart_puts("Mode: FILEDIALOG_TEST - Launching file dialog arrow key test...\n");
  load_and_run_program_in_scheduler("FILEDIAL.BIN", -1, -1, -1, -1);
#elif defined(KERNEL_MODE_JSC)
  /* WK-1 M3: single-program jsc smoke.  Boots ONLY JSC.BIN
     (the statically linked WebKit jsc shell, fork-built) with the smoke
     script SMOKE.JS as argv[1]; both files must be on the boot disk (the
     fork-side minimal-disk recipe assembles kernel + JSC.BIN + SMOKE.JS
     only).  fd 1/2 are unset -> the console-fd fallback (0002) relays
     jsc's stdout/stderr to the serial console. */
  uart_puts("Mode: JSC - Running WebKit jsc smoke (WK-1 M3)...\n");
  load_and_run_program_in_scheduler_args("JSC.BIN", -1, -1, -1, -1, "SMOKE.JS");
#elif defined(KERNEL_MODE_BIGLOAD)
  /* L8 x64-loadfix lane (repro): single ~30 MB v2 load before the
     scheduler — mirrors MODE=jsc's first load without the WebKit assets.
     The image never runs; the load itself is what must not fault. */
  uart_puts("Mode: BIGLOAD - single 30MB v2 load repro...\n");
  load_and_run_program_in_scheduler_args("BIG.BIN", -1, -1, -1, -1, 0);
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
