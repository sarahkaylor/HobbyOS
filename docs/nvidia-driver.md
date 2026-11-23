# The `nv` driver: a native NVIDIA GPU driver for HobbyOS

Status: **v2 — bring-up verified on a physical RTX 4090 (AD102), including the
GSP processor; booting GSP-RM firmware is the open gate.** Branch
`nv/ada-driver`. Written 2026-11-22.

Every claim below is either measured in this repo (with the command or log line
that produced it) or marked **not reached** — a stage the driver does not
implement. Nothing is projected as if it existed.

Target hardware: **RTX 4090 (AD102, `10de:2684`)**, passed through to a HobbyOS
VM (VM 300, `hobbyos-nv`) on Proxmox `r1` (`192.168.10.174`). Build/test host:
`sarah@192.168.0.123` (x86_64, KVM).

Companion docs: `docs/nvidia-driver.md` (this file), `docs/nv-gsp-firmware.md`
(the firmware analysis), `docs/gpu-abi.md` + `docs/cyberpunk.md` (the
virtio-gpu/Venus graphics path, unchanged by this work).

---

## 0. TL;DR

HobbyOS now has a **PCIe bus layer** and a **native NVIDIA driver** that owns a
real card. On the physical 4090 in VM 300 the driver:

- enumerates the GPU by vendor + display class over PCI config space,
- sizes and maps its BARs, including a **32 GiB ReBAR aperture on BAR1**,
- decodes the GPU identity from `PMC_BOOT_0` → **AD102, chipset 0x192**,
- proves the register window is live by reading the **PTIMER counter at
  ~999 MHz** (it advances),
- finds the **PMU and GSP processors** (`UC_CAPS2` version 6; PMU asleep, GSP
  stopped) and correctly rejects the poisoned window at 0x840000,
- reports all of it from userspace: `/dev/nvctl` + `NVSMI.BIN`, and
- leaves the desktop booting normally with the card present.

What it does **not** do: accelerate anything. On Turing and later every useful
GPU function lives inside **GSP-RM** firmware, which the driver has to boot on
the GSP. That is stage P3, it is **not implemented**, and §6 states the
consequence for games plainly.

## 1. Why a native driver at all

The graphics stack in this repo renders with **Mesa in the guest talking to
`virtio-gpu 3D`**; the *host* GPU does the work (virgl for GL, Venus for
Vulkan). That is why games run at all (§6) — and also why a card inside the
guest cannot help them: in a VM with no host renderer, the guest must drive the
GPU itself.

