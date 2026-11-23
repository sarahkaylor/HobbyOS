# GSP firmware: what it is and what the `nv` driver must do with it

Status: **analysis only — no boot code landed.** This document exists so the
next session does not repeat the research, and so the claims in
`docs/nvidia-driver.md` §5 about the GSP gate are grounded in bytes rather than
in folklore. Written 2026-11-22.

Scope: NVIDIA AD102 (RTX 4090), driver package **535.113.01**, firmware taken
from the Linux firmware repository
(`nvidia/ga102/gsp/`). Every size and hash below was computed from the file on
disk; every structural claim says how it was checked. Anything not established
is in §5 ("unknowns") with the experiment that would settle it.

---

## 1. Why this is the gate

On Turing and later the GPU no longer exposes engines a driver can program
directly. Memory management, the channel scheduler, the copy engines, display
and 3D all live in **GSP-RM** — NVIDIA's Resource Manager firmware running on
the GPU's on-die processor. A driver that cannot boot GSP-RM cannot allocate
VRAM, cannot submit a pushbuffer, and cannot set a display mode. There is no
"just enough" partial init: `docs/nvidia-driver.md` §5.

The boot is a chain of trust, and each link is a signed blob:

```
host driver ──writes──► booter ucode (falcon) ──verifies+loads──► HS/RM image
     ▲                        │
     └──── RPC queues ◄───────┴──► GSP-RM runs, host talks to it
```

## 2. Blob inventory (535.113.01, AD102)

| directory | file | bytes | sha256 |
|---|---|---|---|
| `ad102` | `booter_load-535.113.01.bin` | 55 928 | `7d4ef3258da016f5208cfb1b348a9a1d69aaa13c2f8d4ee8c05d66bd05b45cd6` |
| `ad102` | `booter_unload-535.113.01.bin` | 39 800 | `d036dfee0545c36e2194bc555cb268a5e11a6f13b5f66a7dec40838efc6ad87e` |
| `ad102` | `bootloader-535.113.01.bin` | 32 876 | `044d3a21eff73eb12f22840e5f145d685727f40165b3f2d265cf664753e9ff9c` |
| `ga102` | `booter_load-535.113.01.bin` | 59 768 | `3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0` |
| `ga102` | `booter_unload-535.113.01.bin` | 39 544 | `09240c83595856a0fe70d2ab3ec7325455cd8adfc48ec7b47f6fced1e5431beb` |
| `ga102` | `bootloader-535.113.01.bin` | 20 588 | `076d2a0675131ad8a8de215c611ce4271289adcc03e2cf39995f9b29079e2136` |
| `ga102` | `gsp-535.113.01.bin` | 38 061 600 | `e30231ec0d317021770e16c81ae2c0553f728d0344248827582f78132c9447d4` |

The AD102 booter is a **different program** from the GA102 one — same version
string, different size and hash.  Only the RM image is shared.

Roles (from the names and from what each blob is): `booter_load` is the ucode
that runs on the GPU processor first and installs the real image;
`booter_unload` is its counterpart for the teardown path; `bootloader` is the
non-secure loader stage; `gsp-*.bin` is the RM itself.

**The blobs come from two directories, and that is not an accident.** Verified by
listing linux-firmware (2026-11-22):

| directory | `booter_load` | `booter_unload` | `bootloader` | `gsp-*.bin` |
|---|---|---|---|---|
| `nvidia/ga102/gsp/` | yes | yes | yes | **yes** (`gsp-535.113.01.bin`, `gsp-570.144.bin`) |
| `nvidia/ad102/gsp/` | yes | yes | yes | **no** |

