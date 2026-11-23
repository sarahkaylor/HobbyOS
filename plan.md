# HobbyOS native NVIDIA driver (Ada / RTX 4090) — execution plan

Canonical plan for branch `nv/ada-driver`. Public status doc: `docs/nvidia-driver.md`.
Contract docs this work stands on: §5. Written to be read cold after any pause.

Owner's goal, verbatim: *"create a driver that will work for an nvidia card"*,
validated by *"games like Rimworld and Cyberpunk"* with *"reasonable performance"*.
No copying from other drivers; studying them is allowed.

Hardware: RTX 4090 (AD102 `10de:2684`) on Proxmox `r1` (`root@192.168.10.174`),
driver VM 300, ROM-work VM 312. Build host `sarah@192.168.0.123`.

---

## 0. Definition of done, honestly staged

| Level | Claim | Status |
|---|---|---|
| L1 | HobbyOS owns the card, verifies NVIDIA's firmware, and builds every structure GSP-RM will consume | **done, measured** |
| L2 | FWSEC runs → SEC2 unfences → booter runs → GSP-RM reports `INIT_DONE` | **blocked at FWSEC** |
| L3 | A kernel submit reaches the 3-D engine and a clear reads back | not started |
| L4 | nvk enumerates AD102; `vkcube` draws and presents | not started |
| L5 | RimWorld / Cyberpunk render at 720p ≥30 fps | not started |

L1 stands on its own: a from-scratch driver that identifies the part, brings up
the register window, reads 38 MB of vendor firmware from disk and proves it
byte-for-byte, and lays out VRAM the way the vendor booter expects. L2 is the wall.

---

## 1. What is true today (measured; each row cites its log line)

| Stage | Evidence |
|---|---|
| PCIe; BAR0 16 MiB, BAR1 32 GiB (1 GiB window mapped), BAR3 32 MiB | `[NV] found 10DE:2684 at 2:0.0` |
| Identification | `boot0=0x192000A1 chipset=0x192 AD102 stepping=161 gsp=required` |
| PTIMER live | `ptimer hz~999MHz`, advance proved |
| Register map vs NVIDIA 580.178.04 on the same card | 9/9 rows equal (`docs/nvidia-driver.md` §5.5) |
| Falcon probe | PMU/GSP `present`; SEC2 `fenced` |
| 38,061,600 B firmware → contiguous frames, digest == host | `phys=0x200c29000`, `sha256 e30231ec…` |
| radix3 over `.fwimage` | `root=0x2054c3000 pages=21 maps 9288 image pages` |
| 256-byte WPR metadata block, DMA-able | `magic ok wpr=0x5F8700000..0x5FE400000 block_phys=0x2054e2000` |
| VRAM layout (matches contract field for field) | `image=0x5FBEB0000 bootbin=0x5FE2F8000 frts=0x5FE300000` |
| VBIOS readable; FWSEC ucode located | `desc 0x43E3C`, payload `0x44168..0x540E8`, `IMEM+DMEM==StoredSize` |
| Falcon page accepts **writes** (read fence only) | `[NV-PROBE] verdict=WRITES LAND` |
| Ucode staged on the **GSP** with signature + correct BROM params | `[NV-FWSEC] GSP IMEM+DMEM loaded`, `brom para=0xB24 engid=0x400 ucode=9` |
| FWSEC executes | **NO** — `status1438=0xBEEF0000 frts=NEVER WRITTEN`, `SEC2=still fenced` |
| Unit tier | **513 tests, 0 failed** |

Reproduce in ~4 minutes: `tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`.

---

## 2. The wall: FWSEC does not execute

The verdict channel is sound: BAR0 `0x1438` bits 31:16, prefilled with
`0xBEEF0000` so "nobody wrote" is distinguishable from "success". (`MAILBOX0` is
**not** the FWSEC channel — the vendor passes NULL there.) The ucode reaches the
GSP with the right block size, the right BROM parameters and the PKC signature
patched at DMEM `0xB24`. It still does not run.

**Experiment queue — cheapest first, one deploy each, yes/no answers.**

