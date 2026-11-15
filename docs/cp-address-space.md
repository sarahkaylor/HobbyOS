# The compat address space on x86_64 — identity-map inventory and the direct-map design

Status: **design note, nothing implemented.** Written 2026-11-14 by lane C1-as
for `docs/cyberpunk.md` P1 (its first task: "Design note
`docs/cp-address-space.md`") and AD-3. Every statement about HobbyOS carries a
`path:line` read in this tree (branch `cp-as`, base `f7a51cfa`). Statements
about Wine are marked **verify** and close at the Wine pin (P0); none was
measured here. Measurements made for this note are labelled *measured* and
say how.

Companion docs: `docs/cyberpunk.md` (§0, §3.2, §5 AD-3, §6 P1, §7, App. A F1),
`docs/browser/p2-vm-design.md` (the v2 address-space design this note
extends), `docs/linux-abi.md` (the compat contract), `docs/elf-loader.md`.

---

## 0. Summary

- **Inventory** (exhaustive for int↔pointer conversions, reproducible by the
  AST scan in Appendix A): the default x86_64 kernel build has **539**
  int↔pointer cast sites. **219 depend on the identity map (208 class A,
  mechanical; 11 class B, need design); 2 more are class C** (ARM-only code
  compiled on x64). The other 318 are user virtual addresses (185) or
  not address translations at all (133). Unit-test-only code adds 82 A;
  the opt-in test vehicles (`-D` mode flags) add 6 A. ARM-only code has 214
  sites, all class C (unaffected). Twelve dependencies are not casts at all
  (boot tables, walkers bounded to the native window, MMIO windows, the frame
  pool's hard-coded extents, one fault gate); they are listed as design items
  D1–D12, all class B. **Class B total: 11 call sites + 12 design items = 23.**
- **AD-3 fallback check: does not fire** (23 non-mechanical items against a
  trigger of >50; §6).
- **The kernel image does not have to move.** It is linked and loaded at
  `0x70000000` (`linker_x64.ld:7`) and *measured* to end at `__stack_top =
  0x75c72b90` (§1.1). That range sits above the top of Wine's expected low
  reservation (`0x68000000`) and below `KUSER_SHARED_DATA` at `0x7ffe0000`
  (Wine ranges are **verify**). The recommended design (K0) keeps
  the image at its 1:1 address, maps that 94 MiB supervisor-only in every
  address space, and leaves it as a fixed hole in the compat layout. Moving
  the kernel to the higher half (K1) needs a new boot protocol, because the
  kernel boots as an ELF32 multiboot1 image (`Makefile:946-948`, `:2605`).
  K1 is deferred behind a stated trigger (§3.1).
- **A dependency the plan does not state:** the x64 frame pool stops at 8 GiB
  physical whatever `-m` says. It has 5.25 GiB usable in today's 12 GiB guest
  (`frame.h:36-39`), and no RAM map is parsed at all (§1.8). So the plan's
  "≥16 GiB guest" (`docs/cyberpunk.md` §4 Memory row, R4) is blocked on the
  same direct map, plus a multiboot memory-map parser.
- **Second unstated fact (affects the GS work in the same lane):** the x64
  kernel has no `swapgs` anywhere. `GS_BASE` and `KERNEL_GS_BASE` both hold
  `&cpu_locals[cpu]` (`trap.c:5934-5941`), and `syscall_entry` dereferences
  `gs:` immediately (`trap.c:6058-6059`). `docs/cyberpunk.md` §3.2 says
  "kernel uses `swapgs` + `KERNEL_GS_BASE`", which is not what the code does
  (§1.5, amendment in §8).
- **Migration** is 9 steps (S0–S8, §4). Each is gated by tests that exist today,
  and the first two change no behaviour. The tripwire for leftover identity
  dereferences goes in the **native** v2 address spaces (S5), because in a
  compat address space a leftover dereference can land on a user page
  silently and not fault (§3.8).

---

## 1. How x86_64 address spaces work today

### 1.1 Boot path and where the kernel image lives

- Limine boots the kernel with `protocol: multiboot1` (`Makefile:2605`). The
  ELF is linked for x86_64 and then converted in place to `elf32-i386`
  (`Makefile:946-948`), because multiboot1 loads ELF32. Linking uses
  `LDFLAGS = -T linker_x64.ld` (`Makefile:307`) and no `-mcmodel`
  (`Makefile:285`), so the default small code model applies: every kernel
  symbol must be below 2 GiB.
- `linker_x64.ld:7` sets `. = 0x70000000`. VMA and LMA are equal, so the
  image runs at virtual = physical. The boot stacks are the 512 KiB after
  `.bss`, ending at `__stack_top` (`linker_x64.ld:51-53`).
- *Measured* (built from this tree on linuxci with `make ARCH=intel
  hobbyos.elf`, default MODE, then `readelf -l` / `nm`): three LOAD segments,
  `0x70000000` R E `0xcfd75`, `0x700d0000` R `0x9c30`, and `0x700da000` RW
  filesz `0x604e` memsz `0x5b18b84`. Symbols: `__bss_start = 0x70200000`,
  `__bss_end = 0x75bf2b84`, `__stack_top = 0x75c72b90`, `boot_pml4 =
  0x700db000`, `cpu_pml4 = 0x75b5f000`. The image therefore spans
  **[0x70000000, 0x75c72b90)**: 92.4 MiB, nearly all of it `.bss`. A comment
  at `trap.c:3441` records an earlier `__stack_top` of `0x755ffbf0`, so the
  image is growing.
- `boot.s:31-72` builds `boot_pml4 → boot_pdpt → boot_pd0/boot_pd1`, an
  identity map of 0–2 GiB in 2 MiB pages (`0x83`). The boot code then loads
  CR3, enables PAE, LME and NXE (`boot.s:94`), turns on paging and far-jumps
  to `long_mode_entry`. `_start` never saves EBX, so the multiboot information
  pointer is lost (`boot.s:25-133`).
- `main.c:498` calls `mmu_init()` (`mmu.c:180-185`). That builds the real
  tables and loads `cpu_pml4[0]` (`mmu.c:153-171`).

### 1.2 The kernel tables (static, in `.bss`)

`mmu.c:12-27` declares them all as kernel-image arrays. A table's *address*
doubles as its *physical address* everywhere (`mmu.c:107-131`, `:154`,
`:482-489`, `:676`).

```
cpu_pml4[c][0] ──► cpu_pdpt[c]            (per CPU, 0x07 = P|RW|US at upper levels)
                    [0] ► cpu_pd0        0–1 GiB    shared   2 MiB 0x83 (WB, no US)
                    [1] ► cpu_pd1[c]     1–2 GiB    PER CPU  kernel + v1 user overlay
                    [2] ► cpu_pd2        2–3 GiB    shared   (32-bit PCI hole on q35)
                    [3] ► cpu_pd3        3–4 GiB    shared   (LAPIC/IOAPIC/BGA LFB…)
                    [4..7] ► cpu_pd4..7  4–8 GiB    shared   RAM; PD7 also carries MMIO windows
pd1_kernel          1–2 GiB, kernel-only twin of cpu_pd1 for v2 roots (mmu.c:22-27, :144-147)
```

- Leaves are 2 MiB with `0x83` = P|RW|PS. There is no US bit, and NX is clear.
  The PCI hole and LAPIC are mapped WB (`mmu.c:55-67`) and get uncached
  behaviour only through the firmware's MTRRs. Only the dynamically mapped
  MMIO ranges use `0x9B` (PCD|PWT) (`mmu.c:317`, `:364`).
- The v1 user overlay lives *inside* the 1–2 GiB identity PD. A v1 process's
  32 MiB block is mapped at `USER_VIRT_BASE = 0x44000000`
  (`process.h:101-106`) by rewriting `cpu_pd1[cpu][32..47]`
  (`mmu.c:191-216`), and its framebuffer at `0x60000000` goes through
  `cpu_pd1[c][256..]` (`mmu.c:221-253`).

### 1.3 MMIO today

- **LAPIC** at `0xFEE00000` is used through the identity map at fixed
  addresses: `gic.c:50`, `timer.c:9-12`, `smp.c:6-7`, `mmu.c:592-593`,
  `process.c:2777-2778`, `trap.c:3755` (EOI on every interrupt), and
  `trap.c:4736`.
- **BAR windows inside the identity range.** The NVMe BAR is remapped at VA
  `0x1FC000000` by overwriting `cpu_pd7[480]` (`virtio_blk.c:1725-1726`,
  `:1738`). The virtio-gpu BAR (observed at `0xC000000000`) is remapped at VA
  `0x1FE000000` through `cpu_pd7[496..497]` (`virtio_gpu_x64.c:385-427`).
  Because PD7 is an identity table, each window hides the RAM at the same
  physical 2 MiB. The frame pool reserves that "shadow" for this reason
  (`frame.h:40-46`, `frame.c:205-207`, `virtio_blk.c:1756`).
- **`mmu_map_mmio_range`** (`mmu.c:277-381`) maps a physical range at
  *VA = PA*. It walks `cpu_pml4[0]`, allocates from four static PDPTs and 64
  PDs (`mmu.c:268-275`), and stores the result into every CPU's
  `cpu_pml4[c][pml4_idx]` (`mmu.c:312-314`, `:340-342`). Two latent defects,
  both pre-existing and recorded here because the redesign removes them:
  1. For any address below 512 GiB (`pml4_idx == 0`), the PDPT it finds is
     CPU 0's private `cpu_pdpt[0]` (`mmu.c:299-300`, `:327-328`). The loop
     then points *every* CPU's PML4[0] at it, so all CPUs share CPU 0's
     `cpu_pd1[0]` and therefore its v1 overlay.
  2. It never touches v2 roots, whose PML4 copies only PDPT[0..7]
     (`mmu.c:479-489`). A BAR mapped above 8 GiB is therefore invisible while a
     v2 process's CR3 is loaded.

  Its only callers are on the RDMA / VT-d path (`iommu_vtd.c:120-240`, `:510`,
  `net_rdma.c:1156`, `:1185`). The TLB flush is a local CR3 reload whose
  comment calls it "sufficient in QEMU virt" (`mmu.c:242`, `:370-378`).

### 1.4 The three kinds of address space

| kind | root | who | built by | user window |
|---|---|---|---|---|
| kernel / v1 | `cpu_pml4[cpu]` (static, per CPU) | kernel tasks (`process_create_kernel_internal` → `process_create()` → `AS_V1`, `process.c:1363`, `:1374`), legacy flat loads (`program_loader.c:242`) | `mmu_init_tables` | v1 overlay at `0x44000000` in `cpu_pd1[cpu]` |
| v2 native | per-AS PML4 frame `as->root_phys` | every ELF / modern process (`process_create_v2`, e.g. fork at `process.c:4191`) | `vm_arch_root_alloc` (`mmu.c:469-492`) | `USER_VA_BASE` 64 GiB + 32 GiB (`vm.h:27-29`), PDPT[64..95] |
| compat | — (does not exist) | — | — | — |

- **Creating an AS.** `vm_as_create` (`vm.c:152-230`) takes the pool slot
  keyed by tgid, allocates the region array from a frame (`vm.c:190-198`) and
  calls `vm_arch_root_alloc` (`vm.c:216`). That allocates a PML4 and a PDPT
  frame and links PML4[0] → PDPT. PDPT[0..7] point at the **shared kernel PDs**
  `cpu_pd0, pd1_kernel, cpu_pd2..7` (`mmu.c:479-491`). The user window's
  PD/PT frames are allocated on demand by `x64_v2_walk_alloc`. That walker
  only understands PML4[0] and PDPT indices 64..95 (`mmu.c:423-467`, bound at
  `:433`).
- **Tearing down.** `vm_as_teardown` (`vm.c:231-300`) → `vm_arch_teardown`
  (`mmu.c:703-773`). It hands this CPU back to its kernel PML4 if needed
  (`:710-711`), reports any other CPU still translating through the root
  (`:725-742`), then frees PDPT[64..95] subtrees, the PDPT and the PML4. Leaves
  marked `X64_PTE_SHARED` (software bit 9, `mmu.c:399`) are not freed.
- **Switching.** The scheduler calls `vm_arch_switch` for a v2 slot and
  `mmu_switch_user_mapping` for v1 (`process.c:1984-1987`, also `:4735-4741`).
  `vm_arch_switch` writes CR3 and records `v2_active_root[cpu]`
  (`mmu.c:680-684`). Idle entry and kernel threads go back to `cpu_pml4[cpu]`
  through `vm_arch_restore_kernel` (`mmu.c:671-678`, called at
  `process.c:2523`, `:2558`, `:2702`, `:4856`).

### 1.5 Kernel entry while a user AS is loaded

- No CR3 switch happens on `syscall` or on any interrupt. The kernel runs on
  whatever root is current, so every kernel mapping must be present in every
  root. Today that holds because PML4[0] → PDPT[0..7] are shared.
- `syscall_entry` (`trap.c:6054-6071`) does `mov gs:[8], rsp; mov rsp,
  gs:[0]` with no `swapgs`. A grep for `swapgs` over
  `src/kernel/arch/x64/` finds only two comments (`mmu.c:168`,
  `trap.c:5917`). Both GS MSRs are set to `&cpu_locals[cpu]`, a kernel-image
  object (`trap.c:5934-5941`). The per-CPU kernel stack is in the image
  (`trap.c:5879-5880`, `:5919`), and so are the TSS, IDT and GDT
  (`trap.c:5884`, `:5946`, `boot.s:270-307`). LSTAR points at image text
  (`trap.c:5985`). **Every one of these is a kernel-image VA. The image must
  be mapped in every root before the first instruction of any entry path.**
- SFMASK clears only IF (`trap.c:5991`).
- No SMAP or SMEP: CR4 gets only bits 5, 9 and 10 (`boot.s:79-81`,
  `fpu.c:72-74`). The kernel dereferences user VAs directly after validation.
  For v2, validation is region-based through `vm_touch` (`vm.c:1845-1860`).
  The x64 syscall layer funnels into it via `sys_user_range`
  (`trap.c:658-669`) and `process_user_ok` (`process.c:2731-2742`).

### 1.6 TLB, PCID and shootdown

- No PCID (CR4.PCIDE is never set) and no global pages (CR4.PGE is never set;
  same CR4 writes as above). Every CR3 write flushes everything.
- User changes go through `vm_arch_map/unmap/prot` with a local `invlpg`
  (`mmu.c:418-420`, `:494-527`) and `vm_arch_shootdown` (`mmu.c:615-651`).
  The shootdown sends vector `0x82` to the CPUs whose current process is in
  the AS, and the handler reloads CR3 (`mmu.c:655-665`). The sender spins,
  bounded, for acks. There is also a full local reload helper
  (`mmu.c:577-582`).
- Kernel-half changes (MMIO windows, `mmu_map_mmio_range`) flush **locally**
  only (`mmu.c:242-250`, `:370-378`, `virtio_gpu_x64.c:410-417`). The NVMe
  window is installed after `smp_init` (`main.c:528` then `:534`).

### 1.7 SMP bring-up

`smp_init` (`smp.c:21-76`) copies `trampoline_start..trampoline_end` to
physical `0x7000` through the identity map (`smp.c:27-34`). It sets
`smp_temp_stack`, an image VA (`smp.c:40`), and sends INIT/SIPI. The AP runs
16-bit, then 32-bit code at `0x7000`. It loads **`boot_pml4`**
(`boot.s:205-207`; identity 0–2 GiB), enables paging and far-jumps to
`trampoline_lm`, which is at its *link* address in the image (`boot.s:239`,
`:244`). `secondary_main` (`main.c:765-772`) then switches to `cpu_pml4[cpu]`
via `mmu_init_core_with_id`. APs never run on a process root before this.

### 1.8 Physical memory: the frame pool and its 8 GiB cap

- The pool is two fixed extents, `[0x20000000, 0x70000000)` and
  `[0x100000000, 0x200000000)` (`frame.h:36-39`). That is
  327,680 + 1,048,576 frames = **5.25 GiB**. A frame's address is used as its
  kernel VA (`frame.c:68-78`; `frame_alloc_zeroed` stores through it,
  `frame.c:315-325`).
- The extents end at 8 GiB *because the identity map does*. The guest has
  12 GiB (`Makefile:332`, `-m 12288M`), laid out by QEMU as
  `0x100000–0x7FFFFFFF` and `0x100000000–0x37FFFFFFF` (`frame.c:30-42`). So
  **6 GiB of RAM above 8 GiB is never used**, and nothing reads a memory map:
  EBX is dropped at boot (§1.1). A larger `-m` changes nothing until the
  kernel can reach physical memory above 8 GiB.
- The kernel image's physical range is reserved from the pool through its
  *virtual* bounds (`frame.c:216-222`). That is another place that relies on
  VA == PA.

### 1.9 How ARM differs (and why it may stay)

ARM identity-maps 0–18 GiB through TTBR0 L1[0..17] (`arch/arm/mmu.c:21-28`,
`:180-195`). TTBR1 is disabled (`EPD1`, `arch/arm/mmu.c:318`), the v2 user
window is L1[64..95] (`arch/arm/mmu.c:474`), and the ASID is carried in
TTBR0[63:56] (`arch/arm/mmu.c:668-674`). The kernel is linked at
`0x40080000` (`linker.ld:9`), but Limine loads it near the top of RAM
(`Makefile:66-70`). Compat is x86-only (`docs/cyberpunk.md` AD-11), so ARM
keeps all of this. The only obligation is that shared code converted to the
new helpers still compiles and behaves identically on ARM. §3.3 makes the
helpers the identity function there.

---

## 2. Identity-map dependency inventory

### 2.1 Method (how to reproduce the counts)

A grep for casts cannot tell an integer-to-pointer cast from a
pointer-to-pointer cast, so the inventory comes from **the compiler's own
AST**. Every kernel translation unit is parsed with the real kernel CFLAGS
(`Makefile:285` for intel, `:336` for arm). Every `CStyleCastExpr` or
`ImplicitCastExpr` of kind `<IntegralToPointer>` or `<PointerToIntegral>` is
recorded with its spelling location. In C, any use of a physical address as
a pointer, or of a pointer as a physical address, has to pass through one of
these two conversions; implicit ones would also appear, and there are none.
That makes the set exhaustive for its question. The scanner is Appendix A.
Runs:

| pass | flags | unique sites |
|---|---|---|
| x64 default | intel CFLAGS | **539** |
| x64 + unit tests | `+ -DKERNEL_MODE_UNIT_TEST` | 643 (104 more) |
| x64 + every `-D` the Makefile can set | all 31 flags from `grep -oE 'CFLAGS \+= .*' Makefile` | 665 (22 more) |
| arm default | arm CFLAGS | 382, of which **214** are absent from the x64 passes |

Every site was then classified by reading it. The per-line verdicts for
A/B/C are §2.3, U/N are counted per file in §2.5, and the classifier that
produced the counts is Appendix A.2. The rule for each class:

- **A, mechanical.** The fix is a local rewrite at that line, or at the callers
  of one integer-address helper, using `phys_to_virt()`, `virt_to_phys()` or
  `kimg_pa()` (§3.3). It changes no data structure, lock, mapping policy or ABI.
- **B, needs design.** It needs a mapping-policy decision (MMIO above the
  direct map, attributes), a page-table *shape* change, or early-boot handling.
- **C, ARM-only or unaffected.**
- **U, not a dependency.** A user virtual address in the current AS.
- **N, not a dependency.** A kernel VA used as a VA (IDT/TSS/GS/stack/function
  pointers), or a lock address, PC, or other value used for diagnostics.

Casts cannot see one thing: a phys-or-VA integer handed to a function that
takes `uint64_t` and casts inside. Those helpers were traced by hand. The
`fat16_read_direct*` family takes a "physical address integer"
(`program_loader.c:1152-1153` says so). Its callers pass frame addresses
(`elf_loader.c:118`, `vm.c:1309-1312`, `:1380`, `:1531`, `:1639`, `:2199-2203`,
`fat16.c:2863`, `:2928`, `trap.c:5242`, `program_loader.c:365`) or
kernel-image VAs (`elf_loader.c:808-831`, `program_loader.c:1155`,
`fat16.c:2849`). The helper's internal casts (`fat16.c:2567`, `:2592`,
`:2786`) are counted once as A. Under K0 a kernel-image VA *is* its physical
address, so callers of both kinds stay correct once the inside becomes
`phys_to_virt`. On x64 the NVMe driver never DMAs into a caller buffer: it
bounces through `bounce_buf` (`virtio_blk.c:2011`, `:2082`, `:2113-2118`). So
this helper family needs no device-side change.

### 2.2 Counts

| scope | A | B | C | U | N | total |
|---|---|---|---|---|---|---|
| x64 default build (production) | **208** | **11** | **2** | 185 | 133 | 539 |
| + unit-test-only code | 82 | 0 | 0 | 13 | 9 | 104 |
| + opt-in test vehicles (`-D` flags) | 6 | 0 | 0 | 6 | 10 | 22 |
| ARM-only code | — | — | **214** | — | — | 214 |
| design items without a cast (§2.4) | — | **12** | — | — | — | 12 |
| **identity-dependent, x64 production** | **208** | **23** | 2 | | | |

A by category (production): 108 frame dereferences, 62 kernel-image objects
used as physical addresses (page-table links, CR3, DMA rings, framebuffer),
5 kernel VAs passed as physical integers, 3 physical-integer helpers,
15 fixed sub-4 GiB MMIO (LAPIC, BGA, DMAR), 6 ACPI table reads, 8 RDMA
peer-named physical addresses, and 1 low-memory copy (the trampoline).

### 2.3 Every A/B/C site in the x64 production build

Macro-defined casts are counted once, at the macro: `smp.c:6-7` and
`timer.c:9-12`. Paths are relative to `src/kernel/`.

| class | file | lines | what | note |
|---|---|---|---|---|
| A | `arch/x64/gic.c` | 50 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC SVR 0xFEE000F0 |
| A | `arch/x64/mmu.c` | 107, 110, 113, 116, 119, 122, 125, 128, 131, 154, 482, 483, 484, 485, 486, 487, 488, 489, 676 | kernel-image object address used as its physical address | address of a kernel-image page table used as its physical address |
| A | `arch/x64/mmu.c` | 427, 431, 442, 448, 460, 464, 479, 480, 743, 746, 752, 758 | deref of a frame-pool physical address / frame-backed pointer | page-table frame deref (walker/alloc/teardown) |
| A | `arch/x64/mmu.c` | 592, 593 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC ICR |
| A | `arch/x64/smp.c` | 6, 7 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC ICR macros (one site per macro) |
| A | `arch/x64/smp.c` | 27 | low physical page written through the identity map | AP trampoline copied to phys 0x7000 (the copy only; the trampoline itself is design item D3) |
| A | `arch/x64/timer.c` | 9, 10, 11, 12 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC timer macros |
| A | `arch/x64/trap.c` | 1572, 1620 | kernel-image object address used as its physical address | kernel framebuffer (.bss) reported/mapped as phys |
| A | `arch/x64/trap.c` | 3473, 3477, 3483, 3489 | deref of a frame-pool physical address / frame-backed pointer | kpml_present CR3 walk |
| A | `arch/x64/trap.c` | 3755, 4736 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC EOI / ICR |
| A | `arch/x64/trap.c` | 5245, 5247, 5248 | deref of a frame-pool physical address / frame-backed pointer | [UDX] dump reads scratch/object frames |
| A | `blk_cache.c` | 197, 255, 671, 949 | deref of a frame-pool physical address / frame-backed pointer |  |
| A | `elf_loader.c` | 267, 275, 346, 347, 364, 373, 375, 431, 436, 648, 649, 735, 745, 786 | deref of a frame-pool physical address / frame-backed pointer | staging/image buffers are frame_alloc_contig runs |
| A | `elf_loader.c` | 808, 822, 831 | kernel-image VA passed where a physical-address integer is expected | kernel VA passed as the 'physical' dest of fat16_read_direct_pos (VA==PA) |
| A | `fat16.c` | 2567, 2592 | helper whose integer `dest` is a physical address | fat16_read_direct_locked: dest is a physical-address integer (program_loader.c:1152) |
| A | `fat16.c` | 2786 | helper whose integer `dest` is a physical address | fat16_read_direct_pos_raw dest |
| A | `fat16.c` | 2844, 2866, 2932 | deref of a frame-pool physical address / frame-backed pointer |  |
| A | `fat16.c` | 2849 | kernel-image VA passed where a physical-address integer is expected | caller buffer passed as phys integer |
| A | `file_obj.c` | 299, 304, 500, 519, 522 | deref of a frame-pool physical address / frame-backed pointer |  |
| A | `frame.c` | 219, 220 | kernel-image object address used as its physical address | kernel image VA bounds reserved as PHYSICAL range |
| A | `frame.c` | 249, 320 | deref of a frame-pool physical address / frame-backed pointer | RAM probe / zero-fill |
| A | `fs.c` | 3550, 3559 | deref of a frame-pool physical address / frame-backed pointer | NFS exec image staging run |
| A | `iommu_vtd.c` | 125, 137, 168, 187, 212, 230 | ACPI/BIOS table physical address dereferenced | ACPI RSDP/XSDT/RSDT/table phys deref (after mmu_map_mmio_range) |
| A | `iommu_vtd.c` | 511 | fixed sub-4-GiB MMIO physical address dereferenced | DMAR register page (phys from ACPI) |
| A | `iommu_vtd.c` | 569, 688, 816, 847, 864 | kernel-image object address used as its physical address | static VT-d tables programmed as phys |
| A | `iommu_vtd.c` | 809, 877, 956, 970, 983, 986 | kernel-image object address used as its physical address | VT-d table walk: phys -> pointer (tables are .bss) |
| A | `linux_compat_fd.c` | 650 | kernel-image object address used as its physical address | framebuffer phys for FBIOGET_FSCREENINFO |
| A | `net_rdma.c` | 344 | kernel-image object address used as its physical address | MR host_phys = .bss buffer |
| A | `net_rdma.c` | 365, 382, 435, 1541, 1559, 1584 | peer-supplied guest-physical address dereferenced | peer-named guest physical address dereferenced |
| A | `net_rdma.c` | 511, 522 | peer-supplied guest-physical address dereferenced | IOMMU shadow: iova==phys identity |
| A | `process.c` | 695, 730, 1305, 1306, 1595, 1597, 4225, 4227, 4228, 6625 | deref of a frame-pool physical address / frame-backed pointer | v1 user_phys_base block deref |
| A | `process.c` | 2777, 2778 | fixed sub-4-GiB MMIO physical address dereferenced | LAPIC ICR |
| A | `program_loader.c` | 382, 1639, 1643, 1647 | deref of a frame-pool physical address / frame-backed pointer | v1 block phys_base |
| A | `program_loader.c` | 1155 | kernel-image VA passed where a physical-address integer is expected | &stack local passed as phys (comment at :1152) |
| A | `program_loader.c` | 1200, 1250 | deref of a frame-pool physical address / frame-backed pointer | vfs_img run freed by address |
| A | `unix.c` | 46, 52 | deref of a frame-pool physical address / frame-backed pointer | AF_UNIX ring frames |
| A | `virtio_blk.c` | 346, 349, 405, 442, 479, 493, 1841, 1847, 2011, 2118 | kernel-image object address used as its physical address | NVMe queues / bounce in .bss programmed as phys |
| A | `virtio_gpu_3d.c` | 1073 | kernel-image object address used as its physical address | probe buffer (.bss) as phys |
| A | `virtio_gpu_x64.c` | 257 | fixed sub-4-GiB MMIO physical address dereferenced | BGA LFB phys (32-bit BAR) |
| A | `virtio_gpu_x64.c` | 492, 499, 506, 606, 746, 747, 748 | kernel-image object address used as its physical address | virtqueue / request / framebuffer pointers programmed as phys |
| A | `virtio_net.c` | 271, 279, 307, 314, 320, 395, 401 | kernel-image object address used as its physical address | virtqueue rings / buffers (.bss) programmed as phys |
| A | `vm.c` | 104, 105, 141, 144, 145, 198, 252, 283, 313, 349, 372, 503, 508, 1058, 1100, 1376, 1432, 1529, 1648, 2017, 2075, 2100, 2128, 2153, 2246, 2249, 2284, 2753, 2754, 3026, 3027, 3125, 3173, 3198, 4358, 4359, 4366, 4367, 4375, 4450, 4451 | deref of a frame-pool physical address / frame-backed pointer |  |
| A | `vm.c` | 3894 | kernel-image object address used as its physical address | framebuffer phys for vm_map_fb |
| B | `arch/x64/mmu.c` | 300, 313, 328, 341, 351, 361 | identity-VA MMIO mapper | mmu_map_mmio_range: identity-VA MMIO mapper, edits cpu_pml4/cpu_pdpt[0] |
| B | `net_rdma.c` | 1159, 1188 | >8 GiB BAR dereferenced at its identity VA | 64-bit PCI BAR / ROM deref at identity VA via mmu_map_mmio_range |
| B | `virtio_blk.c` | 1738 | BAR window installed inside the identity range | NVMe BAR window at VA 0x1FC000000 (cpu_pd7[480]) |
| B | `virtio_blk.c` | 1772 | BAR window installed inside the identity range | reads the identity alias of the window |
| B | `virtio_gpu_x64.c` | 426 | BAR window installed inside the identity range | virtio-gpu BAR window at VA 0x1FE000000 (cpu_pd7[496]) |
| C | `test_fw_cfg.c` | 11, 12 | ARM-only address in a TU compiled on both arches | ARM virt fw_cfg MMIO; meaningless on x64 (compiled, never called) |

Test-vehicle-only A sites (not in the default build): `arch/x64/mmu.c:605`
(LAPIC ICR, `PANIC_IPI_TEST`), `virtio_blk.c:1363`, `:1381` (bounce as PRP),
`virtio_blk.c:1797`, `:1803`, `:1805` (BAR alias probe through the identity
map).

### 2.4 Dependencies that are not casts (design items, all class B)

| # | where | dependency |
|---|---|---|
| D1 | `boot.s:31-72`, `:205-207` | `boot_pml4` identity 0–2 GiB is the BSP's table until `mmu_init` and every AP's table until `secondary_main`. 32-bit `lea` of image symbols requires the image below 4 GiB |
| D2 | `mmu.c:45-148` | `mmu_init_tables` builds the 0–8 GiB identity map that every root shares |
| D3 | `boot.s:161-260`, `smp.c:21-76` | AP trampoline at phys `0x7000`, entered with `boot_pml4` |
| D4 | `mmu.c:423-467`, `:433` | v2 walker understands only PML4[0] and PDPT[64..95] |
| D5 | `mmu.c:469-492` | v2 root = PML4[0] → PDPT whose [0..7] are the identity PDs |
| D6 | `mmu.c:553-555` | `vm_arch_resume_va_ok`: a kernel RIP is valid iff `< 0x200000000` ("the static identity map") |
| D7 | `mmu.c:743-771` | teardown frees PDPT[64..95] only |
| D8 | `frame.h:35-46`, `frame.c:43-95`, `:161-244` | pool extents hard-wired to identity-mapped RAM; no RAM map; MMIO-shadow reservation |
| D9 | `virtio_blk.c:1725-1734`, `virtio_gpu_x64.c:395-421` | BAR windows written into the shared identity PD7 |
| D10 | `mmu.c:191-253` | v1 overlay and v1 framebuffer live in the per-CPU identity PD1 |
| D11 | `arch/x64/trap.c:5643-5646` | kernel-mode `#PF` demand-fill only when `cr2 >= USER_VA_BASE` (a compat user page below 64 GiB would be a fatal kernel fault) |
| D12 | `elf_loader.c:486-492`, `elf_image.c:252` | ET_EXEC linked below `USER_VA_BASE` refused (`needs_low_window`). The compat AS must accept it (the Wine preloader is expected to be a fixed-address ET_EXEC, **verify**) |

### 2.5 Excluded sites (U and N), unit-test and ARM-only counts

U/N per file (x64 production):

| file | U | N |
|---|---|---|
| `arch/x64/locks.c` | 0 | 27 |
| `arch/x64/smp.c` | 0 | 1 |
| `arch/x64/trap.c` | 121 | 51 |
| `arch/x64/uart.c` | 0 | 1 |
| `fat16.c` | 0 | 1 |
| `fb_mmap.c` | 1 | 0 |
| `fpu_user.c` | 4 | 0 |
| `frame.c` | 0 | 1 |
| `fs.c` | 5 | 0 |
| `linux_compat_fd.c` | 3 | 0 |
| `linux_compat_misc.c` | 3 | 0 |
| `linux_compat_proc.c` | 2 | 0 |
| `linux_compat_thr.c` | 26 | 9 |
| `lockscale.c` | 0 | 11 |
| `pipe.c` | 0 | 1 |
| `process.c` | 15 | 11 |
| `program_loader.c` | 4 | 0 |
| `ssp.c` | 0 | 3 |
| `ssp_report.c` | 0 | 1 |
| `unix.c` | 0 | 4 |
| `virtio_blk.c` | 0 | 6 |
| `virtio_net.c` | 0 | 2 |
| `vm.c` | 1 | 3 |

Unit-test-only sites (`-DKERNEL_MODE_UNIT_TEST`):

| file | A | U | N |
|---|---|---|---|
| `arch/x64/fpu_sig_test.c` | 0 | 3 | 0 |
| `blk_cache.c` | 1 | 0 | 0 |
| `elf_loader_test.c` | 1 | 0 | 0 |
| `fat16.c` | 2 | 0 | 0 |
| `fb_mmap_test.c` | 0 | 0 | 1 |
| `frame_test.c` | 4 | 0 | 0 |
| `fs_test.c` | 0 | 0 | 1 |
| `linux_compat_thr.c` | 0 | 0 | 3 |
| `net_rdma_test.c` | 16 | 0 | 0 |
| `process_test.c` | 3 | 2 | 0 |
| `trap_test.c` | 2 | 8 | 0 |
| `unix_test.c` | 0 | 0 | 4 |
| `virtio_gpu_test.c` | 1 | 0 | 0 |
| `vm.c` | 1 | 0 | 0 |
| `vm_test.c` | 51 | 0 | 0 |

ARM-only sites (class C; ARM keeps its identity map):

| file | C sites |
|---|---|
| `arch/arm/trap.c` | 119 |
| `arch/arm/mmu.c` | 49 |
| `arch/arm/gic.c` | 13 |
| `virtio_gpu.c` | 7 |
| `virtio_net.c` | 7 |
| `virtio_blk.c` | 6 |
| `virtio_input.c` | 4 |
| `arch/arm/locks.c` | 3 |
| `arch/arm/uart.c` | 2 |
| `process.c` | 2 |
| `arch/arm/rtc.c` | 1 |
| `arch/arm/smp.c` | 1 |

### 2.6 Compat VA-layout dependencies (not identity, but compat blockers)

None of these dereference a physical address, but each assumes "user VA ⇔
`[USER_VA_BASE, USER_VA_TOP)`" or "kernel ⇔ `0x70000000..USER_VA_BASE`".
They are mechanical: replace them with per-AS bounds or a single
`vm_va_is_user(as, va)` predicate. They are listed so the compat step (S6) has
a complete checklist.

- **Functional user-range bounds (21 lines):** `process.c:2009`, `:6275`,
  `:6371`; `linux_compat_mem.c:342`, `:411`, `:513`, `:535`; `gpu_abi.c:361`,
  `:362`, `:672`; `vm.c:603`, `:909`, `:1851` (`vm_touch`, the funnel every
  U site uses), `:1993`, `:2051`, `:3979`, `:4189`, `:4253`, `:4331`;
  `fpu_user.c:63`; `trap.c:5646` (D11).
- **Kernel-location literal predicates (16 lines, all diagnostics except
  RDMA):** `trap.c:152`, `:2775`, `:2778`, `:3674`, `:3694`, `:3725`,
  `:3925`, `:4294`, `:5614`; `process.c:1315`, `:1830`, `:4252`, `:5731`;
  `net_rdma.c:364`, `:381`, `:434` (functional: they keep peer writes out of
  the image). All stay valid under K0. Under K1 every one would have to change.
  In passing: the bound `0x75000000` at `trap.c:3674` is already below
  today's measured `__stack_top` (`0x75c72b90`), so that deep-stack dump can
  no longer fire.
- **Native slot constants** (`USER_*_V2`, `USER_IMG_*`, `USER_LIB_*`,
  `ELF_USER_*`, `HB_LDSO_USER_*`): 94 uses, mostly `vm.c` (28),
  `program_loader.c` (26) and `elf_loader.c` (13). They stay native-only. The
  compat AS gets its own placement policy (F1, §5), so only the dispatch
  points that pick a window change (`vm_mmap` first-fit bounds,
  `vm.c:3496`, `:3978`).
- **Region capacity:** `VM_REGION_MAX_CAP 16384` per AS (`vm.c:66`; the
  "1024" in the comment at `vm.h:224` is stale). Whether Wine and the game
  fit is a P0 VA-map question (§7).

---

## 3. Design

### 3.1 Kernel text: stays (K0), or moves (K1)

| | K0: image stays at `0x70000000` (VA = PA) | K1: image moves to `0xffffffff80000000` |
|---|---|---|
| boot | unchanged (multiboot1 ELF32, `boot_pml4`) | new protocol (Limine native or multiboot2) or an a.out-kludge loader stub. ELF32 cannot hold the 64-bit VMAs (`Makefile:946-948`) |
| codegen | unchanged | `-mcmodel=kernel` everywhere |
| `kimg` A sites (62 + 5) | `kimg_pa(x) == (uint64_t)x`, mechanical and free | real subtraction, still mechanical |
| literal predicates (16, §2.6) | stay valid | all change |
| compat cost | a fixed supervisor-only hole `[0x70000000, 0x75E00000)` (94 MiB today) | none in the user half |
| collision risk | Wine wanting VA inside the hole (**verify**) | none |

**Decision: K0 for P1.** The collision analysis, using Wine's x86_64
preloader reservations as recalled (**verify at the pin**: `docs/cyberpunk.md`
§10 Q2): the DOS area at `0x10000–0x110000`, the "low memory" area
`0x110000–0x68000000`, shared user data at `0x7ff00000–0x7fff0000` and the
top-down area at `0x7ffffe000000+`. The measured image
`[0x70000000, 0x75c72b90)` intersects none of them. `KUSER_SHARED_DATA`
(`0x7ffe0000`) is above it. Windows x64 executables default to image bases
above 4 GiB (`0x140000000`), which lie in the 4–8 GiB identity range the
compat AS drops anyway (`docs/cyberpunk.md` §10 Q1).

**K1 revisit trigger:** promote K1 if the P0 VA map (`docs/cyberpunk.md`
§3.3 item 5) or the pinned preloader's link address puts *any* required Wine
or game mapping inside `[0x70000000, CP_KHOLE_HI)`, or if the image outgrows
the F1 ceiling (`0x78000000`).

### 3.2 Kernel-half layout (every root shares PML4[256..511])

| VA | PML4 | contents |
|---|---|---|
| `0xffff800000000000` – `+512 GiB` | 256 | **direct map (DMAP)**: `DMAP_BASE + pa` for every RAM byte, plus 0–4 GiB |
| `0xffff808000000000` – `+512 GiB` | 257 | **ioremap area**: BAR windows (NVMe, virtio-gpu, RDMA BARs, VT-d), `PCD|PWT`, bump-allocated, never re-pointed |
| `0xffffffff80000000` – top | 511 | reserved for K1 (empty in P1) |
| `[0x70000000, CP_KHOLE_HI)` | 0 (low alias) | kernel image, K0. In compat roots this is the *only* kernel mapping below the canonical hole |

- The PDPTs for PML4[256] and [257] are allocated once at boot, in `.bss`,
  and their PML4 entries are copied into `cpu_pml4[c]` and into every root at
  creation. Kernel-half PML4 entries never change after boot, so later DMAP or
  ioremap growth is visible in every root with no per-root bookkeeping. This
  is the same trick `vm_arch_root_alloc` already plays one level down with
  PDPT[0..7] (`mmu.c:482-489`).
- **DMAP 0–8 GiB reuses the existing PDs.** `dmap_pdpt[0..7]` point at
  `cpu_pd0, pd1_kernel, cpu_pd2..7`, so `phys_to_virt(pa)` for `pa < 8 GiB`
  has exactly the attributes the identity map has today, MTRR behaviour
  included. It costs no memory, and the PD7 windows stay reachable while
  they exist (until S4).
- **DMAP above 8 GiB**: 2 MiB WB leaves in new static PDs (one 4 KiB PD per
  GiB), mapping only the RAM ranges from the multiboot memory map. Holes stay
  unmapped, so a stray `phys_to_virt` into a hole faults. 2 MiB rather than
  1 GiB pages because `QEMU_CMD` passes no `-cpu` (`Makefile:332`), and
  QEMU's default x86 model (`qemu64`) does not advertise `pdpe1gb` (**verify**
  with `-cpu help`). TCG runs must keep working.
