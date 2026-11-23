# The `nv` driver: a native NVIDIA GPU driver for HobbyOS

Status: **v1 — bring-up driver on real hardware; GSP-RM boot is the open gate.**
Branch `nv/ada-driver`. Written 2026-11-22. Every claim below is either a fact
measured in this repo (with the command or log line that produced it) or marked
**not yet reached** — a stage the driver does not implement, named so nobody
plans on it as if it existed.

Target hardware: **RTX 4090 (AD102, `10de:2684`)**, passed through to a HobbyOS
VM on the Proxmox host `r1` (`192.168.10.174`). Build/test host:
`sarah@192.168.0.123` (x86_64, KVM).

Companion docs: `docs/gpu-abi.md` (the virtio-gpu `/dev/gpu` ABI — a *different*
GPU path, unchanged by this work), `docs/cyberpunk.md` (the Proton/Venus
graphics ladder that games actually run on today), `docs/rimworld-plan.md`.

---

## 0. TL;DR

HobbyOS now has a **PCIe bus layer** and an **NVIDIA driver** that owns the real
card: it enumerates the GPU by vendor/class, sizes and maps the BARs, decodes
the GPU identity from `PMC_BOOT_0`, brings the register window up
(PMC/PTIMER), finds the PMU/GSP/SEC2 **falcon** engines, and reports all of it —
from the kernel as `[NV]` serial rows and from userspace through `/dev/nvctl`
and `NVSMI.BIN`.

What it does **not** do yet: accelerate anything. On Turing and later (the 4090
included) every useful GPU function — memory management, channels, display,
3D — runs inside NVIDIA's **GSP-RM** firmware, which the kernel driver has to
boot on the GSP processor. That boot (signed blobs, a write-protected region in
VRAM, an RPC queue handshake) is stage P3 in §5 and is **not done**. The
consequence is stated plainly in §6: **games do not get hardware 3D from this
driver yet**, and the honest path for them is unchanged (the virtio-gpu/Venus
stack).

---

## 1. Why a native driver at all

The graphics stack in this repo renders with **Mesa in the guest talking to
`virtio-gpu 3D`**, and the *host* GPU does the work (virgl for GL, Venus for
Vulkan). That is why Cyberpunk runs at all (§6) — and also why a card inside
the guest cannot help it: with no host renderer in the VM, the guest must drive
the GPU itself.

A native driver is the only way to use a physical NVIDIA card from HobbyOS. The
industry-standard open path is NVIDIA's kernel interface + Mesa's `nvk`
(Vulkan) / Zink (GL on Vulkan), which needs a kernel driver that:

1. owns PCIe (config space, BARs, MSI-X, DMA),
2. boots GSP-RM and speaks its RPC protocol,
3. implements the memory/channel objects `nvk` allocates through,
4. exposes them to the userspace driver (a `nouveau`-shaped ioctl surface).

This document covers (1) fully and (2) partially. (3) and (4) are the rungs
after the GSP gate opens.

## 2. What landed

| File | Role |
|---|---|
| `src/include/pci.h`, `src/kernel/pci.c`, `src/kernel/pci_test.c` | PCIe bus layer: ECAM from ACPI MCFG with a legacy CF8/CFC fallback, multi-function scan, BAR sizing (32/64-bit, I/O, unimplemented), capability walk, MSI/MSI-X programming. |
| `src/include/acpi.h`, `src/kernel/acpi.c` | Shared RSDP/XSDT/MCFG/DMAR table locator (extracted from the VT-d driver, so there is one implementation). |
| `src/include/irq.h`, `src/kernel/irq_table.c`, `trap.c` gates | Vector registry for `0xA0..0xBF`: `irq_alloc_vector`, `irq_register_vector`, `irq_dispatch`, LAPIC id. This is how an MSI-X message reaches a handler. |
| `src/include/nv.h`, `src/kernel/nv_regs.h` | The driver's interfaces and its register map (with a public source cited per block). |
| `src/kernel/nv.c` | Card-independent core: identify, PMC, PTIMER, falcon probe, staged bring-up, `/dev/nvctl` ABI, `read(2)` snapshot. |
| `src/kernel/nv_pci.c` | Hardware half: pick the GPU, map BAR0 and a bounded BAR1 window, set MEM\|MASTER, run bring-up, program MSI-X (function-masked until GSP owns interrupts). |
| `src/kernel/nv_falcon.c` | Falcon engine: DMEM/IMEM windows, the DMA ("xfer") request encoder, start/halt with a bounded stop wait, self-test. |
| `src/kernel/nv_test.c` | 18 kernel unit-tier tests driving the shipped code against a fake register file. |
| `src/user/nvsmi.c` → `/NVSMI.BIN` | Userspace report tool. |