| # | Experiment | Positive result means | Negative means |
|---|---|---|---|
| F1 | Read GSP `0x110114` right after the IMEM load, before any reset (window still readable): expect `0xF100` | the DMA landed in the GSP | it did not → F2 |
| F2 | Load IMEM/DMEM by **PIO** (`0x180/0x184`, `0x1C0/0x1C4`), with a wait that does not poll the fenced `XFER_STATUS` | PIO runs the core → the DMA request encoding is wrong | still no run → ucode/params, not transport |
| F3 | After an attempt, read the RISC-V switch `0x111668`: `0x111` = running in RISC-V mode | it switched then stopped → FRTS question | never set → core never got that far |
| F4 | Re-check `0x840000` in the same run for the privilege drop (secondary oracle) | FWSEC's job was done | confirms F2/F3 |
| F5 | Vary the DMA context id: `0x600` is `TRANSCFG(ctx)`, ctx = register index; sysmem-no-coherent is **4**, not 5 | a ctx mismatch explains it | rules the DMA path out |

Still unrun and cheap: **B. fuse check** — read `0x8241C0 + (UcodeId-1)*4` and AND
against the descriptor's `SignatureVersions` (`0x0003`). If the image is not valid
for this part's fuses, the boot ROM rejects it and no amount of transport fixing
helps.

**Sequencing rule:** no more SEC2 work until FWSEC's channel says it ran. SEC2's
fence is privilege-gated (measured twice); engine pokes will not lift it.

---

## 3. Work packages (parallel, each with a gate)

| WP | Scope | Gate | Needs 4090? |
|---|---|---|---|
| **A. FWSEC** | F1-F5, then the launch | `0x1438` hi16 becomes a real code **and** SEC2 unfences | yes |
| **B. fuses** | fuse bitmask vs `SignatureVersions` | non-zero AND, documented | yes, read-only |
| **C. RPC (offline)** | `docs/nv-rpc-implementation-spec.md`: block map, producer header, 48-byte elements, checksum, SWAP_RX, doorbell — pure half only | the spec's 24 cases pass in the kernel unit tier on synthetic queues | **no** |
| **D. DMA facility** | `nv_dma.{c,h}`: bus address ≠ phys, identity guard, `iommu_vtd_translation_enabled()` (VT-d is already initialised from `net_rdma.c` with a passthrough entry for this GPU) | a unit test rejects a mapping whose `bus != phys` under translation | **no** |
| **E. Present gate** | `/dev/fb0` mmap: `vm_map_fb()` maps 10,485,760 B over an 8,294,400 B framebuffer; `vm.c:5303-5310` drops the clamp | pixel at (1919,1079) in a QMP screendump | **no** |
| **F. `/dev/nv*` shim** | nouveau-subset ABI (device, alloc, VM bind, channel, submit, fence) over a mock RM | probe creates device/VM/channel; fence retires | **no** |
| **G. SEC2 booter** | `nv_gsp_sec2_start_booter` (already coded) once FWSEC unfences | metadata `verified` == `0xA0A0…`, or a behavioural equivalent if masked | yes |

Ordering by leverage: D unblocks C; C unblocks the driver's upper half; E unblocks
every present measurement; F lets a real Vulkan stack attach before GSP-RM is up.
C/D/E/F are how this project progresses on days when the card is busy or FWSEC is
stuck.

---

## 4. The ladder to games (`docs/nv-games-ladder.md`; weeks are ESTIMATES)

| # | Rung | Gate | Today |
|---|---|---|---|
| N0 | 1080p present gate | 8,294,400 B maps; pixel visible | fails `-ENOMEM` |
| N1 | RimWorld's own menu on virgl 720p | ≥30 fps median, input, audio | never measured on the current winsys |
| N2 | `/dev/nv*` shim over a mock RM | probe + fence retire | absent |
| N3 | GSP-RM boots | mailbox moves, first RPC answered | FWSEC never ran |
| N4 | RM objects + first submit | 64×64 clear reads back | absent |
| N5 | nvk enumerates AD102 | `vulkaninfo` lists `AD102 (NVK)` | absent |
| N6 | **first frame** (`vkcube`) | draws + presents | absent |
| N7 | RimWorld via Zink+nvk | menu ≥30 fps, world loads | 170.9 fps synthetic only |
| N8 | Cyberpunk via vkd3d+nvk | P9 script end to end | 29.8 fps on Venus |
| N9 | "reasonable performance" | 720p ≥30 fps incl. present | unmeasurable today |