- Size: 512 GiB of DMAP VA. `DMAP_RAM_MAX` starts at 64 GiB of static PDs
  (56 PDs, 224 KiB of `.bss` over the 8 GiB already covered). That is
  well above any planned guest.

### 3.3 Helper API (`src/include/kmem.h`, new; both arches)

```c
/* x86_64 */
#define DMAP_BASE      0xffff800000000000ULL
#define IOREMAP_BASE   0xffff808000000000ULL
static inline void    *phys_to_virt(uint64_t pa);   /* DMAP_BASE + pa; S0-S1: (void *)pa */
static inline uint64_t virt_to_phys(const void *va);/* DMAP -> va - DMAP_BASE;
                                                       kernel image [_start, __stack_top) -> (uint64_t)va (K0);
                                                       anything else -> kmem_bad_va() tripwire */
static inline uint64_t kimg_pa(const void *sym);    /* K0: (uint64_t)sym; K1: sym - KIMG_VMA + KIMG_LMA */
volatile void *ioremap(uint64_t pa, uint64_t len);  /* IOREMAP area, PCD|PWT, 2 MiB granularity */
/* ARM (unchanged identity): phys_to_virt(pa) == (void *)pa, virt_to_phys(va) == (uint64_t)va,
   kimg_pa(sym) == (uint64_t)sym, ioremap(pa, len) == (void *)pa (device attrs already in L1[0]) */
```

