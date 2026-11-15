# Lane C1-mmu — the direct map, RAM above 8 GiB and the compat address space

Status: implemented on branch `cp-mmu` (wave 2, 2026-11-15). Spec:
`docs/cp-address-space.md` (inventory, direct map, F1, S0–S8). This note says
what landed per step, how each was gated, and what is left. Gate output is
quoted in the lane report; the commands are below so anyone can re-run them.

## What landed, per commit

| commit (subject) | migration step | what changed |
|---|---|---|
| `S0+S2 — kmem.h helpers, the direct map …` | S0, S2 | `src/include/kmem.h`: `phys_to_virt` / `virt_to_phys` / `kimg_pa` / `ioremap` (identity on ARM). PML4[256] = direct map at `0xffff800000000000`, its PDPT[0..7] = the existing identity PDs (`pd1_kernel` for 1–2 GiB), so attributes are unchanged. PML4[257] = ioremap area at `0xffff808000000000` (PCD\|PWT, 2 MiB, bump-allocated, never re-pointed). Both installed in every `cpu_pml4[c]` and copied into every v2 root. `[DMAP] … alias=ok` boot row. Linker ASSERT image ≤ `CP_KHOLE_HI`. Tools: `tools/cp/mmu/castscan.py`, `runscan.sh`, `gates.sh`. |
| `S1+S4 — convert every identity-dependent cast …` | S1, S4 | All class-A sites (default build, test vehicles, unit tests) converted. `mmu_map_mmio_range` returns a pointer (direct map below 4 GiB, ioremap above) and no longer edits identity PDs; NVMe and virtio-gpu BAR windows are ioremap windows; the MMIO-shadow frame reservation is gone. `tools/cp/mmu/castscan_check.sh` + `castscan.allow` = the static gate. |
| `S5 — native v2 roots drop the identity map …` | S5, D6 | A native v2 root's PDPT keeps only [1] → `kimg_pd` (the image's 2 MiB leaves, `[0x70000000, 0x78000000)`) plus the user window and the kernel half. A kernel #PF below 8 GiB that is not a user VA prints `[IDMAP-TRIP] cr2= rip= cr3= pid=`. `vm_arch_resume_va_ok` accepts kernel RIPs only in the image. `-DCP_MMU_KEEP_IDMAP` = the old shape (A/B). |
| `S3a+S3b — parse the boot memory map …` | S3a, S3b | `boot.s` keeps EAX/EBX; multiboot1 mmap parsed, fw_cfg `etc/e820` read every boot as a cross-check / fallback (`[MEMMAP]`). RAM above 8 GiB mapped into the direct map (static PDs up to `DMAP_RAM_MAX` = 64 GiB). Frame pool extents are a table; high extents accepted only if the map says RAM and three probes agree; the allocation hint starts above 8 GiB (no identity alias anywhere = tripwire); `fr_hi=` counter. |
| `kernel-mode #PF on a valid user address is a demand fault …` | D11 + C4-x | Kernel #PF on a user VA of the current AS → `vm_handle_fault` with the real R/W bit before the fatal-dump path. writev/readv/l_misc_readv/pread64/pwrite64 validate (readv with write=1 → COW). |
| `S6+S7 — the compat address space …` | S6, S7, D4, D7, D12 | `addr_space.layout/va_lo/va_hi`; `vm_va_is_user` / `vm_range_is_user` everywhere a user range is checked; compat roots (per-AS PD for the image leaves, 4-level walker over PML4[0..255], full teardown); `MAP_FIXED_NOREPLACE`; compat placement; low `ET_EXEC`; every Linux-ABI process gets the compat layout; floor 8 GiB. |
| `S8 — compat user VA starts at 0x10000 …` | S8 | `CP_COMPAT_FLOOR = 0x10000`; `src/user/linux_probe_lowva.c`. |

## Helper API (for other lanes)

```c
#include "kmem.h"            /* frame.h and vm.h include it */
void    *phys_to_virt(uint64_t pa);        /* RAM, sub-4 GiB legacy MMIO */
uint64_t virt_to_phys(const volatile void *kernel_ptr); /* DMAP or image; else [KMEM-BADVA] */
uint64_t kimg_pa(const volatile void *image_symbol);    /* K0: == (uint64_t)sym */
volatile void *ioremap(uint64_t pa, uint64_t len);      /* device windows above 4 GiB */
```

- A frame from `frame_alloc*()` is a **physical** address: dereference it only
  through `phys_to_virt()`, free it with `frame_free(virt_to_phys(p))` when you
  kept the pointer. Frames can be above 8 GiB, where nothing but the direct map
  reaches them.
