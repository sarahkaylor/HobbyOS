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

| file | bytes | sha256 |
|---|---|---|
| `booter_load-535.113.01.bin` | 59 768 | `3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0` |
| `booter_unload-535.113.01.bin` | 39 544 | `09240c83595856a0fe70d2ab3ec7325455cd8adfc48ec7b47f6fced1e5431beb` |
| `bootloader-535.113.01.bin` | 20 588 | `076d2a0675131ad8a8de215c611ce4271289adcc03e2cf39995f9b29079e2136` |
| `gsp-535.113.01.bin` | 38 061 600 | `e30231ec0d317021770e16c81ae2c0553f728d0344248827582f78132c9447d4` |

Roles (from the names and from what each blob is): `booter_load` is the ucode
that runs on the GPU processor first and installs the real image;
`booter_unload` is its counterpart for the teardown path; `bootloader` is the
non-secure loader stage; `gsp-*.bin` is the RM itself. Fetch with
`tools/nv/fetch-firmware.sh` (pinned version, checksums verified), which stages
them under `third_party/nvfw/ga102/gsp/` (gitignored — 38 MB does not belong in
the repository).

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

**Bottom line**: a from-scratch driver can absolutely boot this card's GSP, but
only by matching NVIDIA's firmware interface exactly, and the fastest honest
route is a reference card running a known-good driver next to ours (VM 301),
comparing register and memory state stage by stage. Guessing the WPR2 offsets
or the bootargs layout and testing on the real card would burn hardware cycles
with no diagnosis.