AD102 has no RM image of its own in this pin: it uses the GA102 one, which
carries signature sections for both families (§3.3).  A fetch script that only
looks in `nvidia/ad102/gsp/` finds the booter and then fails at the one file that
matters.  Note also that **570.144** is published for both directories — that is
the version a fresh attempt should pin (535.113.01 is what is analysed here
because it is the version the reference driver in this generation's table names).

Fetch with `tools/nv/fetch-firmware.sh` (pinned version, checksums verified),
which stages them under `third_party/nvfw/` (gitignored — 38 MB does not belong
in the repository).

## 3. Container formats, as measured

### 3.1 `booter_load-535.113.01.bin` — vendor-tagged container

First 24 bytes, little-endian, decoded with `struct.unpack("<6I")`:

| offset | value | reading |
|---|---|---|
| 0x00 | `0x000010DE` | **NVIDIA vendor id** as the container magic (the same 0x10DE the PCI layer matches on) |
| 0x04 | `0x00000001` | container version |
| 0x08 | `0x0000EA00` = 59 904 | declared binary size (file is 59 768 — the header's own size is not counted) |
| 0x0C | `0x00000018` = 24 | header size, i.e. the payload starts right after these six words |
| 0x10 | `0x00000378` = 888 | data offset |
| 0x14 | `0x0000E600` = 58 880 | data size |

Immediately after the container header sit the **heavy-secure descriptors** the
booter uses to authenticate what it is about to load (offsets verified by
walking the bytes at `hdr_off + …`):

| field | offset | value |
|---|---|---|
| production signature start | 0x3C | 0x300 bytes |
| patch location | 0x33C | — |
| patch signature | 0x340 | — |
| meta (device-mask) | 0x344 | 0xC bytes |
| signature count | 0x350 | — |
| load header | 0x354 | 0x24 bytes |

What this means for a from-scratch driver: the *host* does not verify these —
the booter does, in secure mode, against keys fused into the GPU. The host's
job is to place the blob where the booter expects it and start it.

### 3.2 `gsp-535.113.01.bin` — RISC-V relocatable ELF

The first bytes are `7f 45 4c 46` — an ELF. Decoding the ELF64 header:

| field | value | reading |
|---|---|---|
| `e_ident` | `7f454c4602010100…` | ELF, 64-bit, little-endian, version 1 |
| `e_type` | 1 | **ET_REL** — relocatable, not an executable |
| `e_machine` | 0xF3 (243) | **EM_RISCV** — the GSP on this generation is a RISC-V core |
| `e_phoff` / `e_phnum` | 0 / 0 | no program headers: there is nothing to map, the image must be *relocated* |
| `e_shoff` | 0x0244C3A0 | section header table |
| `e_ehsize` | 64 | standard ELF64 header |

The ASCII string `535.113.01` appears at file offset `0xC54E90` (the RM version
baked into its data). Total file 38 061 600 bytes.

Consequences, and they are the reason this stage is a project rather than a
patch:

1. **Relocation is required.** An ET_REL image cannot be copied into memory and
   jumped to; something must process its sections and relocations. In this
   design that something is the booter (NVIDIA's own signed loader), which is
   why the host does not need a RISC-V linker — it needs to hand the booter the
   image plus a description of where it went.
2. **The image is 38 MB.** It does not fit in any falcon IMEM/DMEM; it lives in
   VRAM and the booter DMAs from there. This is why the write-protected region
   (WPR2) exists and why the driver needs a working VRAM aperture before it can
   even start.
3. **The 32 KiB falcon windows the driver already probes are the front end, not
   the RM.** `nv` reads `UC_CAPS2` = `0xCA101136` (version 6) at BAR0+0x110000
   on the real card; that is the processor's control port, and it is the only
   part of the GSP the host touches directly.

### 3.3 The RM image's sections — and why AD102 borrows GA102's

The ELF has ten sections; three matter:

| section | offset | bytes | meaning |
|---|---|---|---|
| `.fwimage` | 0x40 | 38 043 648 (0x2448000) | the RM code+data image |
| `.fwversion` | 0x2448040 | 11 | `"535.113.01"` |
| `.fwsignature_ga10x` | 0x244B04B | 4096 | signature for GA10x |
| `.fwsignature_ad10x` | 0x244A04B | 4096 | signature for **AD10x** |

Both signatures are present in the one file, which is exactly how a GA102 RM
image serves an AD102 part: the bootloader picks the section matching the
fused engine id.  That also tells the driver what it must *not* try to do — §5
item 5.

## 4. The sequence the driver must implement

Each step names the observable that proves it happened. This is the shape of
`nv_gsp.c` (not written yet); the driver's existing stages (PMC, PTIMER,
falcon probe) are the first two rows.

| # | step | register block / memory | observable |
|---|---|---|---|
| 1 | PMC enabled, register window live | BAR0 + 0x0, 0x200 | `[NV] id …`, `[NV] pmc …` — **done** |
| 2 | GSP processor found and stopped | BAR0 + 0x110000 (`UC_CTRL.STOPPED`) | `[NV] falcon gsp … ctl=0x10 present` — **done** |
| 3 | VRAM aperture usable | BAR1 (32 GiB ReBAR measured; driver maps 1 GiB) | `[NV] bar1 window mapped` — **done** (window only) |
| 4 | firmware fetch + verify | host filesystem (`/NVIDIA/GSP/*.BIN`) | driver logs version + sha256 match |
| 5 | WPR2 region reserved | PFB MMU registers (WPR2 base/size, lock) | the region reads back as programmed |
| 6 | image + heap + boot placed in WPR2 | VRAM inside WPR2 | write/read-back of the first page |
| 7 | booter loaded into the processor | falcon IMEM via the DMA ("xfer") engine, which `nv_falcon_xfer()` already encodes | `XFER_STATUS` idle, IMEM read-back matches |
| 8 | booter started | `UC_ENTRY` + `UC_CTRL.START` | `UC_CTRL.STOPPED` clears |
| 9 | booter authenticates + loads HS/RM | (inside the secure processor) | falcon mailbox / boot status moves off its initial value |
| 10 | host/RM handshake | the RPC queues in the shared heap | RM answers the first message |

Steps 1–3 and the *machinery* for 7 exist in `nv.c`/`nv_falcon.c` today.
Steps 5–10 do not.

## 5. Unknowns — read this before writing `nv_gsp.c`

Each of these is a task with an experiment, not a fact:

1. **The WPR2 register set on AD102.** The window base/size registers and the
   lock sequence are documented for earlier generations in public register
   references; AD102's exact offsets and the "lock" handshake are **not**
   confirmed here. *Experiment:* dump the register range a known-good driver
   programs on the reference Linux VM with the same card (VM 301 in the harness
   plan) and diff it against our dump at the same stage. A register map copied
   from a driver that does not match the silicon is worse than no map.
2. **Which processor actually executes `booter_load`.** The GSP window reports a
   falcon version 6 *and* the RM image is RISC-V. The falcon front end most
   likely boots the RISC-V core with the fwsec/booter path, but the split
   (what runs on the falcon vs on the RISC-V core) is unconfirmed here. This
   changes how the image is placed: *Experiment:* read the RISC-V-specific
   register block around the GSP window on the reference card while its driver
   boots, or read the fwsec section headers of `booter_load` (§3.1).
3. **The bootargs structure.** Its field layout must match the firmware build
   exactly (version-tagged). Public firmware-interface headers define it for
   the pinned version; the values AD102 needs (WPR2 address/size, heap, RM
   args) are the unknowns. *Experiment:* build the struct from the pinned
   header, then compare the first 64 bytes the reference driver writes with
   ours.
4. **The RPC queue layout and first message.** Queue location, element size and
   the first command's function id are version-specific. *Experiment:* the
   reference VM's GSP boot can be traced (register dump + the RM debug log the
   firmware prints on the UART-less debug path if enabled), or the layout can be
   taken from the pinned firmware's interface headers.
5. **Whether the booter needs a host-provided signature.** The descriptors in
   §3.1 are the *booter's own* signature over what it loads; that part is
   NVIDIA's and is present in the blob. What is unproven is whether the host
   must also contribute a signed "load header" for the VRAM region — the
   `loadhdr` field at 0x354 suggests the booter reads one from the image, not
   from the host. *Experiment:* the same reference-card diff.
6. **Failure observability.** When the booter rejects an image it does not
   necessarily fault the host visibly; the falcon mailbox / status register is
   the only witness. The driver must poll and log it at every step (it already
   has the mailbox registers defined) — otherwise a wrong WPR2 offset looks
   exactly like a hang.

## 6. What the reference driver does, in register terms

Established from the reference driver's own device headers and boot code during
the research pass.  **Superseded in part by measurement**: a reference Linux
guest with NVIDIA's driver on the same card produced a pre/post BAR0 dump
(`docs/nv-gsp-reference.md`), and three rows below were contradicted by it.  The
table is kept as the record of what was believed, with §6.1 holding the verdict —
do not build on a row marked CONTRADICTED:

| item | value |
|---|---|
| GSP processor block | BAR0 + **0x110000**; mailboxes at **+0x040 / +0x044** (envytools calls these offsets SCRATCH0/1 — see the note in `nv_regs.h`), engine reset at +0x3C0, per-queue head registers at +0xC00 |
| RISC-V core window | BAR0 + **0x111000**; `CPUCTL` +0x388 bit 7 = ACTIVE, boot ROM parameters at +0x668 block |
| Falcon v4 control | `CPUCTL` 0x100, `BOOTVEC` 0x104, `DMACTL` 0x10C, `DMATRFBASE` 0x110/0x128, `DMATRFCMD` 0x118 (idle = bit 1), `HWCFG2` 0xF4 bit 12 = scrubbing |
| post-reset mask write | `0x1668 = 0x111` |
| WPR2 (live check) | lo/hi at **0x1FA824 / 0x1FA828**; nonzero hi means the firmware set it up |
| WPR meta placement | 256-byte `GspFwWprMeta`, magic `0xDC3AAE21371A60B3`, revision 1, at a 128 KiB-aligned base computed top-down from the end of VRAM |
| first RPC exchange | queue head kick via `PGSP_QUEUE_HEAD(0)`; pre-queued `GSP_SET_SYSTEM_INFO` then `SET_REGISTRY`, then wait for the `GSP_INIT_DONE` event |
| RPC frame | 32-byte header: version `0x03000000`, signature `"VRPC"`, length, function, result fields; queue elements carry auth tags, so the region must be exactly as the firmware expects |

### 6.1 Measured verdicts (2026-11-23)

Source: `docs/nv-gsp-reference.md` §5, driver 580.178.04 on this 4090, plus
`nv`'s own probe on the same card (`[NV-GSP] probe` rows in the serial log).

| row | verdict | what is actually true |
|---|---|---|
| GSP block `0x110000`, `UC_CAPS/CAPS2/CTRL` | **CONFIRMED** | `0x110108=0x80420100`, `0x11012C=0xCA101136`, `0x110100=0x10`; identical pre/post init and identical to `nv`'s log |
| falcon v4 control offsets (`0x100`, `0x104`, `0x10C`, `0x110`, `0x128`, `0x118`, `0xF4`) | **CONFIRMED as addresses** | every one answers coherently (`0x118=0x2` idle). Bit *semantics* beyond addresses remain unproven by static reads |
| RISC-V `CPUCTL +0x388` **bit 7 = ACTIVE** | **CONTRADICTED** | with GSP-RM running and Vulkan rendering, `0x111388 = 0x00000010` — **bit 4**, matching falcon `UC_CTRL.STOPPED`. Do not gate on bit 7 |
| `0x1668 = 0x111` post-reset mask | **CONTRADICTED as a BAR0 address** | `0x001668` reads the unreadable-register word in *both* states, and so does `nv` on a fenced window. It is block-relative or privileged; it cannot be written at BAR0+`0x1668` |
| WPR2 "live check" lo/hi `0x1FA824/0x1FA828`, nonzero hi | **CONTRADICTED** | hi is `0` with GSP-RM running, and `0` before init: the word never moves. The pair that *moves* is `0x1FA804`/`0x1FA808` (`0x041B3000`/`0x05CF3000` pre → `0x1FFFF000`/`0` post) — but read as base/size that describes a 4 KiB region, so it is probably not WPR2 either. Which registers are WPR2 on AD102 is **open** |
| WPR meta 256 B, magic, revision, 128 KiB top-down | not read | lives in VRAM; BAR1 was not mapped by the reference lane. Still a hypothesis (see §5) |
| mailboxes `+0x040/+0x044`, engine reset `+0x3C0`, queue head `+0xC00` | **UNTESTABLE** | all read 0 in both states — a static dump cannot distinguish "idle" from "not implemented" |

Two consequences the driver already carries:

- **`0xBADF_xxxx` is not one thing.** `0xBADF5040` is the chip's answer for a
  register this host may not read and it *persists on a healthy card*;
  `0xBADF5620` is a fenced window that a driver has not initialised yet.  `nv`
  used to call both "poison" and print a working card as if it were broken.
- **"Fenced" is not "absent".** SEC2 reads the fence word before init and
  falcon-shaped afterwards, so `nv` now reports `fenced` for a BADF window and
  keeps `absent` for `0xffffffff`.

Cross-check, and the reason to trust these rows: `nv`'s own probe on the same
card reproduces the reference's *pre-init* numbers exactly — `wpr a=0x041B3000
b=0x05CF3000 mmu=0x00100000:0x05CF3FF0`, `mask1668=fenced`,
`riscv_cpuctl=fenced` — after the reference guest had run GSP-RM and been
stopped. Two independent software stacks, same words. It also means the
write-protected region is torn down on VM stop/unload, so the normal case for
`nv` is "untouched", not "already programmed".

## 7. Bottom line

A from-scratch driver can absolutely boot this card's GSP, but only by matching
NVIDIA's firmware interface exactly.  The chain is vendor-signed end to end:
the falcon boot ROM validates an RSA-3072 signature over the booter's patch site
against fused keys, and the bootloader validates the RM image's
`.fwsignature_ad10x` against a key in its ROM.  **There is no host-side
signature to forge and none to skip** — the driver's job is to be a correct
*loader* (place bytes, program registers, start, read the mailbox), never an
authenticator.  That is good news: it means every step is observable, and a
mistake shows up as a mailbox value or a stuck bit rather than a silent hang.

The fastest honest route to a green P3 is therefore a reference card running a
known-good driver next to ours (VM 301 in the harness plan), diffing register
and memory state stage by stage.  Guessing the WPR2 layout or the bootargs
encoding and testing on the real card burns hardware cycles with no diagnosis.