- `virt_to_phys` takes only kernel pointers. A user VA is a bug, and its
  physical address comes from `vm_arch_walk` → `vm_arch_leaf_phys` as today
  (`vm.c:2016`, `:2074`).
- `phys_to_virt` is for RAM and sub-4 GiB legacy MMIO (LAPIC, BGA LFB), whose
  current attributes it inherits through the shared PDs. Anything above 4 GiB
  that is not RAM goes through `ioremap`.
- On ARM the header is the identity function, so the shared files converted
  in S1 compile to the same code (`make ARCH=arm` + ARM unit tests are the
  gate).

### 3.4 Root shapes after the change

```
kernel/v1 root  cpu_pml4[c]:  [0] -> cpu_pdpt[c] (identity 0-8 GiB as today, v1 overlay)
                              [256] DMAP   [257] IOREMAP              (S2)
native v2 root  PML4 frame:   [0] -> PDPT: [1] -> kimg_pd   (kernel image 2 MiB leaves only; S5)
                                           [64..95] -> user window (unchanged)
                              [256] DMAP   [257] IOREMAP
compat root     PML4 frame:   [0] -> PDPT: [1] -> per-AS pd1: entries for the kernel image
                                                  (copied from kimg_pd, US=0), rest user
                                           [0],[2..511] -> user (on demand)
                              [1..255] -> user (on demand)
                              [256] DMAP   [257] IOREMAP
```