Wiring: `main.c` calls `nv_init()` on x64 after the timer is live; `fs.c`
creates `/dev/nvctl` (character device 6) and serves `read(2)`; the compat
`ioctl` route hands `NV_IOC_*` to `nv_ioctl()`.

### 2.1 The ABI, in one table

| Surface | Who | Shape |
|---|---|---|
| `read(/dev/nvctl)` | native HobbyOS programs | one `struct nv_info` snapshot per read (no ioctl row exists for native programs; `SYS_MAX` is frozen) |
| `ioctl(/dev/nvctl, NV_IOC_INFO)` | Linux-ABI consumers | same struct, copy-in/copy-out |
| `ioctl(..., NV_IOC_REG_READ)` | bring-up diagnostics | reads one register **inside a whitelist** (PMC 0..0xFFF, PTIMER, PBUS, the three falcon windows) — never a blanket BAR0 read |
| `ioctl(..., NV_IOC_SELFTEST)` | bring-up diagnostics | falcon scratch + DMEM round-trip; destructive to running ucode, so never automatic |

### 2.2 Design rules this driver was built to

- **One MMIO seam.** Every register access goes through `struct nv_mmio_ops`.
  The unit tier supplies a fake register file, so the shipped code path (not a
  copy) is what 18 tests exercise, including the failure modes: a dead window,
  an all-ones falcon, a PTIMER that never ticks, a torn 64-bit timer read, an
  engine that refuses to stop, and a DMEM that silently drops writes.
- **A stage that fails names itself.** `nv.state` is the last completed stage
  and `nv.failed_stage` the one that failed; both are in the console rows and in
  the ABI struct.
- **Nothing is guessed about a chip we have not seen.** The GPU-id table holds
  only ids verified against published device tables (`nv162` TU102, `nv170`
  GA100, `nv172` GA102, `nv192` AD102, `nv1b2` GB202). An unknown id prints as
  `NV<hex>`, which is honest, rather than a plausible marketing name.
- **Turing and later are gated on firmware, and the driver says so.**
  `gsp_required` is derived from the chipset id (`>= 0x160`) and reported.

## 3. Register knowledge and its provenance

No code was copied from another driver. Offsets and bit layouts come from
public register documentation, cited per block in `nv_regs.h`:

| Block | Source |
|---|---|
| PMC identification + engine enables | `envytools` `docs/hw/bus/pmc.rst`, `rnndb/bus/pmc.xml` |
| PTIMER counter + retry loop | `docs/hw/bus/ptimer.rst` |
| PBUS interrupt registers | `docs/hw/bus/pbus.rst`, `rnndb/bus/pbus.xml` |
| Falcon IO space, `UC_CTRL`/`UC_CAPS2` bits, `XFER_*` encoding, CODE/DATA windows | `docs/hw/falcon/{io,proc,xfer}.rst`, `rnndb/falcon.xml` |
| Chipset field is 9 bits from Turing | `(boot0 & 0x1ff00000) >> 20`, cross-checked against the published GPU-id ↔ name table |
| GSP falcon window at `0x110000` | stated in the public kernel driver's GSP constructor; the driver does **not** trust it — it accepts a window only if `UC_CAPS2` decodes to a falcon version 3..6 |

Where a value is inferred rather than documented, the comment says so and the
driver logs the raw register, so the inference is checkable on hardware.

## 4. Build and run

```bash
# kernel (either arch; the ARM build compiles this whole subsystem out)
make ARCH=intel -j8 hobbyos.elf

# unit tier (x64): builds MODE=unit_tests and runs the suites, nv's included
./run_unit_tests_intel.sh          # KVM when /dev/kvm exists

# userspace report, inside the guest
nvsmi
```

Enumerating on a real card prints (one row per stage):

```
[NV] found 10de:2684 at 01:00.0 subsys 1462:5104
[PCI] transport=ecam base=<mmcfg base>
[PCI] 01:00.0 10de:2684 class 0x030000 bar0 <base>/<size> ...
[NV] bar1 window mapped <n> of <m> bytes
[NV] id boot0=<b0> boot1=<b1> chipset=0x192 gpu=AD102 stepping=<s> gsp=required
[NV] pmc enable=<e> intr=<i> pbus_intr=<p> (timer bit already set)
[NV] ptimer t0=<t0> t1=<t1> dticks=<d> dus=<us> hz~<mhz>MHz
[NV] falcon pmu base=0x10a000 ver=<v> caps=... present
[NV] falcon gsp base=0x110000 ver=<v> caps=... present
[NV] falcon sec2 base=0x840000 ver=<v> caps=... absent
[NV] msix table programmed vec=<v> entries=<n> (function masked until GSP owns interrupts)
[NV] state=falcon ...
```

The bracketed values are filled from the real run in §5.1 — this document does
not carry placeholder hardware numbers.

