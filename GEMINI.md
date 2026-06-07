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
- **Test Shutdowns**: Some tests launch a graphical environment, when it is the desktop, the default behavior of the desktop or user interface applications is to keep running. When tests reach a success or failure point, it is important that tests can shut down qemu upon success or failure when all tests have completed.

## 9. Non Negotiable Requirements
- **SMP**: This system must always support SMP and at least 4 cores or more. Do not decrease the number of cores below 4.
- **ARM64**: This system is targeting ARM64 CPUs and no other CPUs
- **Multi-Processing**: This sytem must always support multi-processing and inter-process communication (IPC).
- **Memory Protection**: This system must always support memory protection and sandboxing.
- **User Mode**: This system must always support user mode and kernel mode.

## 10. Network Stack
- **VirtIO Network Device:** Network communication is driven by the VirtIO MMIO Network device (`virtio-net-device`). It utilizes split TX and RX virtqueues for packet transmission and reception.
- **Protocol Support:** The kernel implements a minimalistic IPv4 stack focusing exclusively on UDP. TCP is not supported. Incoming Ethernet frames are parsed by `net_rx_packet`, validated for IPv4 and UDP, and dispatched to the appropriate Protocol Control Block (PCB).
- **Dynamic Configuration (DHCP):** During boot, the OS broadcasts a DHCP Discover packet to dynamically acquire an IPv4 address, subnet mask, and router IP from the QEMU slirp network backend.
  - *Intel/x86_64 Architecture Parity:* DHCP dynamic configuration must also remain fully functional and enabled on the x86_64 architecture. Avoid shutting down, bypassing, or disabling core network features like DHCP in favor of hardcoded static IP configurations for x86_64.
- **Socket Abstraction:** User-space networking is accessed via the `SYS_CONNECT` system call (for UDP outbound connections). The kernel allocates a `net_pcb` (Protocol Control Block) and maps it into the process's file descriptor table as a Socket (`f->type = 2`). File operations (like `sys_close`) manage the lifecycle of these sockets, ensuring resources are freed upon process termination.

## 11. Remote PCIe Device Sharing over UDP/IP + RDMA (Intel/x86_64 Edition)
HobbyOS contains a high-performance bare-metal subsystem to share physical PCIe Express devices over the network to other instances of the OS. This capability is specifically targeting x86_64 architecture deployments.

### A. Design Details & Core Interface
- **Unified Remote Bus Interface**: A kernel daemon emulates the registers of the remote PCIe device (BAR0 MMIO region) over the network. The guest kernel reads and writes to this virtual device transparently using the compile-time Virtual Consumer API (`v_edu_read32`, `v_edu_write32`, etc.).
- **Role Selection**: The kernel designates instances dynamically as:
  - **Host (Provider)**: The instance containing the physical PCIe card. It intercepts incoming RDMA requests, executes the physical register/memory reads and writes, and transmits responses back.
  - **Receiver (Consumer/Guest)**: The instance utilizing the remote device. It has no physical PCIe device but runs virtual device APIs that route operations to the Host over the network.

### B. Bootup Options & Addressing Configuration
To ensure the capability is only activated when explicitly configured, the remote PCIe sharing subsystem utilizes QEMU's standard **Firmware Configuration (`fw_cfg`)** Port I/O interface. It is completely inactive by default.
- **Port I/O Registers**: Queries `0x510` (Selector, 16-bit) and `0x511` (Data, 8-bit) to locate directory entries.
- **Key Location**: Scans for the file `opt/pcishare`.
- **String Format**: `role:vendor_id:device_id` (e.g. `host:0x1234:0x11e8` or `guest:0x1234:0x11e8`).
- **Default Bypassing**: If the configuration parameter is not passed at startup, the stack marks `g_rdma_active = 0`. Standard unit tests automatically bypass remote sharing checks, preventing boot hangs or dependency timeouts.

### C. Network Implementation (UDP/IP RDMA)
- **Zero TCP Dependency**: To maximize throughput and guarantee predictable bare-metal latencies, data transfer runs strictly over a UDP/IP stack on Port `7777`, completely bypassing heavy-weight TCP state machines.
- **Legacy PCI Network Driver**: The x86_64 stack uses a custom legacy `virtio-net-pci` driver with a 256-descriptor ring layout, direct polling, and 8192-byte used-ring physical alignment, enabling sub-millisecond network packets.
- **RDMA Opcode Protocols**: Bypasses traditional interrupt-bound networking using a synchronous request-response protocol mapping:
  - `RDMA_OP_READ_REQ` / `RDMA_OP_READ_RESP` (MMIO Register Reads)
  - `RDMA_OP_WRITE_REQ` / `RDMA_OP_WRITE_RESP` (MMIO Register Writes)
  - `RDMA_OP_REG_MR` (Memory Region Registrations)
  - `RDMA_OP_DMA_SYNC_TO_HOST` / `RDMA_OP_DMA_SYNC_TO_GUEST` (DMA Buffer Synchronizations)