- `kimg_pd` is a static PD holding only the 2 MiB leaves covering
  `[0x70000000, CP_KHOLE_HI)` (today indices 384..430 of the 1–2 GiB PD). In
  a compat root those leaves are *copied* into the per-AS PD for 1–2 GiB, so
  user page tables for `0x40000000–0x6fffffff` and `0x75E00000–0x7fffffff`
  (KUSER_SHARED_DATA) can sit next to them. The image leaves never change
  after boot, so a copy is safe.
- v1 and kernel tasks keep the identity on `cpu_pml4[c]`. They are native by
  definition, nothing compat runs on that root, and retiring v1 is a separate
  question (§7 Q5).

### 3.5 Compat vs native

- **Selection.** `PROC_FLAG_LINUX_ABI` is set by the loader
  (`program_loader.c:1334`), after the ELF probe (`elf_loader_probe_native`, `elf_loader.c:795-835`)
  and before the image is mapped. The root shape must be chosen there:
  `vm_as_set_layout(as, VM_LAYOUT_COMPAT)` before the first region insert.
  Fork inherits the layout (`vm_as_clone_into`, `vm.c:4382`). Exec picks the
  layout per image.
- **Bounds.** `as->va_lo`/`as->va_hi` replace `USER_VA_BASE`/`USER_VA_TOP` in
  every functional check (§2.6). For native they are exactly the old values,
  so native behaviour is bit-identical.