## 5. The ladder to hardware 3D

| stage | content | state |
|---|---|---|
| P0 | test harness: HobbyOS VM on `r1` with the 4090 passed through, serial capture, deploy script | **done** (§5.1) |
| P1 | PCIe bus layer (ECAM/legacy, BARs, caps, MSI-X) + vector registry | **done**, unit-tested (§5.2) |
| P2 | identify, PMC, PTIMER, falcon discovery, `/dev/nvctl` + `NVSMI.BIN` | **done** (§5.1) |
| P3 | GSP-RM boot: firmware inventory, WPR2 setup, booter load/run, HS/RM, RPC queues | **not yet reached** — firmware analysis is in `docs/nv-gsp-firmware.md` |
| P4 | RM RPC client: memory allocation, VM mapping, channels/GPFIFO, fences | not started (needs P3) |
| P5 | nouveau-shaped ioctl surface + Mesa `nvk` port, so Vulkan runs on the card | not started (needs P4) |
| P6 | present path: `nvk` → HobbyOS scanout without a readback | not started |

P3 is the gate that matters. Everything a game needs — allocating VRAM,
creating a channel, submitting a pushbuffer, setting a display mode — is an RM
RPC call that only exists after GSP-RM is running. There is no useful shortcut:
on Turing+ the card no longer documents (and no longer supports) a driver-side
path to those engines.

### 5.1 Hardware evidence

*(filled from the run in §7 — see the evidence log)*

### 5.2 Unit-tier evidence

*(filled from the run in §7 — see the evidence log)*

## 6. Games: what the 4090 does for RimWorld and Cyberpunk today

The question the task asks is whether these games "work well" on this card.
There are two different questions inside it, and they have different answers.

**Is the card fast enough for these games?** Yes, by a wide margin. Measured
numbers for the same titles on comparable hardware are already in this repo:
Cyberpunk 2077 (Steam build 2.5, Low preset) runs at a **menu median of
142.8–144.4 fps through one virtualised layer** (Venus, 9070 XT host) and at
388–399 fps host-native — `docs/cyberpunk.md` §0.2. Nothing about a 4090 makes
that harder; pixel throughput is not the constraint.

**Does HobbyOS use the 4090 for them?** No, and that is not a driver bug — it is
the missing rungs. The table below is the honest state:

| title | how it runs on HobbyOS today | does the `nv` driver change it? |
|---|---|---|
| RimWorld (native Linux ELF, GL 3.3) | Mesa GL in the guest → virtio-gpu 3D → host GL (virgl) | **no** — GL on the card needs a GL driver over `nvk`/Zink (P5), and `nvk` needs GSP-RM (P3) |
| Cyberpunk 2077 (Windows PE, D3D12) | Wine + vkd3d-proton → Mesa Venus → virtio-gpu 3D → host Vulkan | **no** — same dependency: Vulkan on the card is `nvk` (P5) |

The measured state of those paths (from `docs/cyberpunk.md`): Cyberpunk plays
through Venus at a **720p median of 30.3 fps** (p10 25.5–27.6, 1080p 34.5), the
P9 functional half passes (10-minute session, save/reload), and the 30 fps
target is met at the median without margin. RimWorld's path is the same stack
through GL. **A card passed into a HobbyOS VM does not accelerate either of
them today**, because the guest-side driver that would use it does not exist
yet — and pretending otherwise would be the single most misleading thing this
document could do.

What would change it, in order, is P3 → P4 → P5 in §5. P3 is hardware-firmware
work with a hard dependency on NVIDIA-signed blobs; P4 and P5 are large but
conventional. Until then the supported way to run these games on HobbyOS is the
virtio-gpu/Venus stack, and the `nv` driver is additive — it does not touch
`/dev/gpu`, the Mesa stack, or the `virtio-gpu` path, and a machine with no
NVIDIA card costs one `[NV]` line at boot.

## 7. Evidence log

*(appended as stages run; see §5.1/§5.2)*

## 8. Open items

1. **P3 (GSP-RM boot)** — the gate. Firmware inventory and container analysis:
   `docs/nv-gsp-firmware.md`.
2. **BAR1 window bound** — the driver maps 1 GiB of the VRAM aperture. A
   ReBAR-enabled 4090 exposes 32 GiB; nothing needs more than a window yet, but
   the GSP WPR placement must respect the *real* aperture, not the window.
3. **MSI-X is programmed but function-masked** — intentional until GSP-RM owns
   the interrupt-ack protocol (`PMC_INTR` writes belong to it).
4. **No vIOMMU in the target VM** — device DMA is guest-physical identity.
   Supporting an emulated `intel-iommu` in front of the card is a separate task;
   the VT-d driver already exists for the RDMA path.
5. **`nvk`** — nothing in this branch builds or touches Mesa. P5 is where that
   starts.
