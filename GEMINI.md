# HobbyOS Design Document

This document captures the major design decisions, architecture constraints, and technical solutions implemented in HobbyOS to date. It serves as a reference for future development sessions to ensure consistency and correctness.

## 1. Hardware Target & Environment
- **Architecture:** AArch64 (ARM 64-bit).
- **Environment:** Bare-metal execution modeled around the QEMU `virt` machine (Cortex-A53).
- **Toolchain:** LLVM/Clang (`clang` and `ld.lld`) compiling for the `aarch64-none-elf` target.
- **Boot Mode:** The system executes entirely in Exception Level 1 (EL1) as a single-core OS, verified by checking `MPIDR_EL1` during the bootstrap in `boot.s`.

## 2. Memory Model Constraints
- **MMU Disabled Strategy:** Because the OS currently runs without the Memory Management Unit (MMU) activated, all physical memory maps default to `Device-nGnRnE` (non-Gathering, non-Reordering, non-Early Write Acknowledgement).
- **Alignment Penalty:** A strict consequence of the `Device` memory type is that **Unaligned Memory Accesses are strictly prohibited by hardware**.
- **Compiler Flags:** Clang aggressively uses NEON/SIMD registers (`q0-q31`) to optimize `memcpy` or array loop operations. Since the SIMD units are nominally disabled in EL1 without configuring `CPACR_EL1`, these optimizations trigger immediate Synchronous Exceptions. To remedy this natively, all compilation enforces the `-mgeneral-regs-only` flag.

## 3. Generic Interrupt Controller (GICv2)
- **Interrupt Routing:** The QEMU `virt` machine provides a GICv2 component. The kernel initializes the GIC Distributor and CPU Interface and routes all Non-Secure SPIs (Shared Peripheral Interrupts) to CPU 0. 
- **Exception Vectors:** A hardware vector table (`.align 11`) provides handlers for Synchronous, IRQ, FIQ, and SError traps. 
- **Context Preservation:** When an IRQ fires, `irq_wrapper` meticulously preserves the execution state. Notably, preserving the Link Register `x30` natively is critical before bouncing into compiled C handlers to avoid stack frame corruption upon `eret`.

## 4. VirtIO Storage Subsystem (MMIO Version 1 / Legacy)
- **Interface Selection:** We utilized the Memory Mapped IO (MMIO) specification for VirtIO over PCI. During runtime configuration, QEMU defines the disk block mechanism as a Legacy Version 1 device (`virtio-blk-device` on standard flags).
- **Wait-For-Interrupt (WFI):** To preserve CPU cycles and adhere to efficient hardware scheduling, the driver avoids busy-polling. Read/write ops dispatch Virtqueues, set the `used` acknowledgment checks, and run the `wfi` instruction—which legitimately sleeps the ARM core until the GIC controller wakes it via device assertion.
- **Physical Contiguous Structures:** VirtIO Version 1 requires strictly contiguous queue arrays (`desc`, `avail`, padding, `used`) bounded physically by the `VIRTIO_QUEUE_PFN` and Page Size offsets (4096). These queues are statically allocated to bypass the need for a dynamic heap.

## 5. FAT-16 Filesystem 
- **Direct Interpretation:** The OS includes a minimal native parser for FAT-16 tailored specifically for 512-byte block sectors.
- **Atomic Byte Parsing to avoid Traps:** Packed structs traditionally map out FAT structures like the BPB (BIOS Parameter Block). However, due to `bytes_per_sector` resting unalignedly at offset 11, Clang optimizes fetches using half-word 16-bit loads (`ldrh`), raising fatal alignment faults against the `Device` memory. 
- **Design Enforcement:** As a strict rule moving forward, HobbyOS maps raw FAT metadata segments directly using `volatile uint8_t*` and parses offsets byte-by-byte into integers natively to completely defeat compiler-introduced Unaligned Access vectors.

## 6. Virtual Memory Protection (MMU)
- **Identity Mapping:** Rather than offsetting higher-half kernel variables arbitrarily, the entire memory structure explicitly implements identity mappings spanning `0x00000000` to `0x7FFFFFFF` utilizing 2MB contiguous blocks via Level 2 tables dynamically allocated internally.
- **Cache Avoidance:** All standard Kernel and RAM buffers operate as `MAIR_NORMAL_NC` (Normal Non-Cacheable). Maintaining purely Non-Cacheable architecture provides unaligned-access functionality inherently derived from Normal buffers, while completely bypassing painful architecture-dependent data cache flushes linking the Host DMA from VirtIO devices directly without coherence syncing.
- **Unprivileged Blocks (EL0):** To separate application context boundaries, `0x44000000` specifically translates mapping constraints strictly to `PT_USER_RW` unlocking dual-execution boundaries without flushing TTBR1 registers explicitly. 

## 7. Execution Hand-Offs & Syscalls
- **Trap Structures (SVC):** User mode programs compile dynamically into `.bin` containers triggering the `svc #0` hardware traps bypassing conventional external bindings. Utilizing `irq_lower_el` and `sync_lower_el`, the `boot.s` wrappers capture context frames and redirect them down to internal OS handlers.
- **SPSR Protection:** Exiting EL1 to execute a User process restricts down hardware properties naturally via shifting the process register down to `EL0t` and relying securely on `eret` branching safely natively towards target instruction frames allocated via Virtual Memory dynamically.

## 8. Build Modes & Testing Environment
HobbyOS provides multiple distinct build and execution targets through its Makefile, designed to support both kernel-level validation and high-level user-space integration:
- **Default / Desktop (`make run`)**: Compiles the OS defining `KERNEL_MODE_DESKTOP`. The kernel boots, initializes all drivers, and starts the window manager/desktop environment natively.
- **Integration Tests (`make test`)**: Compiles with `KERNEL_MODE_TEST`. Instead of booting the desktop, the kernel's scheduler automatically spawns a predefined sequence of user-space test binaries (e.g., `fork_test.bin`, `smp_test.bin`, `pipe_test.bin`) to validate syscalls and parallel multi-core process execution.
- **Kernel Unit Tests (`make unit_tests`)**: Compiles with `KERNEL_MODE_UNIT_TEST`. This isolates and tests kernel subsystems directly in EL1 (e.g., file system reads, lock behaviors, trap dispatchers, process control blocks) before the scheduler even starts. Upon test completion, QEMU forcefully halts.
- **Automated Desktop Test (`make desktop_test`)**: Compiles with `KERNEL_MODE_DESKTOP_TEST`. Boots the desktop environment but immediately auto-launches an arbitrary UI application to validate the graphical framebuffer pipeline and event subsystems without requiring manual user input.
- **Host Tests (`make host_tests`)**: Compiles and runs user-space "golden" test executables (e.g., `edit_host`) directly on the macOS host environment. These tests validate core functionalities like the file system and GUI components using standard Unix APIs, providing a fast, reliable baseline before deploying to the emulated kernel.
- **General Knowledge**: If tests take more than 30 seconds to complete, stop the tests and consider that the tests took this long as a sign of a defect, likely a deadlock, a fault, or some other issue that should be investigated and fixed. These kinds of failures don't always provide a log or a clean indication of failure.

## 9. Non Negotiable Requirements
- **SMP**: This system must always support SMP and at least 4 cores or more. Do not decrease the number of cores below 4.
- **ARM64**: This system is targeting ARM64 CPUs and no other CPUs
- **Multi-Processing**: This sytem must always support multi-processing and inter-process communication (IPC).
- **Memory Protection**: This system must always support memory protection and sandboxing.
- **User Mode**: This system must always support user mode and kernel mode.