- **The hole** is a pre-inserted, permanent region of a new kind (`VMK_KHOLE`,
  no backing, never touchable) covering `[0x70000000, CP_KHOLE_HI)`. First-fit
  skips it naturally, `MAP_FIXED` and `MAP_FIXED_NOREPLACE` over it return
  `-ENOMEM`, and the region-based validation (`vm_touch`, `vm.c:1851-1859`)
  can never accept a user pointer into the kernel image. This is the security
  property: the kernel dereferences user pointers with SMAP off (§1.5), so the
  hole *must* be unvalidatable.
- **Walker/teardown/clone.** Generalise `x64_v2_walk_alloc` to four levels
  over any user PML4 index < 256, and to any PDPT index (D4). Teardown walks
  all user PML4 entries (D7). Native roots keep their single PML4[0] → PDPT,
  so the generalised walker is a superset of today's.
- **Placement (no hint)**: top-down like Linux (F1). Native keeps its
  bottom-up first-fit windows.

### 3.6 Syscall / interrupt entry while a compat AS is active

Nothing on the entry path switches CR3 (§1.5), so the compat root has to
carry everything the entry path touches before C code runs: the image
(LSTAR/IDT text, TSS rsp0 stacks, GDT, `cpu_locals` through `gs:`) via the
K0 alias, and the LAPIC. LAPIC EOI is written at `trap.c:3755` *in the
interrupt path*, so S1 must convert it to `phys_to_virt(0xFEE000B0)`, which
lands in DMAP. Converting it is mandatory, not cleanup: an unconverted EOI
in a compat root faults inside the interrupt handler. After entry, every
kernel access to RAM is through DMAP, and every user access is the same
direct dereference of a validated user VA as today.

GS caveat for the same lane's `ARCH_SET_GS` task: with no `swapgs`, the entry
code relies on `GS_BASE == &cpu_locals[cpu]` *while user code runs*
(`trap.c:5941`, `:6058`). A per-thread user GS base therefore requires
`swapgs` on every entry and exit path first. That work is outside this note,
but it is not optional.

### 3.7 TLB / PCID consequences

- Still no PCID: compat roots cost exactly what native v2 roots cost, a full
  flush per switch. Kernel-half translations (DMAP) are refilled after each
  switch. Marking them global (CR4.PGE) is a later optimisation, and **only**
  after the local-CR3-reload kernel flushes (`mmu.c:205-213`, `:242-250`,
  `:370-378`, `virtio_gpu_x64.c:410-417`) become `invlpg` or a CR4.PGE toggle,
  because a CR3 reload does not flush global entries.
- Kernel-half updates after boot are *adds only* (DMAP is static, and the
  ioremap area is bump-allocated and never re-pointed). Changing a
  paging-structure entry from not-present to present needs no TLB
  invalidation, so no broadcast is needed. The NVMe and virtio-gpu windows
  re-point an existing PD7 entry today (§1.6); moving them to ioremap (S4)
  removes that hazard.
- User shootdown (`vm_arch_shootdown`) is unchanged. It reloads the target
  root, which is correct for 4-level compat roots too.
- F1 excludes the last page below 2^47 (`0x7ffffffff000`), as Linux's
  `TASK_SIZE` does, so a `syscall` from the top page can never return to a
  non-canonical RIP.

### 3.8 Tripwires for leftover identity dereferences

The plan asks for a tripwire that "faults any remaining identity dereference
while a compat AS is active" (`docs/cyberpunk.md` §6 P1). That cannot be
guaranteed *inside* a compat root: once user code maps `0x10000..`, a
leftover `(T *)pa` for a small `pa` reads or writes a **user page** and does
not fault. Instead:

1. **Native tripwire (S5).** Native v2 roots drop PDPT[0], [2..7] and keep
   only `kimg_pd` at [1] (§3.4). The user window is at 64 GiB, so nothing
   user-mapped is below 8 GiB, and any leftover identity dereference made
   while a native process is current is a kernel-mode `#PF` with `cr2 < 8 GiB`
   outside the image. The kernel `#PF` path tags it `[IDMAP-TRIP] cr2= rip=`
   and halts. Doom, RimWorld and the desktop all run on native v2 roots, so
   the existing regressions become the tripwire's soak.
2. **Pool-above-8 GiB tripwire (S3, after S5).** Frames above 8 GiB have *no*
   identity alias in any root, so an unconverted dereference of such a frame
   faults on the kernel root too, kernel threads included.
3. **Static gate.** The Appendix A scan becomes a check that fails on any
   int↔pointer cast outside an allowlist (the helpers, the U/N sites, ARM
   code). New code cannot reintroduce an identity dereference unnoticed.
4. **Compat guard floor (S6).** The compat layout first ships with
   `va_lo = 8 GiB`: compat shape, nothing user-mappable where the identity
   used to be. Only after RimWorld and Doom pass on it does `va_lo` drop to
   `0x10000` (S8).

### 3.9 RAM above 8 GiB

