# HobbyOS native NVIDIA driver (Ada / RTX 4090) — execution plan

Working plan for the `nv/ada-driver` branch. Not committed to the repo (the
public design + status doc is `docs/nvidia-driver.md`).

## 0. Objective

A native HobbyOS driver for NVIDIA GPUs (`nv`), targeting the RTX 4090
(AD102, `10de:2684`) passed through to a HobbyOS VM, built fresh (studying
nouveau / open-gpu-kernel-modules / envytools docs allowed; no copying).
Validation hardware: `root@192.168.10.174` (Proxmox `r1`), VM 117 currently
owns `0000:01:00`. Build host: `sarah@192.168.0.123` (sarahpc, x86_64, KVM).

Final validation ask: RimWorld and Cyberpunk "work well" on this card.
That is only reachable with hardware 3D inside HobbyOS, which needs (a) GSP-RM
firmware booted by the kernel driver and (b) a Vulkan userspace (Mesa nvk).
This plan delivers (a) as far as hardware verification allows, plus a measured
statement of where games stand today (Venus stack) and the exact remaining
rungs. Overclaiming here is the failure mode to avoid.

## 1. Facts established before coding (2026-11-22)

| fact | evidence |
|---|---|
| HobbyOS x64 has **no PCI bus driver**; only a private CF8/CFC helper inside `src/kernel/virtio_net.c` | grep; `docs/graphics-accel.md` §2.6 |
| ACPI RSDP/XSDT + DMAR parsing exists, private to `src/kernel/iommu_vtd.c` (`find_rsdp`, `find_acpi_table`) | read |
| VT-d IOMMU driver exists (`iommu_vtd.c`) but only for the QEMU emulated `intel-iommu` | read |
| `mmu_map_mmio_range()` maps arbitrary phys (incl. >4 GiB) non-cacheable, 1 GiB/2 MiB pages | `src/kernel/arch/x64/mmu.c:277` |
| x64 IDT has gates only for 32–47, 129, 130, 132; IRQ dispatch is `general_interrupt_inner()` with per-device globals | `src/kernel/arch/x64/trap.c` |
| No MSI/MSI-X support anywhere | grep |
| Graphics today: Mesa (virgl/Venus/lavapipe) in the guest → virtio-gpu 3D → host virglrenderer | `docs/gpu-abi.md`, `docs/cyberpunk.md` |
| Cyberpunk runs on HobbyOS (Venus, strix): 720p ~30 fps, 1080p lower; P9 target 30 fps not met | `docs/cyberpunk.md` §0 |
| r1: 4090 `01:00` + two 5090 `41:00`,`82:00`, all bound to `vfio-pci`; no host NVIDIA driver, no Vulkan, virglrenderer 1.9.0 | ssh probe |
| VM 117 (Windows gaming) owns `0000:01:00` (`hostpci0: 0000:01:00,pcie=1,x-vga=1`) — may be shut down | `cat /etc/pve/qemu-server/117.conf` |
| sarahpc: clang 21, lld, qemu, mtools, /dev/kvm, HobbyOS checkout | ssh probe |

## 2. Driver architecture

```
userspace   NVSMI.BIN  (card info / register read behind a whitelist)
              |  ioctl /dev/nvctl
kernel      nv.c        probe, lifecycle, ABI, staged bring-up state machine
            nv_regs.h   register map (PMC/PBUS/PFB/PTIMER/FALCON/GSP)
            nv_falcon.c falcon engine: IMEM/DMEM, DMA load, mailbox
            nv_gsp.c    GSP-RM boot: WPR meta, booter, HS, RPC queues
            pci.c/h     generic PCIe: ECAM(MCFG)+legacy, BARs, caps, MSI-X
            acpi.c/h    RSDP/XSDT/MCFG/DMAR table locator (shared)
            irq table   irq_register_vector() + gates 0xA0..0xBF
```

Design rules:
- `#ifdef __x86_64__` everywhere (ARM compiles empty TUs), no new syscall rows
  (SYS_MAX frozen): `/dev/nvctl` rides the Linux-ABI `ioctl` path like
  `/dev/gpu` (`file_is_devgpu`-style predicate in `file_obj.h`).
- Kernel identity-maps phys → virt, so DMA buffers use their kernel addresses
  as bus addresses. Target VM gets **no vIOMMU** (documented decision) so
  device DMA = guest-physical identity; VT-d path stays for the emulated case.
- Everything side-effecting is a state machine with a `[NV]` serial row per
  transition, so a failed stage names itself instead of vanishing.
- MMIO access goes through a `struct nv_mmio_ops` vtable so the sequencing is
  host-testable with a fake register file (this is how P1/P2/P3 get unit
  coverage without a GPU).
