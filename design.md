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