`_start` saves EBX (the multiboot info pointer) in a `.data` word. The
kernel parses the multiboot1 memory map (Limine provides one; **verify**
against its multiboot1 implementation) and adds pool extents for RAM above
8 GiB, mapped in DMAP. The pool bitmap is static (`frame.c:52-55`):
64 GiB of pool costs 2 MiB of `.bss`, which must stay under the F1 image
ceiling. The PCI-hole tripwire (`frame.c:297-298`) and the RAM probe
(`frame.c:170-210`) extend to the new extents unchanged.

---

## 4. Migration order

Each step is one commit series that can be reverted on its own, gated by
tests that exist today. "Full x64 tier" means `make host_tests`,
`./run_unit_tests_intel.sh`, `make test_intel`, `make doom_elf_test` and
`make desktop_test_intel` (targets at `Makefile:4345`, `:4456`, `:4462`,
`:4471`, `:4601`), plus RimWorld to its menu (the plan's P1 gate).

| step | change | behaviour change | gate |
|---|---|---|---|
| **S0** | add `kmem.h` with identity implementations on both arches; add the castscan check (report-only) | none | `make` both arches; `make unit_tests_arm`; full x64 tier |
| **S1** | convert all 208 + 6 + 82 class-A sites (production, test vehicles, unit tests) to the helpers; helpers still identity | none (same object code modulo inlining) | full x64 tier; ARM `make test_arm`; castscan: zero A sites left |
| **S2** | build DMAP (PML4[256], PDPT[0..7] → existing PDs) in `cpu_pml4[c]` and in every v2 root; switch x64 `phys_to_virt` to `DMAP_BASE + pa`; `virt_to_phys` with the image/DMAP split and the tripwire | kernel now reaches RAM through DMAP; identity still present | full x64 tier; `frame_test.c` / `vm_test.c` / `mmu_test.c` in the unit tier; `./run_two_instances.sh` (RDMA) |
| **S3a** | save EBX; parse the multiboot memory map; log it next to `[FRAME]` (`frame.c:227-243`) | log only | boot log shows the map; full x64 tier |
| **S4** | ioremap area (PML4[257]); move the NVMe and virtio-gpu windows off `cpu_pd7`; reimplement `mmu_map_mmio_range` as `ioremap` (fixes both defects in §1.3); drop the MMIO-shadow reservation (`frame.h:45-46`) | MMIO VAs move | full x64 tier (NVMe boot, desktop on virtio-gpu); `./run_two_instances.sh`; Proxmox GPU path per `skills/hobbyos-proxmox-gpu` if available |
| **S5** | native v2 roots drop the identity PDs except `kimg_pd`; `[IDMAP-TRIP]` tag in the kernel `#PF` path; `vm_arch_resume_va_ok` checks the image range instead of `< 8 GiB` (D6) | leftover identity dereferences now fault | full x64 tier **and** RimWorld to menu, Doom ELF, icewm (`make icewm_test`), each with zero `[IDMAP-TRIP]` |
| **S3b** | pool extents above 8 GiB from the memory map, mapped in DMAP | more RAM | boot at `-m 12288M` and 24 GiB: `[FRAME]` totals grow; `test_frame_pool_is_ram_only` (`frame_test.c:296`); full x64 tier |
| **S6** | compat layout: `VM_LAYOUT_COMPAT`, 4-level walker/teardown, per-AS bounds (§2.6), `VMK_KHOLE`, top-down placement, ET_EXEC acceptance (D12), D11 gate via `vm_va_is_user`; **guard floor `va_lo = 8 GiB`**; behind a build flag, off by default | none unless enabled | flag off: full x64 tier. Flag on: Doom ELF and RimWorld to menu with every Linux-ABI process in a compat root |
| **S7** | flag on by default for Linux-ABI processes | compat roots in production | full x64 tier + RimWorld + Doom |
| **S8** | `va_lo = 0x10000`, the F1 final layout | low VA available | `linux_probe_lowva.c` (P1 plan): maps `0x10000` and `0x7ffe0000`, `MAP_FIXED_NOREPLACE` at both, a 1 TiB `PROT_NONE` reservation without frames, a page just below `0x7ffffffff000`; `MAP_FIXED` into the hole returns `-ENOMEM`; then full x64 tier + RimWorld |

S3 is split. S3a (reading the map) is harmless early. S3b (using RAM
above 8 GiB) waits for S5, because it is the step that turns every leftover
into a crash on *every* root.

---

## 5. F1 draft — compat address space, x86_64 (refines `docs/cyberpunk.md` App. A)

```
F1 v1 (draft; C1 owns; amendment protocol per docs/cyberpunk.md §7.3)

applies to      processes loaded with PROC_FLAG_LINUX_ABI once S7 lands;
                native processes keep the v2 layout of src/include/vm.h:15-29 unchanged
lowest user VA  CP_VA_LO  = 0x0000000000010000
user ceiling    CP_VA_HI  = 0x00007ffffffff000   (exclusive; last page below 2^47 never mappable)
kernel hole     [0x70000000, CP_KHOLE_HI)        CP_KHOLE_HI = round_up_2MiB(__stack_top + 64 KiB)
                                                 = 0x75E00000 on 2026-11-14; linker ASSERT CP_KHOLE_HI <= 0x78000000
                hole semantics: never mappable by user; MAP_FIXED / MAP_FIXED_NOREPLACE -> -ENOMEM;
                hints inside it are ignored; not listed in /proc/<pid>/maps (open question)
guaranteed mappable fixed addresses (the kernel only allows; Wine maps):
                0x7ffe0000 (KUSER_SHARED_DATA), everything in [CP_VA_LO, 0x70000000),
                [CP_KHOLE_HI, CP_VA_HI)
placement       no-hint mmap: top-down from CP_MMAP_TOP = 0x7ffff7fff000 (Linux non-ASLR mmap_base shape)
                main ET_DYN:  CP_ET_DYN_BASE = 0x555555554000 (Linux ELF_ET_DYN_BASE, no ASLR)
                main ET_EXEC: at its link address (must lie in user range, outside the hole)
                brk: from the page after the main image's highest PT_LOAD
                main stack: top = CP_VA_HI, 8 MiB demand reserve + 64 KiB guard (as USER_MAIN_STK_*)
reservations    PROT_NONE / MAP_NORESERVE ranges cost a region record, no frames
                (VM_REGION_MAX_CAP = 16384 per AS, vm.c:66)
not provided    vsyscall page, vDSO (none today); no ASLR
kernel half     DMAP_BASE 0xffff800000000000 (PML4[256], 512 GiB); IOREMAP_BASE 0xffff808000000000
                (PML4[257]); 0xffffffff80000000 reserved (K1). Identical in every root.
TLB             no PCID; CR3 switch = full flush; user shootdown unchanged (vector 0x82)
```

Changes from the plan's draft: the floor stays `0x10000`. The ceiling becomes
`0x7ffffffff000`, not `0x7fffffffffff`, which keeps the sysret guard page.
The kernel-image hole is named and bounded. The direct-map base is chosen.
Placement policy is added, because Wine's preloader assumes that Linux puts
unhinted maps high (**verify**).

---

## 6. AD-3 fallback check

Trigger (`docs/cyberpunk.md` AD-3): ">50 identity-dependent call sites that
cannot be mechanically converted".

| count | value |
|---|---|
| identity-dependent cast sites, x64 production | 219 (208 A + 11 B), plus 2 C |
| of which not mechanically convertible (B) | **11**: `mmu.c:300-361` (6, one function), `net_rdma.c:1159`, `:1188`, `virtio_blk.c:1738`, `:1772`, `virtio_gpu_x64.c:426` |
| non-cast design items (D1–D12) | **12** |
| **total non-mechanical** | **23** |

**The fallback does not fire.** The 11 B sites collapse into 3 mechanisms
(one MMIO mapper, two windows); all of them are solved by the ioremap area
(S4). Adding the 21 compat bound checks of §2.6 (mechanical, and not identity
dependencies) still gives 44 < 50.

---

## 7. Effort, risks, open questions

**Effort** (relative, using the plan's scale, S < M < L < XL): S0 S; S1 M
(≈300 one-line rewrites across 29 files, no logic); S2 M; S3a S; S3b M;
S4 M; S5 S; S6 L (walker/teardown, bounds, placement policy, ET_EXEC, hole);
S7 S; S8 S. **Overall L** for the address-space half of P1. That is
consistent with the plan's XL for P1 as a whole, which also contains XSAVE
and GS (and GS now carries the `swapgs` prerequisite of §3.6).

**Risks**

| # | risk | mitigation |
|---|---|---|
| AS-1 | Wine or the game needs VA inside the kernel hole (preloader link address, fixed allocations) | P0 VA map + preloader pin answer it before S6; K1 trigger (§3.1) |
| AS-2 | a leftover identity dereference in a compat root silently hits a user page | S5 native tripwire, S3b pool > 8 GiB, castscan gate, compat guard floor before S8 (§3.8) |
| AS-3 | interrupt-path MMIO (LAPIC EOI `trap.c:3755`) missed → fault inside an IRQ handler | it is in the A list; S5 catches it on native roots first |
| AS-4 | `.bss` growth (a larger pool bitmap) pushes `__stack_top` past the F1 ceiling | linker ASSERT; move big tables to frames; K1 trigger |
| AS-5 | ownership: S1 touches files other lanes own (`virtio_gpu_3d.c:1073`, C6; `gpu_abi.c:361-362`, `:672`, C6; `linux_compat_fd.c:650`, `linux_compat_mem.c` bounds, C2; `fs.c:3550-3559`, C3) | C1 submits those hunks as diffs to the owning lane / integrator (one-writer rule, §7.1 of the plan) |
| AS-6 | Wine/game VMA count exceeds 16,384 regions per AS | P0 VA map records the count |
| AS-7 | TCG (Mac) x64 runs break on 1 GiB pages | design uses 2 MiB pages only (§3.2) |
| AS-8 | the RDMA path relies on peer-named physical addresses (`net_rdma.c:365`, `:1541`); DMAP must cover whatever a peer names | DMAP maps all RAM; a non-RAM address now faults instead of scribbling (an improvement); `./run_two_instances.sh` gates S2/S4 |

**Open questions**

1. (extends plan §10 Q2) Which ranges does the pinned x86_64
   `wine-preloader` reserve, at what address is it linked, and does any of it
   intersect `[0x70000000, 0x78000000)`? The ranges in §3.1 are recalled, not
   read.
2. Does Wine tolerate a hole in the low 2 GiB? An unhinted or hinted `mmap`
   that cannot be satisfied there, and `MAP_FIXED_NOREPLACE` returning
   `-ENOMEM`/`-EEXIST`. Expected yes, since Wine probes; P0's host-native
   `/proc/<pid>/maps` shows whether anything lands there on Linux.
3. Should every Linux-ABI process (RimWorld, Doom, the X server's compat
   clients) move to the compat layout in S7, or only Wine-launched ones? This
   note assumes all of them, because that is what makes RimWorld the compat
   layout's soak test.
4. Should the kernel hole appear in `/proc/<pid>/maps` (`fs.c:496` builds
   that file)? Wine may parse it (**verify**).
5. Is v1 still needed on x64 (kernel tasks, the flat loader at
   `program_loader.c:242`)? If kernel tasks moved to a v2-shaped kernel root,
   `cpu_pml4[c]` could drop the identity map too, and the tripwire would
   cover kernel threads.
6. Does Limine's multiboot1 path pass a usable memory map in EBX on this
   OVMF build? If not, read the e820 table through `fw_cfg` instead.
7. (Noticed, outside AS scope.) SFMASK clears only IF (`trap.c:5991`), so a
   user DF/TF/AC survives into the kernel on `syscall`. Linux masks
   DF|TF|AC|IF|NT. Windows code entering Wine with DF set is unlikely, but the
   kernel's `rep movs` would run backwards.

---

## 8. Proposed amendments to `docs/cyberpunk.md` (for C0; not applied)

See the lane report for exact text. In summary: correct the §3.2 GS row (no
`swapgs`); add §3.2 rows for the kernel image extent and the 8 GiB pool cap;
note in §4 and R4 that the 16 GiB guest depends on P1's direct map; record
K0 in AD-3, plus the fallback result and the K1 trigger; replace App. A F1
with §5; move P1's tripwire wording to native roots; extend §10 Q2 with the
hole question.

---

## Appendix A — the scanner and classifier (reproduce §2.2)

### A.1 Scanner

Run from the repo root on a box with clang 21 (linuxci). The scanner reads
clang's text AST dump, which prints locations relative to the previously
printed one, so it tracks the running file and line. Type names that embed
`file:line:col` ("unnamed struct at …") are stripped first, because the
dumper does not treat them as locations.

```python
#!/usr/bin/env python3
"""Stream a clang -ast-dump (text) and print every IntegralToPointer /
PointerToIntegral cast whose location is in the given source prefix.

clang's text dump prints locations relative to the previous printed one:
  <path:L:C ...>   file + line + col
  <line:L:C ...>   same file, new line
  <col:C ...>      same file and line
We track the running (file,line) exactly as the dumper does, using the
begin location of each node's range.  Macro-expanded nodes print the
spelling location of the macro body; those are reported at that location.

usage: clang ... -Xclang -ast-dump -fno-color-diagnostics | castscan.py PREFIX TU
"""
import re
import sys

prefix = sys.argv[1]
tu = sys.argv[2]
LOC = re.compile(r'<(?:([^<>:, ]+):(\d+):(\d+)|line:(\d+):(\d+)|col:(\d+)|(<invalid sloc>)|(<scratch space>):(\d+):(\d+)|(<built-in>):(\d+):(\d+))')
# every location token in a line updates the running position (clang
# updates "last location" for both begin and end, and for the extra
# location printed after the range on decls).
ANYLOC = re.compile(r'(?<![\w>])(?:([/\w.+-]+\.[ch]|<scratch space>|<built-in>):(\d+):(\d+)|line:(\d+):(\d+)|col:(\d+))')
cur_file, cur_line = None, None
for raw in sys.stdin:
    kind = None
    if 'CStyleCastExpr' in raw or 'ImplicitCastExpr' in raw:
        if '<IntegralToPointer>' in raw:
            kind = 'I2P'
        elif '<PointerToIntegral>' in raw:
            kind = 'P2I'
    first = None
    # Type names embed non-location file positions ("unnamed struct at
    # src/include/fs.h:116:5"); the dumper does not update its last-location
    # state for those, so drop quoted types and "(... at f:l:c)" first.
    scan = re.sub(r"'[^']*'", "''", raw)
    scan = re.sub(r"\((?:anonymous|unnamed)[^()]* at [^()]*\)", "()", scan)
    for m in ANYLOC.finditer(scan):
        if m.group(1):
            cur_file, cur_line = m.group(1), int(m.group(2))
        elif m.group(4):
            cur_line = int(m.group(4))
        if first is None:
            first = (cur_file, cur_line)
    if kind and first and first[0] and first[0].startswith(prefix):
        expl = 'C' if 'CStyleCastExpr' in raw else 'implicit'
        ty = re.search(r"'([^']*)'", raw)
        print(f"{first[0]}:{first[1]}\t{kind}\t{expl}\t{ty.group(1) if ty else ''}\t{tu}")
```

```bash
#!/bin/bash
# Run castscan over every kernel TU for one arch.  Usage: runscan.sh intel|arm [extra -D...]
set -u
cd "$(git rev-parse --show-toplevel)"   # castscan.py and this script live in obj-scan/ (scratch, not committed)
ARCH=$1; shift
EXTRA="$*"
if [ "$ARCH" = intel ]; then
  CF="-O2 -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -fstack-protector-strong -mstack-protector-guard=global"
  AD=src/kernel/arch/x64
else
  CF="-O2 -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -fstack-protector-strong -mstack-protector-guard=global"
  AD=src/kernel/arch/arm
fi
ls src/kernel/*.c $AD/*.c | xargs -P 48 -I{} sh -c \
  "clang $CF $EXTRA -fsyntax-only -w -Xclang -ast-dump -fno-color-diagnostics {} 2>/dev/null | python3 obj-scan/castscan.py src/ {}"
# x64 default / + unit tests / arm:
bash obj-scan/runscan.sh intel                          > x64.raw
bash obj-scan/runscan.sh intel -DKERNEL_MODE_UNIT_TEST  > x64ut.raw
bash obj-scan/runscan.sh arm                            > arm.raw
for s in x64 x64ut arm; do cut -f1,2 $s.raw | sort -u > $s.u; done
wc -l x64.u                        # 539
comm -13 x64.u x64ut.u | wc -l     # 104 unit-test-only
comm -13 x64ut.u arm.u | wc -l     # 214 ARM-only
```

### A.2 Classifier

Run as `python3 classify.py x64.lst x64.class`, where `x64.lst` is `x64.u`
with the source line appended as a third tab-separated column. It prints
the per-class totals of §2.2 and refuses to finish silently if a site has
no verdict.

```python
#!/usr/bin/env python3
"""Classify every x64 int<->pointer cast site from castscan (x64.lst).

Classes (docs/cp-address-space.md section 2):
  A  mechanical: wrap in phys_to_virt()/virt_to_phys()/kimg_pa()
  B  needs design
  U  not a dependency: a USER virtual address (current AS)
  N  not a dependency: a kernel VA used as a VA, a lock/diag value, etc.
Category tags after the class letter say why.
"""
import re
import sys
from collections import Counter, defaultdict

rows = []
for l in open(sys.argv[1]):
    parts = l.rstrip("\n").split("\t")
    loc, kind = parts[0], parts[1]
    text = parts[-1]
    f, n = loc.rsplit(":", 1)
    rows.append((f, int(n), kind, text))

R = {}  # (file, line) -> (cls, cat, note)


def put(f, lines, cls, cat, note=""):
    for n in lines:
        R[(f, n)] = (cls, cat, note)


K = "src/kernel/"
X = K + "arch/x64/"

# ---- arch/x64 ------------------------------------------------------------
put(X + "gic.c", [50], "A", "mmio-low", "LAPIC SVR 0xFEE000F0")
put(X + "smp.c", [6, 7], "A", "mmio-low", "LAPIC ICR macros (one site per macro)")
put(X + "smp.c", [27], "A", "lowmem", "AP trampoline copied to phys 0x7000 (the copy only; the trampoline itself is design item D3)")
put(X + "smp.c", [40], "N", "kva", "AP stack = kernel-image VA")
put(X + "timer.c", [9, 10, 11, 12], "A", "mmio-low", "LAPIC timer macros")
put(X + "uart.c", [47], "N", "diag")
for n in [235] + list(range(289, 600)):
    R.setdefault((X + "locks.c", n), ("N", "diag", "lock address / return PC"))
put(X + "mmu.c", [107, 110, 113, 116, 119, 122, 125, 128, 131, 154, 482, 483,
                  484, 485, 486, 487, 488, 489, 676], "A", "kimg",
    "address of a kernel-image page table used as its physical address")
put(X + "mmu.c", [300, 313, 328, 341, 351, 361], "B", "mmio-map",
    "mmu_map_mmio_range: identity-VA MMIO mapper, edits cpu_pml4/cpu_pdpt[0]")
put(X + "mmu.c", [427, 431, 442, 448, 460, 464, 479, 480, 743, 746, 752, 758],
    "A", "frame", "page-table frame deref (walker/alloc/teardown)")
put(X + "mmu.c", [592, 593], "A", "mmio-low", "LAPIC ICR")
put(X + "trap.c", [3473, 3477, 3483, 3489], "A", "frame", "kpml_present CR3 walk")
put(X + "trap.c", [3755, 4736], "A", "mmio-low", "LAPIC EOI / ICR")
put(X + "trap.c", [1572, 1620], "A", "kimg", "kernel framebuffer (.bss) reported/mapped as phys")
put(X + "trap.c", [5245, 5247, 5248], "A", "frame", "[UDX] dump reads scratch/object frames")
put(X + "trap.c", [2777, 3461, 3537, 3676, 3693, 3724, 3741, 4466, 4501, 4513,
                   4518, 4558, 4576, 4578, 5402, 5403, 5450, 5452, 5466, 5476,
                   5480, 5486, 5499, 5547, 5619, 6286], "N", "diag")
put(X + "trap.c", [5879, 5884, 5919, 5935, 5946, 5985] + list(range(6464, 6497)),
    "N", "kva", "TSS/GS/IDT/LSTAR take linear (virtual) addresses")

# ---- shared kernel -------------------------------------------------------
put(K + "blk_cache.c", [197, 255, 671, 949], "A", "frame")
put(K + "elf_loader.c", [267, 275, 346, 347, 364, 373, 375, 431, 436, 648, 649,
                         735, 745, 786], "A", "frame",
    "staging/image buffers are frame_alloc_contig runs")
put(K + "elf_loader.c", [808, 822, 831], "A", "kimg-int",
    "kernel VA passed as the 'physical' dest of fat16_read_direct_pos (VA==PA)")
put(K + "fat16.c", [2567, 2592], "A", "phys-int",
    "fat16_read_direct_locked: dest is a physical-address integer (program_loader.c:1152)")
put(K + "fat16.c", [2786], "A", "phys-int", "fat16_read_direct_pos_raw dest")
put(K + "fat16.c", [2844, 2866, 2932], "A", "frame")
put(K + "fat16.c", [2849], "A", "kimg-int", "caller buffer passed as phys integer")
put(K + "fat16.c", [2958], "N", "diag", "id->buf records the VA")
put(K + "fb_mmap.c", [32], "U", "user")
put(K + "file_obj.c", [299, 304, 500, 519, 522], "A", "frame")
put(K + "fpu_user.c", [86, 97, 111, 119], "U", "user")
put(K + "frame.c", [219, 220], "A", "kimg", "kernel image VA bounds reserved as PHYSICAL range")
put(K + "frame.c", [249, 320], "A", "frame", "RAM probe / zero-fill")
put(K + "frame.c", [343], "N", "diag")
put(K + "fs.c", [1270, 1308, 3162, 3700, 3708], "U", "user")
put(K + "fs.c", [3550, 3559], "A", "frame", "NFS exec image staging run")
put(K + "iommu_vtd.c", [125, 137, 168, 187, 212, 230], "A", "acpi",
    "ACPI RSDP/XSDT/RSDT/table phys deref (after mmu_map_mmio_range)")
put(K + "iommu_vtd.c", [511], "A", "mmio-low", "DMAR register page (phys from ACPI)")
put(K + "iommu_vtd.c", [569, 688, 816, 847, 864], "A", "kimg",
    "static VT-d tables programmed as phys")
put(K + "iommu_vtd.c", [809, 877, 956, 970, 983, 986], "A", "kimg",
    "VT-d table walk: phys -> pointer (tables are .bss)")
put(K + "linux_compat_fd.c", [364, 393, 534], "U", "user")
put(K + "linux_compat_fd.c", [650], "A", "kimg", "framebuffer phys for FBIOGET_FSCREENINFO")
put(K + "linux_compat_misc.c", [88, 120, 143], "U", "user")
put(K + "linux_compat_proc.c", [662, 671], "U", "user")
put(K + "linux_compat_thr.c", [424, 531, 1235, 1273, 1699, 1975, 2046, 2340, 3905],
    "N", "diag", "AS pointer as an identity token")
for n in [1518, 1585, 1661, 1717, 1740, 1831, 1929, 2168, 2928, 2957, 2966, 2967,
          2970, 3108, 3109, 3112, 3113, 3121, 3137, 3139, 3186, 3515, 3566, 3574,
          3578, 3581]:
    R[(K + "linux_compat_thr.c", n)] = ("U", "user", "futex word / signal frame")
for n in range(1, 800):
    R.setdefault((K + "lockscale.c", n), ("N", "diag"))
put(K + "net_rdma.c", [344], "A", "kimg", "MR host_phys = .bss buffer")
put(K + "net_rdma.c", [365, 382, 435, 1541, 1559, 1584], "A", "rdma",
    "peer-named guest physical address dereferenced")
put(K + "net_rdma.c", [511, 522], "A", "rdma", "IOMMU shadow: iova==phys identity")
put(K + "net_rdma.c", [1159, 1188], "B", "mmio-high",
    "64-bit PCI BAR / ROM deref at identity VA via mmu_map_mmio_range")
put(K + "pipe.c", [277], "N", "diag")
put(K + "process.c", [695, 730, 1305, 1306, 1595, 1597, 4225, 4227, 4228, 6625],
    "A", "frame", "v1 user_phys_base block deref")
put(K + "process.c", [1054, 1673, 5623, 6650, 6697], "N", "diag")
put(K + "process.c", [1382, 1400], "N", "kva", "kernel-thread entry/arg")
put(K + "process.c", [1950, 4744, 6731, 6732], "N", "kva", "kernel stack VA")
put(K + "process.c", [2777, 2778], "A", "mmio-low", "LAPIC ICR")
put(K + "process.c", [2980, 2998, 3003, 3613, 3646, 3673, 3889, 3973, 3995, 4022,
                      4158, 6084, 6417, 6419, 6618], "U", "user")
put(K + "program_loader.c", [120, 130], "U", "user-v1", "v1 overlay VA USER_VIRT_BASE")
put(K + "program_loader.c", [382, 1639, 1643, 1647], "A", "frame", "v1 block phys_base")
put(K + "program_loader.c", [1155], "A", "kimg-int", "&stack local passed as phys (comment at :1152)")
put(K + "program_loader.c", [1200, 1250], "A", "frame", "vfs_img run freed by address")
put(K + "program_loader.c", [1751, 1782], "U", "user")
put(K + "ssp.c", [43, 69, 76], "N", "kva")
put(K + "ssp_report.c", [62], "N", "kva")
put(K + "test_fw_cfg.c", [11, 12], "C", "arm-addr",
    "ARM virt fw_cfg MMIO; meaningless on x64 (compiled, never called)")
put(K + "unix.c", [46, 52], "A", "frame", "AF_UNIX ring frames")
put(K + "unix.c", [363, 383, 688, 701], "N", "kva", "iovec base: VA carried in an integer")
put(K + "virtio_blk.c", [346, 349, 405, 442, 479, 493, 1841, 1847, 2011, 2118],
    "A", "kimg", "NVMe queues / bounce in .bss programmed as phys")
put(K + "virtio_blk.c", [589, 591, 1743, 1921, 1923, 2104], "N", "diag")
put(K + "virtio_blk.c", [1738], "B", "mmio-window", "NVMe BAR window at VA 0x1FC000000 (cpu_pd7[480])")
put(K + "virtio_blk.c", [1772], "B", "mmio-window", "reads the identity alias of the window")
put(K + "virtio_gpu_3d.c", [1073], "A", "kimg", "probe buffer (.bss) as phys")
put(K + "virtio_gpu_x64.c", [257], "A", "mmio-low", "BGA LFB phys (32-bit BAR)")
put(K + "virtio_gpu_x64.c", [426], "B", "mmio-window", "virtio-gpu BAR window at VA 0x1FE000000 (cpu_pd7[496])")
put(K + "virtio_gpu_x64.c", [492, 499, 506, 606, 746, 747, 748], "A", "kimg",
    "virtqueue / request / framebuffer pointers programmed as phys")
put(K + "virtio_net.c", [271, 279, 307, 314, 320, 395, 401], "A", "kimg",
    "virtqueue rings / buffers (.bss) programmed as phys")
put(K + "virtio_net.c", [303, 305], "N", "diag")
put(K + "vm.c", [104, 105, 141, 144, 145, 198, 252, 283, 313, 349, 372, 503, 508,
                 1058, 1100, 1376, 1432, 1529, 1648, 2017, 2075, 2100, 2128, 2153,
                 2246, 2249, 2284, 2753, 2754, 3026, 3027, 3125, 3173, 3198, 4358,
                 4359, 4366, 4367, 4375, 4450, 4451], "A", "frame")
put(K + "vm.c", [1988], "U", "user-v1", "v1 window read")
put(K + "vm.c", [2846, 2963, 3723], "N", "diag")
put(K + "vm.c", [3894], "A", "kimg", "framebuffer phys for vm_map_fb")

# trap.c defaults: syscall-argument casts and their range checks are user VAs
out = []
missing = []
for f, n, kind, text in rows:
    c = R.get((f, n))
    if c is None and f == X + "trap.c" and (
            "tf->regs" in text or "sys_user_range" in text or "process_user_ok" in text
            or "vm_user_read" in text or "u_strcpy" in text):
        c = ("U", "user", "syscall argument")
    if c is None and f == X + "trap.c" and n in (194, 774, 807, 1691, 1166, 1210, 1438, 1545, 1825, 1989,
                                                 2022, 2154, 2155, 2172, 2303, 2970,
                                                 2971, 5219, 5220, 5327):
        c = ("U", "user")
    if c is None:
        missing.append((f, n, kind, text))
        continue
    out.append((f, n, kind, c[0], c[1], c[2] if len(c) > 2 else "", text))

for m in missing:
    print("UNCLASSIFIED", *m, file=sys.stderr)
cnt = Counter(o[3] for o in out)
print("total", len(out), "unclassified", len(missing), dict(cnt), file=sys.stderr)
cat = Counter((o[3], o[4]) for o in out)
for k in sorted(cat):
    print("  ", k, cat[k], file=sys.stderr)
with open(sys.argv[2], "w") as o:
    for r in sorted(out):
        o.write("\t".join([f"{r[0]}:{r[1]}", r[2], r[3], r[4], r[5], r[6]]) + "\n")
```

### A.3 Supporting greps (§2.4, §2.6)

```bash
grep -rnE 'USER_VA_BASE|USER_VA_TOP' src/kernel src/include          # bound checks
grep -rnE '0x0*200000000|0x1[Ff][CcEe]000000|0x0*7[0-5]0{6}(ULL)?\b|0x0*80000000ULL|0x0*100000000ULL' \
     src/kernel src/include                                           # layout literals
grep -rn 'swapgs' src/kernel/arch/x64                                 # comments only
grep -rn 'mmu_map_mmio_range(\|cpu_pd7\[' src/kernel                  # MMIO mappers / windows
grep -rnE 'fat16_read_direct(_pos)?(_raw)?\(' src/kernel              # phys-integer helper callers
```