- No copying: register names/sequences re-derived from public documentation
  (envytools docs, open-gpu-kernel-modules headers, nouveau's *behaviour*),
  and each sequence in our source cites the datasheet section or the behaviour
  it reproduces (not the source file).

## 3. Stages and gates

| stage | deliverable | gate (must actually run) |
|---|---|---|
| P0 harness | `tools/nv/deploy.sh` (build on sarahpc → push to r1 → recreate VM → boot → pull serial), `tools/nv/gpu-switch.sh` (move 4090 between VMs, stop 117 last/first), VM 300 `hobbyos-nv`, VM 301 `linux-ref` | HobbyOS x64 boots on r1 with and without the 4090; serial captured |
| P1 pci | `acpi.c/h`, `pci.c/h`, `pci_test.c`, IRQ vector table | unit tier green (host + x64 QEMU); on r1 the 4090 is enumerated with BAR sizes equal to the reference Linux VM's |
| P2 nv core | `nv.c/h`, `nv_regs.h`, `/dev/nvctl`, `NVSMI.BIN` | `[NV]` rows on real hardware: boot0 → AD102, BAR0/BAR1/VRAM match reference VM; `NVSMI.BIN` prints them from userspace |
| P3 falcon/GSP | `nv_falcon.c`, `nv_gsp.c`, `nv_fw.c/h`, firmware fetch+analysis doc | falcon ucode loaded and run, boot status/mailbox rows captured (partial credit recorded honestly) |
| P3a firmware | `nv_gsp_firmware_check()` + `deploy.sh` staging | **DONE on silicon**: 38,061,600 B RM image read into contiguous frames, container parsed, SHA-256 == host `shasum` (`e30231ec0d317021`) |
| P3b WPR/boot | WPR2 layout, metadata, booter handoff | **GATED**: needs `docs/nv-gsp-wpr-contract.md` (lane L-wpr); each step must prove itself by read-back |
| P3c RPC | queue + message transport | **GATED** on `docs/nv-gsp-rpc-contract.md` (lane L-rpc) |
| P4 games | measurement + ladder | RimWorld/Cyberpunk numbers on the Venus stack (control) + reference-VM 4090 numbers for the same titles; explicit statement of what the driver adds and what is still missing |

## 4. Games validation shape

1. **Control A (existing stack)**: RimWorld + Cyberpunk on HobbyOS via Venus
   (strix KVM) — numbers already in `docs/cyberpunk.md`; re-quote with anchors.
2. **Control B (the card is good)**: RimWorld native Linux in VM 301 with the
   4090 passed through — fps at 1080p. Answers "is this card enough for these
   games" independent of HobbyOS.
3. **Target**: HobbyOS + native `nv` driver — needs P3 complete + Mesa nvk, so
   P4 reports the gap, not a pass. No fabricated fps.

## 5. Lanes

| lane | worktree | owns |
|---|---|---|
| L-pci | `~/Documents/GitHub/hobbyos-nv/lane-pci` | `src/include/{pci,acpi}.h`, `src/kernel/{pci,acpi,pci_test}.c`, trap.c vector table, unit_test.c registration |
| L-infra | `lane-infra` | `tools/nv/*`, r1 VMs, reference measurements, evidence |
| L-gsp | `lane-gsp` | `docs/nv-gsp-firmware.md`, firmware fetch tool, `nv_gsp.c` skeleton + host tests |
| parent | `ada` (= `nv/ada-driver`) | `nv.c/h`, `nv_regs.h`, `nv_falcon.c`, `/dev/nvctl` wiring, `NVSMI.BIN`, integration, pushes |

## 5.1 State at 2026-11-23 (measured, not planned)

| item | state | evidence |
|---|---|---|
| card owned by HobbyOS | yes | `[NV] found 10DE:2684 at 2:0.0`, BAR0 16 MiB, BAR1 32 GiB (64 MiB window mapped) |
| identification | yes | `boot0=0x192000A1 chipset=0x192 AD102 stepping=161 gsp=required` |
| timer | yes | `ptimer hz~999 MHz`, advance proven |
| falcon probe | yes | PMU/GSP `present`; SEC2 `fenced` (was wrongly reported `absent`) |
| register map cross-check | yes | 9/9 rows equal to NVIDIA 580.178.04's pre-init dump (`docs/nvidia-driver.md` §5.5) |
| firmware on disk | yes | BOOTER/LOADER/GSPRM staged as 8.3 role names, read through FAT16 |
| firmware integrity | yes | kernel SHA-256 == host `shasum` for all three blobs |
| RM image described (radix3) | **yes** | `root=0x2054cd000 pages=21 maps 9288 image pages` |
| metadata block | **yes** (built, DMA-able) | `256B magic ok wpr=0x5F8700000..0x5FE400000 block_phys=0x2054e2000` |
| VRAM layout | **yes**, matches the contract field for field | `image=0x5FBEB0000 bootbin=0x5FE2F8000 frts=0x5FE300000` |
| booter started on SEC2 | **no — gated** | `[NV-SEC2] unfence FAILED: no single PMC_ENABLE bit lifts the fence` |
| GSP-RM running | **no** | needs PMU → FWSEC → SEC2 first (measured, §5.7) |
| hardware 3D | **no** | needs GSP-RM + a Vulkan userspace |
| unit tier | **512 tests, 0 failed** | `run_unit_tests_intel.sh` on sarahpc |
| branch | `nv/ada-driver` @ `9cb42ecf`+, not merged | owner review pending |

What a reviewer should test first: `tools/nv/deploy.sh --mode test --gpu --wave
NVSMI.BIN` reproduces every row above in ~4 minutes.

## 5.2 Resume sheet (state at branch 6313df11)

Read this first after any pause. Everything below is measured, not planned.

| Stage | State | Evidence row |
|---|---|---|
| PCI / BARs / ID / timer | done | `[NV] found 10DE:2684`, `ptimer hz~999MHz` |
| Register map vs NVIDIA driver | 9/9 rows equal | `docs/nvidia-driver.md` 5.5 |
| Firmware from disk (38 MB, contiguous) | done | `[NV-GSP] fw GSPRM.BIN bytes=38061600 phys=0x200c29000` |
| radix3 for the RM image | done | `radix3 root=0x2054c3000 pages=21 maps 9288 image pages` |
| WPR metadata block (VBIOS-independent) | done | `meta 256B magic ok wpr=0x5F8700000..0x5FE400000 block_phys=...` |
| VRAM layout matches contract | done, field for field | `image=0x5FBEB0000 bootbin=0x5FE2F8000 frts=0x5FE300000` |
| VBIOS read + FWSEC ucode located | done | `desc 0x43E3C`, payload `0x44168..0x540E8`, `IMEM+DMEM==StoredSize` |
| Falcon page accepts writes | **yes** (read fence only) | `[NV-PROBE] verdict=WRITES LAND` |
| GSP reset + ucode staged (IMEM/DMEM) | done | `[NV-FWSEC] IMEM loaded`, `DMEM loaded` |
| FWSEC actually executes | **NO** | `mailbox never overwritten -- the ucode did not run` |
| SEC2 booter | blocked behind FWSEC | `[NV-SEC2] fence is privilege state` |
| GSP-RM RPC | contract written, not coded | `docs/nv-gsp-rpc-contract.md` |

### Blockers, in the order they must fall

1. **Why the core does not run.** Two candidates, both cheap to settle:
   (a) `bl_dmem_desc_v2` field layout is inferred (11 dwords, 64-bit bases);
   (b) the boot ROM's GSP parameters at `0x111210`-ish may not be writable, so
   the ROM refuses to start.  Settle (b) first: it invalidates (a) if true.
2. **SEC2's fence** lifts only after FWSEC; do not re-attempt engine pokes.
3. **The booter** (already coded, `nv_gsp_sec2_start_booter`) needs the metadata
   `verified` word — a read-back that may also be masked, so judge by behaviour.
4. **RPC** (`docs/nv-gsp-rpc-contract.md`): queues in sysmem, 129-entry address
   table, 48-byte elements, `VRPC` header, doorbell `0x110C00`.
5. **Games**: `docs/nv-games-state.md` — the honest ladder, measured.

### Rules that have earned their place (all learned the hard way here)

- A masked read is not evidence. Every wrong conclusion so far came from reading
  a fenced register as if it were data. Use `MAILBOX0`-style channels or effect.
- Hex arithmetic in comments is how a false "the document must be wrong" gets
  written; put the numbers in a test instead.
- Prefill any register whose zero means success before judging it.
- One `[NV-*]` row per stage, and the row names the stage when it fails.

## 6. Risks

| risk | mitigation |
|---|---|
| GSP boot is a multi-week RE problem | graded stages; falcon + booter progress is real, verifiable work; document the fence precisely |
| 4090 BAR1 is 32 GiB (ReBAR); QEMU may assign a huge window HobbyOS can't map | BAR probe falls back to the 256 MiB window; log both sizes; `mmu_map_mmio_range` handles >4 GiB |
| Only one VM can hold the card | `gpu-switch.sh`, stop/recreate discipline, never two VMs with the same hostpci |
| Passthrough VM needs a console without the GPU | virtio-vga + serial socket; `x-vga=0` on the 4090 |
| Agents sharing one tree collide on obj/disk.img | one worktree per lane; parent runs integration builds and all hardware boots |