**Highest-leverage rung: N6** — the one observation where VRAM, VA, submit,
fences, shader upload and present all work at once, and exactly where both games'
already-proven userspace attaches. Critical path N3→N6 ≈ 20-37 weeks.

Two facts that keep it honest: both titles' non-graphics stacks already work on
HobbyOS (audio via virtio-snd/ALSA; Cyberpunk played 10.5 min and reloaded), and
Venus and the native driver are mutually exclusive on this card — Venus needs the
GPU on the host, `nv` needs it in the guest, and r1 has one.

---

## 5. Contracts this work stands on (read before coding)

| Doc | What it settles |
|---|---|
| `docs/nvidia-driver.md` | what the driver does; every claim anchored to a log line |
| `docs/nv-gsp-firmware.md` | blob formats, HS headers, what the boot ROM validates |
| `docs/nv-gsp-reference.md` | NVIDIA's own driver on this card: pre/post BAR0 dumps |
| `docs/nv-gsp-wpr-contract.md` | WPR2 ownership (the **booter** programs it), layout, meta fields |
| `docs/nv-pmu-acr-contract.md` | boot order: host → FWSEC on the GSP → SEC2 booter; FRTS |
| `docs/nv-vbios-map.md` | finding ucode in the ROM (EFI-stripped logical base rule) |
| `docs/nv-devinit-contract.md` | why devinit replay is dead on Turing+; the sentinel method |
| `docs/nv-fwsec-launch-contract.md` | launch sequence, BROM values, diagnosis table |
| `docs/nv-rpc-implementation-spec.md` | transport field by field + 24 test cases |
| `docs/nv-games-ladder.md` | the ladder, effort ranges, "build now" list |

Corrections already applied, so nobody re-derives them: the fence is **read-side
only**; `0x1FA824/828` are `WPR2_ADDR_LO/HI` as (start,END) in 4 KiB units and are
read-protected; there is **no** descriptor to write on GA102; FWSEC's verdict is
`0x1438`, not `MAILBOX0`; VBIOS pointers are relative to a logical image with the
EFI ROM removed (`+0x14E00` above `0xFC00`).

---

## 6. Rules of evidence (each cost at least one run)

1. **A masked read is not evidence.** Reads beyond the readable blocks return
   `0xBADF5620`, whose bit 4 is clear — "not halted" and "unreadable" look
   identical. Use a channel that can answer, or judge by effect.
2. **Prefill anything whose zero means success.** Bitten twice, in two registers.
3. **Hex arithmetic belongs in a test, not a comment.** Every "the document must
   be wrong" so far was my own subtraction slip.
4. **One `[NV-*]` row per stage; the row names the stage when it fails.**
5. **Check that an observation could have been negative** before calling a
   subsystem working.
6. **Do not copy.** Contracts and byte layouts yes; code no. Tag claims
   MEASURED / SOURCE / INFERRED.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| FWSEC becomes a long RE problem | F1-F5 are single-deploy tests; WP C/D/E/F proceed without the card |
| One card, many lanes | `tools/nv/gpu-switch.sh`; never two VMs with the same hostpci; VM 300 = driver, 312 = ROM work |
| A wrong write wedges the card | every write preceded by a read or a contract; VM restart + FLR clears state (observed) |
| Session/attention decay | this file + `docs/nvidia-driver.md` §5.x are the memory; every claim cites a log line |
| Estimates are optimistic | the ladder states its assumptions; N0-N2 are deliberately driver-independent |

---

## 8. Resuming cold — checklist

1. `git log --oneline -12` on `nv/ada-driver`; the last commit message states the
   current obstacle in its own words.
2. Run `tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`; read the
   `[NV-FWSEC]`, `[NV-PROBE]`, `[NV-MAP]`, `[NV-GSP]` rows.
3. If FWSEC still reports the sentinel, run the next un-tried §2 experiment.
4. If a contract doc contradicts this file, the doc wins — fix this file in the
   same commit.
5. Keep the tier green: `UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` on
   `sarah@192.168.0.123` (513 tests today).