A native driver is the only way to use a physical NVIDIA card from HobbyOS. The
open industry path is a kernel driver that (1) owns PCIe, (2) boots GSP-RM and
speaks its RPC protocol, (3) implements the memory/channel objects a userspace
driver allocates through (Mesa's `nvk` for Vulkan, Zink for GL on Vulkan), and
(4) exposes them as an ioctl surface. This document covers (1) fully and (2)
partially.

## 2. What landed

| File | Role |
|---|---|
| `src/include/pci.h`, `src/kernel/pci.c`, `src/kernel/pci_test.c` | PCIe bus layer: ECAM from ACPI MCFG with a legacy 0xCF8/0xCFC fallback, multi-function scan, BAR sizing (32/64-bit, I/O, unimplemented), capability walk, MSI/MSI-X programming. |
| `src/include/acpi.h`, `src/kernel/acpi.c` | Shared RSDP/XSDT/MCFG/DMAR locator, with a **fw_cfg fallback** that accepts both firmware shapes (an RSDP address *or* the RSDP bytes) — see §5.4. |
| `src/include/irq.h`, `src/kernel/irq_table.c`, `trap.c` | Vector registry for `0xA0..0xBF` (`irq_alloc_vector`, `irq_register_vector`, `irq_dispatch`, LAPIC id), wired into the x64 trap handler. |
| `src/include/nv.h`, `src/kernel/nv_regs.h` | Driver interfaces and the register map (public source cited per block, measurements added inline). |
| `src/kernel/nv.c` | Card-independent core: identify, PMC, PTIMER, falcon probe, staged bring-up, `/dev/nvctl` ABI, `read(2)` snapshot. |
| `src/kernel/nv_pci.c` | Hardware half: pick the GPU, map BAR0 + a bounded BAR1 window, set MEM\|MASTER, run bring-up, program MSI-X (function-masked until GSP owns interrupts). |
| `src/kernel/nv_falcon.c` | Falcon engine: DMEM/IMEM windows, the DMA ("xfer") request encoder, start/halt with bounded waits, self-test. |
| `src/kernel/nv_gsp.c` | GSP firmware stage: a read-only probe (WPR block, mailboxes, RISC-V status) and the staged boot machine, which stops at the first contract the reference dump has not settled. |
| `src/include/nv_fw.h`, `src/kernel/nv_fw.c` | Firmware handling, pure and MMIO-free: `0x10DE` container, HS-v2 + load headers with the sum check, RISC-V ELF section walk, WPR metadata codec, SHA-256, and a `/NVIDIA/GSP` file loader. |
| `src/kernel/nv_test.c`, `src/kernel/nv_fw_test.c` | 22 + 14 kernel unit-tier tests driving the shipped code against a fake register file and synthetic firmware blobs. |
| `src/user/nvsmi.c` → `/NVSMI.BIN` | Userspace report tool. |

Wiring: `main.c` calls `nv_init()` on x64 once the timer is live; `fs.c` creates
`/dev/nvctl` (character device 6) and serves `read(2)`; the compat `ioctl` route
hands `NV_IOC_*` to `nv_ioctl()`.

### 2.1 The ABI in one table

| Surface | Who | Shape |
|---|---|---|
| `read(/dev/nvctl)` | native HobbyOS programs | one `struct nv_info` snapshot per read (native programs have no `ioctl(2)` row; `SYS_MAX` is frozen) |
| `ioctl(..., NV_IOC_INFO)` | Linux-ABI consumers | the same struct, copy-in/copy-out |
| `ioctl(..., NV_IOC_REG_READ)` | diagnostics | one register **inside a whitelist** (PMC, PTIMER, PBUS, the three falcon windows) — never a blanket BAR0 read |
| `ioctl(..., NV_IOC_PEEK)` | diagnostics | up to 256 words in one call, inside the same whitelist (extended to PFB for the WPR block and the GSP window out to `+0x3000` for the RISC-V core). A per-word ioctl cannot dump an engine window, and a blanket read is the primitive it must not become. |
| `ioctl(..., NV_IOC_SELFTEST)` | diagnostics | falcon scratch + DMEM round-trip; destructive to running ucode, so never automatic |

### 2.2 Design rules this driver was built to

- **One MMIO seam** (`struct nv_mmio_ops`). The unit tier supplies a fake
  register file, so the shipped code path — not a copy — is what the tests
  exercise, including the failure modes: dead window, all-ones falcon, torn
  64-bit timer read, an engine that refuses to stop, a DMEM that drops writes.
- **A failed stage names itself**: `state` is the last completed stage,
  `failed_stage` the one that failed, both on the console and in the ABI.
- **Nothing is guessed about a chip we have not seen.** The GPU-id table holds
  only ids verified against published device tables (`nv162` TU102, `nv170`
  GA100, `nv172` GA102, `nv192` AD102, `nv1b2` GB202); an unknown id prints as
  `NV<hex>`.
- **Turing and later are gated on firmware, and the driver says so**
  (`gsp_required`, derived from the chipset id).

## 3. Register knowledge and its provenance

No code was copied from another driver. Offsets and bit layouts come from public
register documentation, cited per block in `nv_regs.h`:

| Block | Source |
|---|---|
| PMC identification + engine enables | envytools `docs/hw/bus/pmc.rst`, `rnndb/bus/pmc.xml` |
| PTIMER counter + retry loop | `docs/hw/bus/ptimer.rst` |
| PBUS interrupt registers | `docs/hw/bus/pbus.rst`, `rnndb/bus/pbus.xml` |
| Falcon IO space, `UC_CTRL`/`UC_CAPS2` bits, `XFER_*` encoding, CODE/DATA windows | `docs/hw/falcon/{io,proc,xfer}.rst`, `rnndb/falcon.xml` |
| 9-bit chipset field | `(boot0 & 0x1ff00000) >> 20`, cross-checked against the published GPU-id ↔ name table |
| GSP window at `0x110000` | stated in the public kernel driver's GSP constructor; **not trusted** — a window is accepted only if `UC_CAPS2` decodes to a falcon version 3..6 |
| every offset the GSP stage needs | **measured** on this card against NVIDIA's own driver in a reference guest (`docs/nv-gsp-reference.md`); §5.5 lists what that corrected |

Where a value is inferred rather than documented, the comment says so and the
driver logs the raw register so the inference is checkable on hardware. Three
places where the *hardware* corrected the documentation are recorded in §5.3.

## 4. Build and run

```bash
make ARCH=intel -j8 hobbyos.elf        # the ARM build compiles this subsystem out
./run_unit_tests_intel.sh              # x64 unit tier, nv's 18 tests included
nvsmi                                  # inside the guest: the driver's report
```

`tools/nv/deploy.sh` (lane infra) builds, uploads to `r1`, recreates VM 300,
boots it and pulls the serial log. A full loop is ~4 minutes: build, gzip the
1 GiB disk over the link (~45 s), boot, read.

## 5. Evidence

### 5.1 Physical RTX 4090, serial log

VM 300 (`hobbyos-nv`, q35, 8 vCPU, 16 GiB, `hostpci0: 0000:01:00,pcie=1`,
virtio-vga console, serial to file) booted `MODE=test` with `/WAVE.CFG` naming
`NVSMI.BIN`. Verbatim rows from `/root/hobbyos-nv/serial.log`:

```
[ACPI] fw_cfg rsdp addr=0x0000000020445352 size=4
[ACPI] no valid RSDP in the first 1 MiB of low memory
[PCI] transport=legacy-cf8 base=0x0000000000000CF8 (no MCFG)
[NV] found 0x00000000000010DE:0x0000000000002684 at 2:0.0 subsys 0x0000000000001462:0x0000000000005104
[PCI] 2:0.0 vendor=0x00000000000010DE device=0x0000000000002684 class=3:0:0 hdr=0x0000000000000080 subsys=0x0000000000001462:0x0000000000005104
[PCI]   bar0 base=0x0000000080000000 size=0x0000000001000000 mem
[PCI]   bar1 base=0x0000380800000000 size=0x0000000800000000 mem 64bit prefetch
[PCI]   bar3 base=0x0000381000000000 size=0x0000000002000000 mem 64bit prefetch
[PCI]   bar5 base=0x0000000000007000 size=0x0000000000000080 io
[PCI]   msi capable
[PCI]   pcie capable
[NV] bar1 window mapped 0x0000000040000000 of 0x0000000800000000 bytes
[NV] id boot0=0x00000000192000A1 boot1=0x0000000000000000 chipset=0x0000000000000192 gpu=AD102 stepping=161 gsp=required
[NV] pmc enable=0x0000000040000000 intr=0x00000000BADF5040(poison) pbus_intr=0x00000000BADF5040 (timer not gated by PMC_ENABLE on this chip)
[NV] ptimer t0=0x0000000199F3FE60 t1=0x000000019A403A20 dticks=0x00000000004C3BC0 dus=0x0000000000001388 hz~999MHz
[NV] falcon pmu base=0x000000000010A000 ver=6 caps=0x00000000800E0100 caps2=0x00000000EA104136 ctl=0x0000000000000020 busy=0x0000000000000000 present
[NV] falcon gsp base=0x0000000000110000 ver=6 caps=0x0000000080420100 caps2=0x00000000CA101136 ctl=0x0000000000000010 busy=0x0000000000000000 present
[NV] falcon sec2 base=0x0000000000840000 ver=6 caps=0x0000000080420100 caps2=0x00000000CA101136 ctl=0x00000000BADF5620 busy=0x00000000BADF5620 absent
[NV] state=falcon chip=AD102 bar0=0x0000000080000000 bar0size=0x0000000001000000 bar1=0x0000380800000000 bar1size=0x0000000800000000 bar3=0x0000381000000000 bar3size=0x0000000002000000 falcons=0x0000000000000003 vec=0 gsp=required
```

and from userspace in the same boot:

```
nv: HobbyOS native NVIDIA driver report
nv: abi         1
nv: state       falcon
nv: gpu         AD102 chipset 0x0000000000000192
nv: pci         2:0.0 vendor 10de:0x0000000000002684
nv: bar0 phys   0x0000000080000000
nv: bar0 size   0x0000000001000000
nv: bar1 phys   0x0000380800000000        <- 32 GiB ReBAR aperture, base above 4 GiB
nv: bar1 size   0x0000000800000000
nv: bar3 phys   0x0000381000000000
nv: pmc boot0   0x00000000192000A1
nv: ptimer hz   999347200
nv: irq vector  0x0000000000000000
nv: falcon      pmu v6 caps 0x00000000800E0100 caps2 0x00000000EA104136 ctl 0x0000000000000020
nv: falcon      gsp v6 caps 0x0000000080420100 caps2 0x00000000CA101136 ctl 0x0000000000000010
nv: falcon      sec2 absent
nv: gsp         required on this chip; 3D/memory/display work only after GSP-RM boots
nv: verdict     driver reached 'falcon'; hardware 3D is not available until GSP-RM is running
```

Reading of those rows, in plain terms:

| observation | what it proves |
|---|---|
| `10DE:2684` at `2:0.0`, subsys `1462:5104` | the pass-through worked; the driver found the real card, not a fake |
| BAR1 = **0x800000000 = 32 GiB**, 64-bit prefetchable, base `0x380800000000` | ReBAR is enabled and the PCIe layer sizes a 32 GiB 64-bit BAR correctly (a naive 32-bit BAR probe cannot) |
| `boot0=0x192000A1` → chipset `0x192` → AD102 | identification is from the card, not from the PCI id table |
| `dticks=0x4C3BC0` over `0x1388` µs → **999 MHz** | the register window is live and the PTIMER really counts; a stuck or aliased window cannot produce this |
| PMU `ctl=0x20` (SLEEPING), GSP `ctl=0x10` (STOPPED) | the processors' control registers decode to coherent states, which is what a driver needs before loading ucode |
| SEC2 window at 0x840000 reads `0xBADF5620` → absent | the driver refuses a poisoned window instead of accepting "version 6" from garbage (§5.3) |

Desktop evidence with the card present: `Desktop starting...` follows the driver
rows, and a QMP screendump of that boot is `docs/nv-4090-desktop.png`
(1920×1080, non-black fraction 1.0, taskbar + clock + cursor visible).

### 5.2 Unit tier

`./run_unit_tests_intel.sh` on the build host (KVM), merged tree:

```
Tests run:    490
Tests failed: 0
```

The 490 include `pci_test`'s MCFG/BAR/capability/multi-function/IRQ cases and
`nv_test`'s 18, which drive the real `nv.c`/`nv_falcon.c` through a fake
register file. The same kernel booted on the physical card also reports
`Tests run: 483` / `0 failed` in `MODE=unit_tests` — the suite is green with and
without the GPU and under both TCG/KVM and the Proxmox VM.

### 5.3 Three things the hardware taught us (each is a fixed defect)

1. **PTIMER is not gated by `PMC_ENABLE` bit 16 on modern chips.** The bit reads
   0 and refuses the write on AD102. The first version treated that as fatal and
   stopped at stage `pmc`, so it never probed a single processor. Now the bit is
   only written on pre-G80 parts and the counter itself (999 MHz, advancing) is
   the proof. *A read is evidence; an unverifiable write is not.*
2. **NVIDIA sentinel values decode as plausible register contents.** A read of
   a window that is not the engine answers `0xBADF....`, and `0xCA101136 & 0xF
   == 6`, so a version-based probe accepted a sentinel as a falcon v6. Sentinels
   are now rejected for `BOOT_0`, `UC_CAPS`, `UC_CAPS2` and `UC_CTRL`. The
   *meaning* of the two sentinels was wrong at the time and is corrected in
   §5.5: `0xBADF5040` is normal on this chip, `0xBADF5620` means fenced.
3. **A test reporter that fails unconditionally looks exactly like a broken
   driver.** `pci_test`'s hex row helper printed `FAIL` for every comparison,
   including `got == want`; five suites read as red on a green build. It is an
   assert-equal now.

### 5.4 ACPI under OVMF: measured, still open

With OVMF + limine (the VM's boot path) there is no RSDP in the legacy scan
window, so the PCIe layer falls back to `0xCF8` config access — which works for
this card (bus 2) and is why every row above is real. The driver now also asks
QEMU's fw_cfg for `etc/acpi/rsdp`; measured on this VM, that entry is **4 bytes**
containing `"RSD "` (`0x20445352`), i.e. neither the pointer form (QEMU's BIOS
path publishes a 4-byte address) nor a complete RSDP (20+ bytes with a valid
checksum). The fallback therefore declines it rather than guessing, and logs the
size. **Open item** — see §8.5; the experiment is a direct-kernel/BIOS boot of
the same kernel, where the entry should be the address form, plus a look at what
firmware this OVMF build actually publishes.

### 5.5 The reference driver on the same card, and what it corrected

A reference guest (VM 301, Ubuntu 24.04, `0000:01:00` passed through, driver
**580.178.04**, GSP-RM running, `vulkaninfo` rendering, 25 °C / 19 W idle)
dumped BAR0 read-only twice — before any driver bound and again with the stack
up. Full tables: `docs/nv-gsp-reference.md`; raw dumps in
`docs/nv-gsp-reference-evidence/`. Four of this driver's assumptions died there:

| assumption | measured | correction now in the code |
|---|---|---|
| `0xBADF....` = engine absent | `0xBADF5040` persists at `PMC_INTR_0`/`PMC_INTR_EN_0`/`PBUS_INTR_0` **while the card renders Vulkan** | three sentinels with three meanings (`NV_REG_UNREADABLE`, `NV_REG_FENCED`, `NV_REG_…720`); `pmc intr=…` no longer prints "poison" for a healthy card |
| a sentinel window has no engine | SEC2 at `0x840000` reads the fence word pre-init and falcon-shaped afterwards (`0,0,0x10,0x01000000`) | `struct nv_falcon.fenced`; the log says `fenced`, and only `0xffffffff` still says `absent` |
| `0x111388` bit 7 = RISC-V ACTIVE | `0x00000010` with GSP-RM running — **bit 4**, same bit as falcon `UC_CTRL.STOPPED` | nothing gates on bit 7; the probe logs the raw word |
| WPR2 live check at `0x1FA824/28`, "nonzero hi" | hi is `0` in **both** states — a dead word | the probe now reports `0x1FA804/08` (the pair that moves) with a three-way verdict, and calls the result a *signature*, not "WPR2 is up": whether that pair is WPR2 is still open (§8) |

Two further facts worth having: `0x001668` is **not** a BAR0 register (it reads
the unreadable word in both states, so the "post-reset mask write" cannot be
done at that address), and the write-protected region is **torn down when the
owner goes away** — see the cross-check below.

Cross-check, two independent stacks on one card:

| register (BAR0) | reference, pre-init | HobbyOS `nv`, after that guest stopped |
|---|---|---|
| `0x1FA804` | `0x041B3000` | `0x041B3000` |
| `0x1FA808` | `0x05CF3000` | `0x05CF3000` |
| `0x1FA82C` / `0x1FA830` | `0x00100000` / `0x05CF3FF0` | `0x00100000` / `0x05CF3FF0` |
| `0x1FA824` / `0x1FA828` | `0x1FFFFE00` / `0` | `0x1FFFFE00` / `0` |
| `0x110108` `UC_CAPS` | `0x80420100` | `0x80420100` |
| `0x11012C` `UC_CAPS2` | `0xCA101136` | `0xCA101136` |
| `0x110100` `UC_CTRL` | `0x10` | `0x10` |
| `0x111388` | fenced | fenced (`0xBADF5620`) |
| `0x840000` SEC2 | fenced | fenced (`0xBADF5620`) |

That agreement is the useful part: our reader and NVIDIA's stack see the same
words, so a later disagreement is a bug in *our* code, not in the map. It also
answers a question the boot stage needed — a card handed over after a working
driver ran comes back with the WPR block at its **pre-init** signature, so
"tear down someone else's region" is the rare path and "lay out the region" is
the normal one.

### 5.7 Stage 5 measured: SEC2 is gated, and PMC_ENABLE is not the gate

The first stage that writes to the card produced a definite answer, just not the
one the plan expected:

```
[NV-SEC2] pmc_enable=0x0000000040000000 while the window is fenced
[NV-SEC2] probe write-then-read ctl=0x00000000BADF5620 (write ignored as well: fully fenced)
[NV-SEC2] unfence FAILED: no single PMC_ENABLE bit lifts the fence, so SEC2 is gated by
          something this driver has not done yet (the secure-boot handshake is the
          remaining candidate)
```

That is four facts, each of which took a register read to establish:

1. **The whole SEC2 block (`0x840000`) is fenced, and it is *write*-fenced too** —
   a write to `UC_CTRL` reads back the fence word, so the host cannot even
   signal the engine it is not to run.
2. **`PMC_ENABLE` reads `0x40000000`** — one bit. Engine enable is not how SEC2
   is gated on this part; a single-bit sweep across all 32 bits (restored
   afterwards) lifts nothing.
3. **The PMU is the only falcon readable pre-boot** (`ctl=0x20`, valid
   `UC_CAPS/CAPS2`). That is consistent with the boot chain being
   host → PMU → FWSEC/ACR → SEC2 → GSP, with SEC2 unreachable until the PMU has
   run the secure handshake. The contract lists FWSEC-FRTS as its step 5 and
   marks it "not implemented in HobbyOS" — that step is now the blocker by name
   rather than by guess.
4. The booter's own artifacts are ready and were verified in the same run: the
   metadata block sits in a DMA-able frame (`block_phys=0x2054e2000`), the
   radix3 root is `0x2054cd000`, the ad10x signature address resolved.

The PMU contract (`docs/nv-pmu-acr-contract.md`) then answered this properly —
and it said the PMU is *not* in the boot path on AD102 at all: FWSEC runs on the
**GSP falcon**, and the boot order is
`reset GSP → FWSEC-FRTS → reset GSP → 0x111668=0x111 → SEC2 booter → RPC`.
`UC_CTRL=0x20` on the PMU decodes as bit 5 = SLEEPING: firmware resident, idle,
nothing to upload. So the PMU reads are a red herring that the log now labels as
such.

The contract also named a two-write experiment that separates the two possible
causes of the fence, so it was run:

```
[NV-SEC2] block1400: 0x5 0x20040004 0x0 … 0x2000 0x8000 0x81000 … 0x7 0x0
[NV-SEC2] after pulse ctl=0xBADF5620 addr2=0xBADF5620 still fenced
          -> the fence is privilege state, so FWSEC must run on the GSP falcon first
```

Two results for two writes: the fence is **privilege-gated, not reset-state**, so
the engine-reset pulse alone will never open `0x840000`; and the `0x1400` block
*is* readable pre-boot, with `0x1438 = 0` (no FWSEC status recorded yet), which
gives the next stage its success oracle. FWSEC-FRTS — on the GSP falcon, with
`frtsOffset>>12`/`frtsSize>>12`/`type=2` and `init_cmd = 0x15` — is the next
stage, and it needs the card's VBIOS for its ucode (AppID `0x85`).

### 5.8 The VBIOS chain is resolved, and FWSEC now has a different blocker

`docs/nv-vbios-map.md` settled the ucode lookup from dumped bytes, and the driver
implements it. This run, on the card:

```
[NV-VBIOS] images: img0=0xFC00 efi=0x14E00 b2=0x24A00 type2=0xE0 shift=0x14E00
[NV-VBIOS] ucode table at 0x95BE8 hdr=01 06 06 10
[NV-VBIOS] slot 9 app=133 logical=0x2F03C -> desc 0x43E3C FWSEC
[NV-VBIOS] desc hdr=0x032C0301 hdrsize=0x32C stored=0xFF80 imem=0xF200 dmem=0xD80
           engid=0x400 ucode=9
[NV-VBIOS] FWSEC payload 0x44168..0x540E8
```

Every number matches the byte-level map derived independently from a Linux
guest's dump, including the payload's exact end (`0x44168 + 65408 = 0x540E8`) and
the `IMEM + DMEM == StoredSize` checksum (`61952 + 3456 = 65408`). The address
rule that made this hard: VBIOS-internal pointers address a **logical** image with
the EFI ROM removed, so `flash = logical + 0x14E00` above `0xFC00` — which is why
`0x80DE8` read as noise when used raw.

Then FWSEC step 1-3, also measured, and it moved the blocker:

```
[NV-FWSEC] gsp already STOPPED (ctl=0x10): skipping the destructive reset,
           which measurably fences the window instead of resetting it
[NV-FWSEC] reset done=1 scrubbed=1 ctl=0x10 hwcfg2=0x47B7
[NV-FWSEC] dma cfg 624=0xBADF5620 10c=0xBADF5620 600=0xBADF5620
```

Two findings, both of which changed the code:

1. **The documented falcon reset must not be applied here.** On this card it turns
   the GSP window fenced (`0x10 → 0xBADF5620`) and `0x0F4` bit 31 never sets. The
   core is already STOPPED on handover, which is the property that matters, so the
   reset is skipped — with a test asserting the destructive half does not run on a
   stopped core and does run on a running one.
2. **The DMA-configuration registers (`0x600`, `0x624`, `0x10C`) are fenced too.**
   They read back the inaccessibility signature after being written. So the falcon
   block past its basic IO page is not ours yet: on this generation the driver is
   expected to replay the VBIOS's **devinit (golden-register) tables** before any
   engine register above the IO page is writable. That is the next rung, it is
   named, and it is the last thing standing between this driver and launching
   FWSEC.

### 5.6 What the two contract docs settled (and one plan they killed)

`docs/nv-gsp-wpr-contract.md` and `docs/nv-gsp-rpc-contract.md` were built from
public sources after the hardware dump, and they changed the plan:

- **The host does not program the write-protected region on AD102/535.** The
  SEC2-run booter builds WPR2 from the 256-byte metadata block; the host
  writes that block into *system* memory and hands over its physical address.
  Every earlier step here that said "lay out the region, then program it" was
  wrong at the first move.
- **`0x1FA824/828` are `WPR2_ADDR_LO/HI`, a (start, END) pair in 4 KiB units** —
  and on this card they are read-protected (`0x1FA7C8` bit 0 = 1, measured
  `0x45CB8F` through this driver). The words decode to a start above any real
  24 GiB FB, so they are a *name* for the region, **not a state oracle**. The
  probe now says exactly that instead of printing a verdict.
- **`0x1668` was falcon-relative all along**: the live word is `0x111668` =
  `BCR_CTRL`, value `0x111` = VALID | RISCV | BRFETCH (run the RISC-V core, fetch
  from boot ROM). Measured `0x1` with GSP-RM up, `0xBADF5620` (fenced) before.
- **The 256-byte metadata block is now implemented field by field** (33 offsets,
  V1 layout), replacing the earlier hypothesis that had a region array and a
  size field which do not exist. `verified` (`0xA0A0…`) is written by the
  booter — a protocol-free proof that the block was accepted, and the test
  asserts a host-written block never reads as verified.
- **The transport, when we get there**: queues in host system memory described
  by a 129-entry table of GPU bus addresses; 48-byte element header; `VRPC`
  messages with `length` including the 32-byte header; `SET_SYSTEM_INFO` /
  `SET_REGISTRY` queued *before* the processor starts; first round trip is
  `GET_GSP_STATIC_INFO`; doorbell `PGSP_QUEUE_HEAD(0) = 0x110C00`.

One open discrepancy: the reference's pre-init dump had the whole `0x110000`
window fenced, while `nv` reads real `UC_CAPS/CAPS2` there. The likely cause is
a PCI function-level reset on VM start (QEMU does one with `pcie=1`) which
deasserts the fence for the host-visible falcon window, but that has not been
isolated. It does not block anything: `nv` treats a fenced window as untouchable
either way.

## 6. Games: what the 4090 does for RimWorld and Cyberpunk today

Two questions hide inside "do they work well", and they have different answers.

**Is the card fast enough?** Yes, by a wide margin — and this repo has the
numbers, not a guess. Cyberpunk 2077 (Steam build 2.5, Low preset) runs at a
**menu median of 142.8–144.4 fps through one virtualisation layer** (Venus on an
RX 9070 XT) and 388–399 fps host-native (`docs/cyberpunk.md` §0.2), and the same
document records the game *playing* through Venus at a **720p median of 30.3 fps**
(p10 25.5–27.6; 1080p 34.5) with the P9 functional half passing (10-minute
session, save/reload). Nothing about a 4090 makes pixel throughput the
constraint.

**Does HobbyOS use the 4090 for them?** No. Not because the driver is wrong, but
because two rungs are missing:

| title | how it runs on HobbyOS today | does `nv` accelerate it? |
|---|---|---|
| RimWorld (native Linux ELF, GL 3.3) | Mesa GL in the guest → virtio-gpu 3D → host GL (virgl) | **no** — GL on the card needs Zink over `nvk` (P5), and `nvk` needs GSP-RM (P3) |
| Cyberpunk 2077 (Windows PE, D3D12) | Wine + vkd3d-proton → Venus → host Vulkan | **no** — same dependency chain |

So the honest answer to the task's validation question is: **the card is more
than enough for these titles, and HobbyOS cannot use it for them yet.** A card
passed into a HobbyOS VM today gets enumerated, identified and reported — it does
not render. Claiming otherwise would be the single most misleading thing this
document could do.

What closes the gap, in order: **P3** (GSP-RM boot, `docs/nv-gsp-firmware.md`),
**P4** (RM RPC client: memory, VM, channels, fences), **P5** (nouveau-shaped
ioctl surface + Mesa `nvk`, so Vulkan runs on the card and Zink carries GL). P3
is firmware work gated on NVIDIA-signed blobs; P4/P5 are large but conventional.
Until then the supported way to run these games on HobbyOS is the
virtio-gpu/Venus stack, and `nv` is additive: it does not touch `/dev/gpu`, the
Mesa stack, or the virtio-gpu path, and a machine with no NVIDIA card costs one
`[NV]` line at boot.

## 7. The ladder to hardware 3D

| stage | content | state |
|---|---|---|
| P0 | harness: HobbyOS VM on `r1` with the 4090 passed through, serial + screenshot capture, deploy script | **done** |
| P1 | PCIe bus layer + ACPI locator + MSI-X vector registry | **done**, unit-tested |
| P2 | identify, PMC, PTIMER, falcon discovery, `/dev/nvctl` + `NVSMI.BIN` | **done on hardware** (§5.1) |
| P3 | GSP-RM boot: firmware inventory, WPR2, booter load/run, HS/RM, RPC queues | **not reached** — analysis in `docs/nv-gsp-firmware.md` |
| P4 | RM RPC client: memory allocation, VM mapping, channels/GPFIFO, fences | not started (needs P3) |
| P5 | nouveau-shaped ioctls + Mesa `nvk`/Zink | not started (needs P4) |
| P6 | present path: card → HobbyOS scanout without a readback | not started |

## 8. Open items

1. **P3 — GSP-RM boot.** The gate. Blob inventory, container formats and the
   step-by-step sequence with observables: `docs/nv-gsp-firmware.md`. Stages 1-2
   are done on silicon; 3+ are waiting on two contracts.
1a. **Which registers are WPR2 on AD102 — settled, with a caveat.**
   `0x1FA824/828` are `WPR2_ADDR_LO/HI` as a (start, END) pair in 4 KiB units,
   per `docs/nv-gsp-wpr-contract.md` §2.1; the words themselves are
   read-protected on this card, so they never answer usefully. Reading them as
   base/size was our earlier mistake. Their *identity* is established from
   sources, their *value* is not observable — which is fine, because the host
   never writes them.
1b. **The GSP RPC transport.** Queues, element format, message format and the
   init handshake: `docs/nv-gsp-rpc-contract.md`. Nothing on the transport may be
   written before the queue-address question is answered — a wrong address hangs
   the card, and the card is somebody's working machine when we are not using it.
2. **BAR1 window.** 1 GiB mapped of a 32 GiB aperture — enough for firmware, not
   a design for RM's memory manager. The GSP stage must use the *real* aperture
   size, not the window.
3. **MSI-X is programmed but function-masked** (deliberately): the interrupt-ack
   protocol on Turing+ belongs to GSP-RM.
4. **No vIOMMU in the target VM** (device DMA is guest-physical identity).
   Supporting the emulated `intel-iommu` in front of the card is a separate task;
   the VT-d driver exists for the RDMA path.
5. **ACPI/ECAM under OVMF** (§5.4): `etc/acpi/rsdp` is 4 bytes of `"RSD "`. Next
   experiment: boot the same kernel via the BIOS/direct-kernel path and compare
   the entry, and check whether this OVMF publishes tables only through the UEFI
   configuration table (which a multiboot1 kernel never sees). Until then the
   legacy 0xCF8 path is what the log shows, and it is sufficient for this card.
6. **`nvk`** — nothing in this branch builds or touches Mesa; P5 is where that
   starts.