- DMA addresses of kernel-image buffers: `kimg_pa(&buf)`; of a frame-backed
  buffer you hold as a pointer: `virt_to_phys(p)`.
- New int↔pointer casts fail `bash tools/cp/mmu/castscan_check.sh` (linuxci);
  review, convert, or add to the allowlist with `--update`.
- User-range checks: `vm_va_is_user(as, va)` / `vm_range_is_user(as, va, len)`
  (`vm.h`), never `USER_VA_BASE`/`USER_VA_TOP` comparisons.
- For C2-sys's tmpfs: its `T_KVA` macro becomes `phys_to_virt(x)` (one line).

## F1 as implemented (x86_64)

```
applies to      every PROC_FLAG_LINUX_ABI process (compat track of the v2 loader);
                CP_COMPAT_AS=0 keeps all native.  Native processes: USER_VA_BASE/TOP unchanged.
user VA         [0x10000, 0x7ffffffff000) minus the kernel hole
kernel hole     [0x70000000, 0x78000000)  (CP_KHOLE_HI fixed = the image ceiling, linker ASSERT)
                never a region, never validatable; MAP_FIXED -> ENOMEM, NOREPLACE -> EEXIST,
                hints inside it are ignored; not shown in /proc/<pid>/maps
MAP_FIXED       any page-aligned user range (replaces); outside user VA -> ENOMEM
NOREPLACE       over an existing mapping or the hole -> EEXIST; addr 0 -> EPERM; atomic under vm_lock
hint            honoured anywhere in user VA when free
no-hint mmap    native MMAP window first-fit [64+2 GiB, 64+20 GiB) (unchanged placement), then
                top-down from 0x7ffff7fff000 above the hole, then top-down below it
main ET_DYN     USER_IMG_BASE (64 GiB) as native; interpreter in the USER_LIB band as native
main ET_EXEC    at its link address (must be user VA, outside the hole)
stack / brk     as native (main stack top 96 GiB; heap slot at 65 GiB)
kernel half     PML4[256] DMAP 0xffff800000000000, PML4[257] ioremap 0xffff808000000000
roots           compat: PML4[0..255] user on demand; PDPT[1] of PML4[0] = per-AS PD with the
                image leaves copied (US=0)
```

Differences from the draft in `docs/cp-address-space.md` §5: `CP_KHOLE_HI` is
the fixed ceiling `0x78000000` rather than `round_up(__stack_top + 64 KiB)`, so
the user-visible layout does not move when `.bss` grows; no-hint placement
keeps the native window first (so existing Linux-ABI programs keep their
placement) and only goes top-down for what does not fit; ET_DYN / stack / brk
stay at their native addresses.

## Gates (linuxci, KVM, every boot under `flock /tmp/cp-qemu.lock`)

```
bash tools/gate/t3_linux_probe.sh lowva
flock /tmp/cp-qemu.lock env GATES_LOCKED=1 bash tools/cp/mmu/gates.sh probes \
   hello timers futex_sig fpu_sig tramp_sig join mem mmap_race vmcommit redzone proc fd clock lowva
bash tools/cp/mmu/gates.sh wave            # smoke: all probes in one boot
TOOLS_SKIP=1 bash tools/cp/mmu/gates.sh unit
T3_QEMU_ARGS="-m 16G" bash tools/gate/t3_linux_probe.sh lowva   # RAM above 8 GiB
bash tools/cp/mmu/castscan_check.sh
make ARCH=arm TOOLS_SKIP=1 hobbyos.elf     # in a separate tree (shared hobbyos.elf)
```

Results: see the lane report.

## Not done / open

- Wine itself, RimWorld to its menu, Doom ELF, icewm on the new MMU were not
  run in this lane (no Wine runtime staged in a HobbyOS image yet; `doom10.elf`
  absent on linuxci; RimWorld needs the NFS share). The probe set is the soak.
- No low `ET_EXEC` probe: Linux `execve` of an ELF is not implemented, and the
  probe harness stages one ELF; the loader path is code-reviewed only.
- No-hint compat placement is "native window, then top-down", not pure
  top-down; revisit when Wine runs (if its unhinted maps must sit high).
- The 16 diagnostic kernel-location predicates (`trap.c:152` BADIRET,
  `process.c:1830` BADELR, …) still call `[0x70000000, USER_VA_BASE)`
  implausible; for compat processes with code between 0x78000000 and 64 GiB
  they can print false `[BADIRET]`/`[BADELR]` rows (bounded at 20, no
  behaviour change).
- PCID/global pages, K1 (higher-half image) — deferred as in the design.
- `cpu_pml4[c]` (kernel tasks, v1) keeps the 0–8 GiB identity map (open
  question 5).