### D. Memory Layout & Shadow DMA Buffer Syncing
Since physical PCIe devices directly access physical host memory (DMA) without standard CPU page translation, the subsystem implements a custom **Memory Region (MR) Translation Table**:
- **Address Mapping**: When the Guest allocates a buffer in its local memory space (`guest_phys`), it registers this memory region with the Host via `RDMA_OP_REG_MR`.
- **Shadow Allocation**: The Host allocates a physically contiguous block of standard RAM on the provider node (`host_phys`) to serve as a shadow bounce buffer.
- **Synchronous Syncing**: Before starting a device DMA transaction, the Guest flushes its local data over the network to the Host's shadow buffer (`RDMA_OP_DMA_SYNC_TO_HOST`). The physical PCIe device executes the DMA transfer using `host_phys`. Once complete, the Guest pulls the modified shadow buffer data back over the network into its local memory (`RDMA_OP_DMA_SYNC_TO_GUEST`), maintaining coherent virtual memory protection across machine boundaries.

## 12. Intel-only Testing Servers
- Available at 192.168.10.174 is a Proxmox Server with a large number of CPU cores, and RAM. It can be connected to via ssh with a command: ```ssh -i ~/.ssh/mac_to_r1 root@192.168.10.174```
- The server at 192.168.10.174 has an Nvidia RTX 4090 GPU at PCIE address 0000:01:00 which is available to be used in QEMU VMs that this proxmox system hosts
- An example host OS that has this GPU connected, has a configuration file located at /etc/pve/nodes/r1/qemu-server/117.conf . This is an example file and should not be modified. see the line with 'hostpci0' for the example configuration line
- Example commands for how to use the testing server (The VM ID number is 201 in each command):
  - To create a new VM: ```qm create 201 --name HobbyOSHost1 --cores 4 --memory 4096 --net0 virtio,bridge=vmbr0 ```
  - To add a 32GB disk to VM: ```qm set 201 --scsihw virtio-scsi-pci --scsi0 local-lvm:32,format=raw ``` 
  - To start a VM: ```qm start 201```
  - To check console output (Serial): ```qm terminal 201```
  - To stop a VM: ```qm stop 201```
  - To delete a VM: ```qm destory 201 --purge```
- Example of how to import a disk into a new VM:
  ```bash
  qm create 100 --name ImportedVM && \
  qm importdisk 100 /root/disk.raw local-lvm && \
  qm set 100 --scsi0 local-lvm:100/vm-100-disk-0 --bootdisk scsi0 --boot d && \
  qm start 100
  ```

## 13. Virtual IOMMU (vIOMMU) Support for Remote PCIe Sharing
HobbyOS supports a virtual Intel IOMMU (VT-d) emulated by QEMU to enable transparent DMA address translation for remote PCIe device sharing. This is critical for modern GPUs (e.g., NVIDIA RTX 4090 with GSP firmware) that embed I/O Virtual Addresses (IOVAs) in deeply nested DMA structures.

### A. Architecture Overview
- **Problem:** Modern GPU drivers (NVIDIA GSP firmware) embed hundreds of DMA addresses inside memory structures that the `net_pci_client` cannot intercept via MMIO register sniffing alone.
- **Solution:** With a vIOMMU enabled in both guest and host VMs, the GPU driver uses IOVAs instead of raw Guest Physical Addresses (GPAs). The IOMMU hardware translates IOVAs to physical addresses transparently, solving the embedded-address problem.
- **QEMU Configuration:** Both VMs use `-machine q35,accel=kvm,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on`.

### B. Intel VT-d IOMMU Driver (`iommu_vtd.c`)
- **x86_64 Only:** All code is guarded by `#ifdef __x86_64__`. The ARM build compiles an empty translation unit.
- **ACPI Discovery:** The driver scans the BIOS area (0xE0000–0xFFFFF) for the RSDP signature, follows RSDP → XSDT → DMAR table, and extracts the DRHD register base address.
- **3-Level Page Tables:** Uses 39-bit IOVA space (AGAW=1) with statically allocated root table, context tables (4 buses), L2 tables (64), and L1 tables (512).
- **API:** `iommu_vtd_init()`, `iommu_vtd_map(bus, devfn, iova, phys, size)`, `iommu_vtd_unmap(bus, devfn, iova, size)`, `iommu_vtd_invalidate_iotlb()`.

### C. RDMA Protocol Extensions
- **`RDMA_OP_IOMMU_MAP` (14):** Guest → Host: "Map this IOVA to a shadow buffer and program your IOMMU." The host allocates a shadow buffer from a 32MB bump-allocator pool and programs its VT-d IOMMU.
- **`RDMA_OP_IOMMU_MAP_RESP` (15):** Host → Guest: Returns the host physical address of the shadow buffer.
- **`RDMA_OP_IOMMU_UNMAP` (16):** Guest → Host: "Tear down this IOVA mapping."
- **`RDMA_OP_IOMMU_UNMAP_RESP` (17):** Host → Guest: Acknowledges unmapping.
- **`RDMA_OP_DMA_SYNC_TO_HOST` (7):** Updated to check IOMMU shadow buffers first. Data addressed to an IOVA lands in the correct shadow buffer.

### D. net_pci_client IOMMU Mode
- **Activation:** Pass `--iommu` as a command-line flag: `./net_pci_client /tmp/sock 10.0.2.16 0x10de 0x2684 --iommu`.
- **DMA Callback Behavior:** With `caching-mode=on`, QEMU sends fine-grained DMA MAP/UNMAP events for every IOMMU page table change. The `dma_register_cb` records each IOVA and the `irq_thread` asynchronously sends `RDMA_OP_IOMMU_MAP` + data sync to the host.
- **Legacy GPA Sniffing:** The old GPA sniffing logic in `bar_access_cb` is bypassed when IOMMU mode is active. The vIOMMU handles all address translation transparently.
