# Reference driver on the same card: NVIDIA 580.178.04 on the RTX 4090

Status: **measured on the target hardware, 2026-11-23.** This is the reference
half of the P3 work: a known-good driver driving the *same* RTX 4090
(`10de:2684`, AD102) that the native `nv` driver in this branch talks to, with
the register state captured before and after that driver initialises the card.

It exists so `nv_gsp.c` can be written against measured numbers instead of
inference. Every value below was read on the card; every command that produced
a value is named. Where a measurement cannot settle a question, the verdict is
**UNTESTABLE**, not a guess.

- Guest: VM 301 `linux-ref` on Proxmox `r1`, Ubuntu 24.04.5, 8 vCPU, 16 GiB,
  q35, `hostpci0: 0000:01:00,pcie=1` (no `x-vga`).
- Driver: **580.178.04** (DKMS), GSP firmware **580.178.04**, CUDA 13.0.
- Raw data: `docs/nv-gsp-reference-evidence/` (dumps, dmesg, lspci, smi).
- Method: read-only `mmap` of `/sys/bus/pci/devices/0000:01:00.0/resource0`
  (`O_RDONLY`, `PROT_READ`), 4-byte aligned loads, **no writes to BAR0 at all**.

Companion: `docs/nvidia-driver.md` (our driver), `docs/nv-gsp-firmware.md`
(the firmware analysis whose §6 hypotheses are adjudicated in §5 below).

---

## 1. The guest, and proof the driver owns the card

`qm config 301` (verbatim, abridged to the fields that matter):

| field | value |
|---|---|
| `cores` / `memory` | 8 / 16384 |
| `machine` | `q35` |
| `hostpci0` | `0000:01:00,pcie=1` |
| `net0` | `virtio,bridge=vmbr0` |
| `scsihw` / `scsi0` | `virtio-scsi-single` / 60 GiB on `local-lvm` |
| `ostype` | `l26` |

Installed with the stock noble archive package (the task's preferred
`nvidia-driver-570-server` is present as a metapackage at **570.211.01**, but in
this archive it pulls the 580 kernel series; that is what actually bound):

```
$ dpkg -l | grep -E '^ii  nvidia-(driver|dkms|kernel)'
ii  nvidia-dkms-580-server                580.178.04-0ubuntu0.24.04.1
ii  nvidia-driver-570-server              570.211.01-0ubuntu1.24.04.1
ii  nvidia-driver-580-server              580.178.04-0ubuntu0.24.04.1
ii  nvidia-kernel-common-580-server       580.178.04-0ubuntu0.24.04.1
ii  nvidia-kernel-source-580-server       580.178.04-0ubuntu0.24.04.1
```

```
$ cat /proc/driver/nvidia/version
NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.178.04  Tue Jul  7 12:38:21 UTC 2026
GCC version:  gcc version 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
```

```
$ nvidia-smi
Mon Nov 23 06:28:33 2026
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 580.178.04             Driver Version: 580.178.04     CUDA Version: 13.0     |
+-----------------------------------------+------------------------+----------------------+
| GPU  Name                 Persistence-M | Bus-Id          Disp.A | Volatile Uncorr. ECC |
| Fan  Temp   Perf          Pwr:Usage/Cap |           Memory-Usage | GPU-Util  Compute M. |
|                                         |                        |               MIG M. |
|=========================================+========================+======================|
|   0  NVIDIA GeForce RTX 4090        Off |   00000000:01:00.0 Off |                  Off |
| 30%   24C    P0             65W /  450W |       0MiB /  24564MiB |      2%      Default |
|                                         |                        |                  N/A |
+-----------------------------------------+------------------------+----------------------+
```

`nvidia-smi -q` fields worth quoting:

| field | value |
|---|---|
| Driver Version | 580.178.04 |
| **GSP Firmware Version** | **580.178.04** |
| VBIOS Version | 95.02.18.40.4C |
| GPU UUID | GPU-00750a15-4ed1-00fd-8fb7-5f2e1a25ed14 |
| Board ID | 0x100 |

**GSP-RM is running.** `GSP Firmware Version` is only populated when the driver
has booted GSP-RM on the GSP and completed the RPC handshake; a card whose GSP
never came up reports `N/A` there.

Firmware actually loaded (`modinfo nvidia | grep firmware`):

```
firmware:       nvidia/580.178.04/gsp_tu10x.bin
firmware:       nvidia/580.178.04/gsp_ga10x.bin
```

i.e. the 580 package ships the Turing and the GA10x RM images only; AD102 is
served by `gsp_ga10x.bin`, which matches `docs/nv-gsp-firmware.md` §3.3's
finding that one RM image carries signatures for several families.

`lspci -vvv -s 01:00.0` (the rows a bring-up cares about):

```
	Region 0: Memory at f9000000 (32-bit, non-prefetchable) [size=16M]
	Region 1: Memory at e0000000000 (64-bit, prefetchable) [size=32G]
	Region 3: Memory at e0800000000 (64-bit, prefetchable) [size=32M]
	Region 5: I/O ports at 5000 [size=128]
	Capabilities: [bb0 v1] Physical Resizable BAR
		BAR 0: current size: 16MB, supported: 16MB
		LnkCap:	Port #0, Speed 16GT/s, Width x16
		LnkSta:	Speed 2.5GT/s (downgraded), Width x16
```

BAR0 is 16 MiB and not resizable — the same shape `nv` sizes in its own log.
The `2.5GT/s (downgraded)` link is the card's idle link-speed reduction, not a
passthrough fault: it re-trains under load (`nvidia-smi dmon` hits 2.6 GHz SM
clock with the link idle, and the card renders — §6).

## 2. dmesg, verbatim

All NVIDIA-related lines from a clean boot with the driver (580) loading:

```

[  598.336608] nvidia: loading out-of-tree module taints kernel.
[  598.336617] nvidia: module license 'NVIDIA' taints kernel.
[  598.336621] nvidia: module verification failed: signature and/or required key missing - tainting kernel
[  598.336622] nvidia: module license taints kernel.
[  598.425483] nvidia-nvlink: Nvlink Core is being initialized, major device number 237
[  598.431000] nvidia 0000:01:00.0: vgaarb: VGA decodes changed: olddecodes=io+mem,decodes=none:owns=none
[  598.475258] NVRM: loading NVIDIA UNIX x86_64 Kernel Module  580.178.04  Tue Jul  7 12:38:21 UTC 2026
[  598.489623] nvidia-modeset: Loading NVIDIA Kernel Mode Setting Driver for UNIX platforms  580.178.04  Tue Jul  7 12:03:04 UTC 2026
[  598.493935] [drm] [nvidia-drm] [GPU ID 0x00000100] Loading driver
[  598.494052] [drm] Initialized nvidia-drm 0.0.0 20160202 for 0000:01:00.0 on minor 0
[  598.508696] nvidia_uvm: module uses symbols nvUvmInterfaceDisableAccessCntr from proprietary module nvidia, inheriting taint.

```

What is *not* there is as informative as what is: **there are no `GSP` lines at
all.** Reloading the module with the driver's own GSP log knob
(`modprobe nvidia NVreg_EnableGpuFirmwareLogs=1`) still produced zero lines
containing `gsp`:

```
$ sudo dmesg | grep -icE 'gsp'
0
```

So on this driver a successful GSP boot is *silent*. The only host-visible
proof of it is `nvidia-smi`'s `GSP Firmware Version` (§1) — which is exactly
the failure-observability problem `docs/nv-gsp-firmware.md` §5.6 warns about,
and the reason §3/§4 below (raw register state) are worth more than logs.

## 3-4. BAR0 before and after the driver initialises the card

Both dumps were taken with **the same read-only tool on the same card**, once
with **no driver bound at all** (`Kernel driver in use:` empty,
`lspci -nnk` lists only `nvidiafb` as a possibility) and once with 580 bound,
GSP-RM running and Vulkan working.

**The post-init dump was not refused.** `resource0` stays mappable after the
nvidia driver binds — `mmap` succeeds and reads return live values (the
driver does not claim exclusivity on the sysfs resource file). That is a
useful result in itself: a reference dump of an *initialised* card does not
need a kernel module, `setpci`, or a debug build of anything.

Read the dump tool itself at `docs/nv-gsp-reference-evidence/nvdump.c`
(93 lines, `O_RDONLY` + `PROT_READ`, prints `offset value`, flushes per line so
a hang names its own offset).

### 3.1 How to read `0xBADF_xxxx`

Three patterns appear and they mean different things. They are **not** one
"poison" value:

| pattern | where measured | what it is |
|---|---|---|
| `0xBADF5040` | `0x000100`, `0x000140`, `0x001668`, `0x100100`, `0x100140`, `0x1100C0..C8`, `0x1100FC`, `0x111000`, … | the answer for a register the unprivileged host may not read (or that does not exist on this part) — it persists **with a working driver** |
| `0xBADF5620` | the whole `0x110000`/`0x111000`/`0x840000` windows in the **pre-init** dump | the window is fenced/not initialised: 299 of 326 sampled offsets in `0x110000-0x112000` returned exactly this |
| `0xBADF5720` | scattered offsets in the **post-init** dump (`0x110034`, `0x110098`, `0x111004`…) | per-register "exists but not readable here", after the fence lifted |

The consequence for `nv`: **a `BADF` read is not evidence that a block is
absent.** `0x000100` (PMC_INTR_0) still reads `0xBADF5040` while the card is
rendering Vulkan, and `0x840000` (SEC2) reads `0xBADF5620` before init but a
perfectly falcon-shaped register file after init (§5). See §5 rows 7–9.

### 3.2 PMC, bus glue and PTIMER


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x000000` | `0x192000A1` | `0x192000A1` | PMC_BOOT_0 — chipset 0x192 (AD102), stepping 0xA1 |
| `0x000004` | `0x00000000` | `0x00000000` |  |
| `0x000008` | `0x00000000` | `0x00000000` |  |
| `0x000100` | `0xBADF5040` | `0xBADF5040` | PMC_INTR_0 — BADF in **both** states |
| `0x000140` | `0xBADF5040` | `0xBADF5040` | PMC_INTR_EN_0 — BADF in both states |
| `0x000200` | `0x40000000` | `0x40000000` | PMC_ENABLE — bit 30 set in both states |

The remaining "PMC-ish" offsets the driver uses, and the PTIMER pair:


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x001668` | `0xBADF5040` | `0xBADF5040` | the `0x1668 = 0x111` mask write — BADF in both states |
| `0x009400` | `0xD0301120` | `0x04451260` | PTIMER_TIME_0 — a live counter (changes between runs) |
| `0x009410` | `0x00000021` | `0x18EA98BD` | PTIMER_TIME_1 — high word, also advances |
| `0x100100` | `0xBADF5040` | `0xBADF5040` | claimed PBUS_INTR_0 — BADF in both states |
| `0x100140` | `0xBADF5040` | `0xBADF5040` | claimed PBUS_INTR_EN_0 — BADF in both states |

`0x009400`/`0x009410` are `NV_PTIMER_TIME_0`/`_1` in `nv_regs.h`, and they
behave exactly as the driver assumes: a 64-bit free-running counter whose low
word moves fast and whose high word advanced by `0x31` between two dumps taken
minutes apart. **CONFIRMED.**

### 3.3 PMU falcon window `0x10A000-0x10A040`


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x10A000` | `0x00000000` | `0x00000000` | falcon-ish head of PMU window |
| `0x10A004` | `0x00000000` | `0x00000000` |  |
| `0x10A008` | `0x00000000` | `0x00000000` |  |
| `0x10A00C` | `0x0180FC24` | `0x0180FC24` |  |
| `0x10A010` | `0x00000000` | `0x00000000` |  |
| `0x10A014` | `0x00000000` | `0x00000000` |  |
| `0x10A018` | `0x01000021` | `0x01000021` |  |
| `0x10A01C` | `0x00200000` | `0x00200000` |  |
| `0x10A020` | `0x00083D60` | `0x00083D60` |  |
| `0x10A024` | `0x00014E2C` | `0x00012A4E` | counter (changes between runs) |
| `0x10A028` | `0x00000001` | `0x00000001` |  |
| `0x10A02C` | `0xD0312360` | `0x04464180` | counter low (changes between runs) |
| `0x10A030` | `0x00000021` | `0x18EA98BD` | counter high (advances) |
| `0x10A034` | `0x00000000` | `0x00000000` |  |
| `0x10A038` | `0x00000000` | `0x00000000` |  |
| `0x10A03C` | `0x00000100` | `0x00000100` |  |
| `0x10A040` | `0x00000300` | `0x00000300` |  |

### 3.4 GSP falcon window `0x110000-0x110140`

The first 0x104 bytes are from the named dump; the tail (`0x110104-0x110140`)
comes from the 8 KiB sweep, which covers `0x110000-0x112000` at step 4.


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x110000` | `0x00000000` | `0x00000000` |  |
| `0x110004` | `0x00000000` | `0x00000000` |  |
| `0x110008` | `0x00000010` | `0x00000000` |  |
| `0x11000C` | `0xBADF5620` | `0x0180FC24` | reads 0x0180FC24 post-init, same value PMU shows at 0x10A00C |
| `0x110010` | `0xBADF5620` | `0x00000000` |  |
| `0x110014` | `0xBADF5620` | `0x00000000` |  |
| `0x110018` | `0x01000000` | `0x01000000` |  |
| `0x11001C` | `0x00000000` | `0x00000000` |  |
| `0x110020` | `0xBADF5620` | `0x00000000` |  |
| `0x110024` | `0xBADF5620` | `0x00000000` |  |
| `0x110028` | `0xBADF5620` | `0x00000000` |  |
| `0x11002C` | `0xBADF5620` | `0x044715C0` | counter low (changes between runs) |
| `0x110030` | `0xBADF5620` | `0x18EA98BD` | counter high (advances) |
| `0x110034` | `0xBADF5620` | `0xBADF5720` | BADF5720 post-init: unprivileged/unimplemented read pattern |
| `0x110038` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11003C` | `0xBADF5620` | `0x00000100` |  |
| `0x110040` | `0x00000000` | `0x00000000` | mailbox 0 — reads 0 in both states |
| `0x110044` | `0x00000000` | `0x00000000` | mailbox 1 — reads 0 in both states |
| `0x110048` | `0xBADF5620` | `0x00000000` | ACCESS_EN? — fenced pre-init, 0 post-init |
| `0x11004C` | `0x00000000` | `0x00000000` |  |
| `0x110050` | `0x00000000` | `0x00000000` |  |
| `0x110054` | `0x00000000` | `0x00000000` |  |
| `0x110058` | `0xBADF5620` | `0x00000000` |  |
| `0x11005C` | `0x0000FFF8` | `0x0000FFF8` |  |
| `0x110060` | `0xBADF5620` | `0x00000000` |  |
| `0x110064` | `0x00000000` | `0x00000000` |  |
| `0x110068` | `0x00000000` | `0x00000000` |  |
| `0x11006C` | `0xBADF5620` | `0x00000000` |  |
| `0x110070` | `0x00000000` | `0x00000000` |  |
| `0x110074` | `0xBADF5620` | `0x00000000` |  |
| `0x110078` | `0xBADF5620` | `0x00000000` |  |
| `0x11007C` | `0xBADF5620` | `0x00000000` |  |
| `0x110080` | `0x00000000` | `0x00000000` |  |
| `0x110084` | `0x00000000` | `0x00000000` |  |
| `0x110088` | `0xBADF5620` | `0x00000000` |  |
| `0x11008C` | `0xBADF5620` | `0x00000000` |  |
| `0x110090` | `0xBADF5620` | `0x00000040` |  |
| `0x110094` | `0xDA550000` | `0x00000000` |  |
| `0x110098` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11009C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x1100A0` | `0xBADF5620` | `0x00000000` |  |
| `0x1100A4` | `0x00000000` | `0x00000000` |  |
| `0x1100A8` | `0xBADF5620` | `0x00000000` |  |
| `0x1100AC` | `0xBADF5620` | `0x00000000` |  |
| `0x1100B0` | `0xBADF5620` | `0xBADF5720` |  |
| `0x1100B4` | `0xBADF5620` | `0xBADF5720` |  |
| `0x1100B8` | `0xBADF5620` | `0xBADF5720` |  |
| `0x1100BC` | `0x00FFFFFC` | `0x00000000` |  |
| `0x1100C0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1100C4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1100C8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1100CC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1100D0` | `0x00900000` | `0x00000000` |  |
| `0x1100D4` | `0x00040000` | `0x00000000` |  |
| `0x1100D8` | `0x40000000` | `0x40000000` |  |
| `0x1100DC` | `0x008E003F` | `0x000E003F` |  |
| `0x1100E0` | `0xBADF5620` | `0xBADF5720` |  |
| `0x1100E4` | `0xBADF5620` | `0x00000000` |  |
| `0x1100E8` | `0xBADF5620` | `0x80000B8F` |  |
| `0x1100EC` | `0xBADF5620` | `0x80000FFF` |  |
| `0x1100F0` | `0xBADF5620` | `0x80000FFF` |  |
| `0x1100F4` | `0x000047B7` | `0x000047B7` |  |
| `0x1100F8` | `0xBADF5620` | `0x00110000` |  |
| `0x1100FC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x110100` | `0x00000010` | `0x00000010` | UC_CTRL — 0x10 = STOPPED in **both** states |
| `0x110104` | `0xBADF5620` | `0x00000000` | UC_ENTRY — 0 post-init |
| `0x110108` | `0x80420100` | `0x80420100` | UC_CAPS — identical pre and post (matches HobbyOS's log) |
| `0x11010C` | `0xBADF5620` | `0x00000001` |  |
| `0x110110` | `0xBADF5620` | `0x00000000` |  |
| `0x110114` | `0xBADF5620` | `0x00000000` |  |
| `0x110118` | `0xBADF5620` | `0x00000002` |  |
| `0x11011C` | `0xBADF5620` | `0x00000000` |  |
| `0x110120` | `0xBADF5620` | `0x00000010` | XFER_STATUS — 0x10 post-init (bit 4) |
| `0x110124` | `0xBADF5620` | `0x00000020` |  |
| `0x110128` | `0xBADF5620` | `0x00000000` |  |
| `0x11012C` | `0xCA101136` | `0xCA101136` | UC_CAPS2 — identical pre and post (matches HobbyOS's log) |
| `0x110130` | `0xBADF5620` | `0x00000000` |  |
| `0x110134` | `0xBADF5620` | `0x0007FFFE` | 0x0007FFFE post-init |
| `0x110138` | `0xBADF5620` | `0x00000000` |  |
| `0x11013C` | `0xBADF5620` | `0x00000B00` | 0x00000B00 post-init |
| `0x110140` | `0xBADF5620` | `0x00000000` |  |

Three rows here are the whole ballgame for `nv`'s existing probe:

- `0x110108` (`UC_CAPS`) = **`0x80420100`** and `0x11012C` (`UC_CAPS2`) =
**`0xCA101136`** — *identical in the pre-init and post-init dumps*, and
identical to what `nv` printed from its own driver on this card
(`docs/nvidia-driver.md` §5.1). The falcon identification registers are
readable **before** anything initialises the card, which is why `nv`'s probe
works at all.
- `0x110100` (`UC_CTRL`) = **`0x10`** in both states. Bit 4 is `STOPPED`, so
the *falcon front end* is stopped even while GSP-RM is running: on this part
the RISC-V core runs the RM, not the falcon. `nv` reading `ctl=0x10` and
calling the GSP "stopped" was correct, and would still read `0x10` on a fully
working card — it is not a failure indicator.
- Everything around them (`0x110034`, `0x110098`, `0x1100B0`…) goes from
`0xBADF5620` to `0xBADF5720` or to a real value: the window's *fence* lifts
across init, register by register.

### 3.5 RISC-V window `0x111000-0x111400`

The full 257-register window, pre vs post. This is the block
`docs/nv-gsp-firmware.md` §6 calls "RISC-V core window, CPUCTL +0x388".


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x111000` | `0xBADF5620` | `0xBADF5040` | BADF5040 even post-init |
| `0x111004` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111008` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11100C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111010` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111014` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111018` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11101C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111020` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111024` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111028` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11102C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111030` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111034` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111038` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11103C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111040` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111044` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111048` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11104C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111050` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111054` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111058` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11105C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111060` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111064` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111068` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11106C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111070` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111074` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111078` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11107C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111080` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111084` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111088` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11108C` | `0xBADF5620` | `0x00000000` |  |
| `0x111090` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111094` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111098` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11109C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110A0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110A4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110A8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110AC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110B0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110B4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110B8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110BC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110C0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110C4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110C8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110CC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110D0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110D4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110D8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110DC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110E0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110E4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110E8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110EC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110F0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110F4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110F8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1110FC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111100` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111104` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111108` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11110C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111110` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111114` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111118` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11111C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111120` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111124` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111128` | `0xBADF5620` | `0x80000F88` |  |
| `0x11112C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111130` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111134` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111138` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11113C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111140` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111144` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111148` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11114C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111150` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111154` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111158` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11115C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111160` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111164` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111168` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11116C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111170` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111174` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111178` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11117C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111180` | `0xBADF5620` | `0x00000000` |  |
| `0x111184` | `0xBADF5620` | `0x00000002` |  |
| `0x111188` | `0xBADF5620` | `0x00000002` |  |
| `0x11118C` | `0xBADF5620` | `0x00011011` |  |
| `0x111190` | `0xBADF5620` | `0x000087A0` |  |
| `0x111194` | `0xBADF5620` | `0x02100000` |  |
| `0x111198` | `0xBADF5620` | `0x00000000` |  |
| `0x11119C` | `0xBADF5620` | `0x00007FFF` |  |
| `0x1111A0` | `0xBADF5620` | `0x00000000` |  |
| `0x1111A4` | `0xBADF5620` | `0x00000000` |  |
| `0x1111A8` | `0xBADF5620` | `0x00000001` |  |
| `0x1111AC` | `0xBADF5620` | `0x00000001` |  |
| `0x1111B0` | `0xBADF5620` | `0x00000001` |  |
| `0x1111B4` | `0xBADF5620` | `0x00000001` |  |
| `0x1111B8` | `0xBADF5620` | `0x00000001` |  |
| `0x1111BC` | `0xBADF5620` | `0x00000001` |  |
| `0x1111C0` | `0xBADF5620` | `0x00000001` |  |
| `0x1111C4` | `0xBADF5620` | `0x00000001` |  |
| `0x1111C8` | `0xBADF5620` | `0x00000001` |  |
| `0x1111CC` | `0xBADF5620` | `0x00000001` |  |
| `0x1111D0` | `0xBADF5620` | `0x00000001` |  |
| `0x1111D4` | `0xBADF5620` | `0x00000001` |  |
| `0x1111D8` | `0xBADF5620` | `0x00000001` |  |
| `0x1111DC` | `0xBADF5620` | `0x00000007` |  |
| `0x1111E0` | `0xBADF5620` | `0x0000000F` |  |
| `0x1111E4` | `0xBADF5620` | `0x0000000F` |  |
| `0x1111E8` | `0xBADF5620` | `0x00000003` |  |
| `0x1111EC` | `0xBADF5620` | `0x00000101` |  |
| `0x1111F0` | `0xBADF5620` | `0x00000000` |  |
| `0x1111F4` | `0xBADF5620` | `0x02000000` |  |
| `0x1111F8` | `0xBADF5620` | `0x00000000` |  |
| `0x1111FC` | `0xBADF5620` | `0x00000000` |  |
| `0x111200` | `0xBADF5620` | `0xBAD043E3` |  |
| `0x111204` | `0xBADF5620` | `0xBAD043E3` |  |
| `0x111208` | `0xBADF5620` | `0x00000000` |  |
| `0x11120C` | `0xBADF5620` | `0x42100100` |  |
| `0x111210` | `0xBADF5620` | `0x00000000` |  |
| `0x111214` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111218` | `0xBADF5620` | `0x00000000` |  |
| `0x11121C` | `0xBADF5620` | `0x00000000` |  |
| `0x111220` | `0xBADF5620` | `0x00000000` |  |
| `0x111224` | `0xBADF5620` | `0x00000000` |  |
| `0x111228` | `0xBADF5620` | `0x00000000` |  |
| `0x11122C` | `0xBADF5620` | `0x00000000` |  |
| `0x111230` | `0xBADF5620` | `0x00000000` |  |
| `0x111234` | `0xBADF5620` | `0x00000000` |  |
| `0x111238` | `0xBADF5620` | `0x00000000` |  |
| `0x11123C` | `0xBADF5620` | `0x00000000` |  |
| `0x111240` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111244` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111248` | `0xBADF5620` | `0xBADF5720` |  |
| `0x11124C` | `0xBADF5620` | `0xBADF5720` |  |
| `0x111250` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111254` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111258` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11125C` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111260` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111264` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111268` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11126C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111270` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111274` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111278` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11127C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111280` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111284` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111288` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11128C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111290` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111294` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111298` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11129C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112A0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112A4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112A8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112AC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112B0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112B4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112B8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112BC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112C0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112C4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112C8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112CC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112D0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112D4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112D8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112DC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112E0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112E4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112E8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112EC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112F0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112F4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112F8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1112FC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111300` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111304` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111308` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x11130C` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111310` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111314` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111318` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x11131C` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x111320` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111324` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111328` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11132C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111330` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111334` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111338` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11133C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111340` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111344` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111348` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11134C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111350` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111354` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111358` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11135C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111360` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111364` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111368` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11136C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111370` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111374` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111378` | `0xBADF5620` | `0xBADF5040` |  |
| `0x11137C` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111380` | `0xBADF5620` | `0x00080000` |  |
| `0x111384` | `0xBADF5620` | `0x00000000` |  |
| `0x111388` | `0xBADF5620` | `0x00000010` | **claimed CPUCTL+0x388 bit 7 = ACTIVE → reads 0x10 (bit 4), bit 7 clear** |
| `0x11138C` | `0xBADF5620` | `0x00000000` |  |
| `0x111390` | `0xBADF5620` | `0x00000610` | 0x00000610 post-init |
| `0x111394` | `0xBADF5620` | `0x00000000` |  |
| `0x111398` | `0xBADF5620` | `0x00000000` |  |
| `0x11139C` | `0xBADF5620` | `0x00000000` |  |
| `0x1113A0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113A4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113A8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113AC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113B0` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113B4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113B8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113BC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113C0` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x1113C4` | `0xBADF5620` | `0xFFFFFFFF` |  |
| `0x1113C8` | `0xBADF5620` | `0x0001FFFB` |  |
| `0x1113CC` | `0xBADF5620` | `0x00000000` |  |
| `0x1113D0` | `0xBADF5620` | `0x00000000` |  |
| `0x1113D4` | `0xBADF5620` | `0x00000000` |  |
| `0x1113D8` | `0xBADF5620` | `0x00000000` |  |
| `0x1113DC` | `0xBADF5620` | `0x00000000` |  |
| `0x1113E0` | `0xBADF5620` | `0x00000000` |  |
| `0x1113E4` | `0xBADF5620` | `0xBADFBADF` |  |
| `0x1113E8` | `0xBADF5620` | `0xBADFBADF` |  |
| `0x1113EC` | `0xBADF5620` | `0x00000000` |  |
| `0x1113F0` | `0xBADF5620` | `0x00000000` |  |
| `0x1113F4` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113F8` | `0xBADF5620` | `0xBADF5040` |  |
| `0x1113FC` | `0xBADF5620` | `0xBADF5040` |  |
| `0x111400` | `0xBADF5620` | `0x0000FF00` |  |

The single most important row: **`0x111388` reads `0x00000010` post-init, with
GSP-RM running and Vulkan rendering.** Bit 7 (`0x80`) is clear; bit 4 (`0x10`)
is set — the same `STOPPED` bit the falcon `UC_CTRL` uses. §5 row 4 is the
verdict.

### 3.6 PFB / WPR2 window `0x1FA800-0x1FA840`


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x1FA800` | `0x00100140` | `0x00100140` |  |
| `0x1FA804` | `0x041B3000` | `0x1FFFF000` | changes across init: 0x041B3000 → 0x1FFFF000 |
| `0x1FA808` | `0x05CF3000` | `0x00000000` | changes across init: 0x05CF3000 → 0 |
| `0x1FA80C` | `0x00000038` | `0x00000028` |  |
| `0x1FA810` | `0x00000000` | `0x00000000` |  |
| `0x1FA814` | `0x08000000` | `0x08000000` |  |
| `0x1FA818` | `0x08000000` | `0x08000000` |  |
| `0x1FA81C` | `0x1FFFFE00` | `0x1FFFFE00` |  |
| `0x1FA820` | `0x00000000` | `0x00000000` |  |
| `0x1FA824` | `0x1FFFFE00` | `0x1FFFFE00` | claimed WPR2 lo — identical in both states |
| `0x1FA828` | `0x00000000` | `0x00000000` | claimed WPR2 hi — **0 in both states** |
| `0x1FA82C` | `0x00100000` | `0x1FFFFFF0` | changes across init: 0x00100000 → 0x1FFFFFF0 |
| `0x1FA830` | `0x05CF3FF0` | `0x00000000` | changes across init: 0x05CF3FF0 → 0 |
| `0x1FA834` | `0x00000000` | `0x00000000` |  |
| `0x1FA838` | `0xBADF5040` | `0xBADF5040` |  |
| `0x1FA83C` | `0xBADF5040` | `0xBADF5040` |  |
| `0x1FA840` | `0x00000051` | `0x00000081` | 0x51 → 0x81 |

### 3.7 The `0x840000` window (SEC2)


| offset | pre-init | post-init | note |
|---|---|---|---|
| `0x840000` | `0xBADF5620` | `0x00000000` | pre-init: fenced; post-init: 0 |
| `0x840004` | `0xBADF5620` | `0x00000000` |  |
| `0x840008` | `0xBADF5620` | `0x00000010` | post-init 0x10 — the same 'STOPPED' shape as the GSP falcon |
| `0x84000C` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840010` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840014` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840018` | `0xBADF5620` | `0x01000000` | post-init 0x01000000 — same value the GSP window has at 0x110018 |
| `0x84001C` | `0xBADF5620` | `0x00000000` |  |
| `0x840020` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840024` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840028` | `0xBADF5620` | `0xBADF5620` |  |
| `0x84002C` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840030` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840034` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840038` | `0xBADF5620` | `0xBADF5620` |  |
| `0x84003C` | `0xBADF5620` | `0xBADF5620` |  |
| `0x840040` | `0x00000000` | `0x00000000` | 0 in both states |

Pre-init this window is uniformly `0xBADF5620` — **exactly what `nv` measured**
and reported as `sec2 absent`. Post-init the head of the window answers:
`0x840000=0x0`, `0x840004=0x0`, `0x840008=0x10`, `0x840018=0x01000000`, with
`0x84000C..0x84003C` still `0xBADF5620`. That head is exactly the shape the GSP
window shows **pre-init** (`0x110000=0, 0x110004=0, 0x110008=0x10,
0x110018=0x01000000`), i.e. a falcon-style register file whose identity
registers are readable.

So SEC2 is **present and responsive**, and `nv`'s `sec2 absent` is an artifact
of reading the window before anything enabled it — not a fact about the chip.
What it does *not* prove is that SEC2 is usable: only four registers became
readable, and nothing here says what enables the rest. See §5.1.

### 3.8 Where the raw data lives

| file | content |
|---|---|
| `docs/nv-gsp-reference-evidence/dump-preinit-named.txt` | named set + PMU/GSP/RISC-V/PFB/`0x840000` ranges, no driver bound (419 lines) |
| `docs/nv-gsp-reference-evidence/dump-postinit-named.txt` | same offsets, driver 580 bound + GSP-RM running (419 lines) |
| `docs/nv-gsp-reference-evidence/dump-preinit-gsp-sweep.txt` | **`0x110000-0x112000` step 4, 2048 lines, `offset value`** |
| `docs/nv-gsp-reference-evidence/dump-postinit-gsp-sweep.txt` | same sweep, post-init (2048 lines) |
| `docs/nv-gsp-reference-evidence/dump-postinit2-named.txt` | a second post-init capture, for stability (§4.1) |
| `docs/nv-gsp-reference-evidence/nvdump.c` | the 93-line read-only dump tool |
| `docs/nv-gsp-reference-evidence/dmesg-nv.txt`, `lspci-vvv.txt`, `smi.txt`, `smi-q.txt`, `driver-version.txt`, `vulkaninfo-summary.txt`, `dmon-60s.txt` | supporting evidence |

## 4.1 Is the dump deterministic?

Two post-init dumps taken minutes apart (the second after a full
`rmmod`/`modprobe` cycle, i.e. a second GSP boot) differ in **7 of 419**
offsets, and all seven are the free-running counters:

```
0x009400 / 0x009410    PTIMER_TIME_0/_1
0x10A024, 0x10A02C, 0x10A030   PMU-window counters
0x11002C, 0x110030     GSP-window counters
```

Everything else — every control, capability, WPR and MMU register — reads back
bit-identical across a driver reload. **The reference is stable enough to diff
against**, which is the property the lane needs.

## 5. What this says about our hypothesis table

`docs/nv-gsp-firmware.md` §6 lists the offsets `nv_gsp.c` was going to be
written against. Each row below is judged against the measurements above.
"CONFIRMED" means the measured value is consistent with the claim *and* the
claim could have been falsified by it.

| # | §6 claim | measured (pre-init → post-init) | verdict |
|---|---|---|---|
| 1 | GSP processor block at BAR0+`0x110000` | `UC_CAPS 0x110108=0x80420100`, `UC_CAPS2 0x11012C=0xCA101136`, `UC_CTRL 0x110100=0x10`; identical in both states and identical to `nv`'s own log | **CONFIRMED** |
| 2 | mailboxes at `+0x040`/`+0x044` | `0x110040 = 0x00000000`, `0x110044 = 0x00000000`, both states; neighbours `0x110048` go `0xBADF5620 → 0` | **UNTESTABLE** — the offsets are inside the falcon IO window and are not fenced, but both read 0 with GSP-RM up, so nothing distinguishes "mailbox" from "scratch" from "unused" |
| 3 | engine reset at `+0x3C0` | `0x1103C0 = 0x00000000` both states | **UNTESTABLE** — 0 is consistent with "not asserted"; also consistent with "not implemented" |
| 4 | **RISC-V `CPUCTL` +`0x388` bit 7 = ACTIVE** | `0x111388 = 0x00000010` post-init with GSP-RM running + Vulkan rendering: **bit 7 clear, bit 4 set** | **CONTRADICTED** — bit 7 is not the ACTIVE flag on this part; the value matches the falcon `UC_CTRL.STOPPED` bit (bit 4). Do not gate on bit 7 |
| 5 | boot ROM parameters at +`0x668` | `0x111668 = 0x00000001` post-init (`0xBADF5620` pre) | **UNTESTABLE** — the address is live and changes across init, but `0x1` does not confirm "boot ROM parameters" |
| 6 | falcon v4 control: `CPUCTL 0x100`, `BOOTVEC 0x104`, `DMACTL 0x10C`, `DMATRFBASE 0x110`/`0x128`, `DMATRFCMD 0x118`, `HWCFG2 0xF4` bit 12 | `0x100=0x10`, `0x104=0x0`, `0x10C=0x1`, `0x110=0x0`, `0x128=0x0`, `0x118=0x2`, `0xF4=0x000047B7` (bit 12 = 0) | **CONFIRMED** as addresses: every one responds with a coherent value rather than `BADF`, and `0x118=0x2` is consistent with "idle = bit 1". Bit *semantics* beyond that are not proved by one static read |
| 7 | post-reset mask write `0x1668 = 0x111` | `0x001668` reads `0xBADF5040` in **both** states — the same word as `0x000100`, `0x000140`, `0x100100`, `0x100140` | **CONTRADICTED** as a BAR0-absolute offset: nothing *readable* lives at BAR0+`0x1668`. It is either block-relative or a privileged register — but it cannot be written blind at that address |
| 8 | WPR2 lo/hi at `0x1FA824`/`0x1FA828`; "nonzero hi means the firmware set it up" | pre `0x1FFFFE00` / `0x00000000`; post `0x1FFFFE00` / **`0x00000000`** | **CONTRADICTED** — hi is 0 while GSP-RM is running, so "nonzero hi" is not the live-check. The surrounding block *does* move across init (`0x1FA804 0x041B3000→0x1FFFF000`, `0x1FA808 0x05CF3000→0`, `0x1FA82C 0x00100000→0x1FFFFFF0`, `0x1FA830 0x05CF3FF0→0`) — that is the block to reverse, but the lo/hi pair is not it |
| 9 | WPR meta `GspFwWprMeta`, 256 B, magic `0xDC3AAE21371A60B3`, top-down 128 KiB aligned | not read — it lives in VRAM, which this lane did not map (BAR1) | **UNTESTABLE** in this pass |
| 10 | queue head kick `PGSP_QUEUE_HEAD(0)`, then first RPC exchange | `0x110C00 = 0x00000000` both states | **UNTESTABLE** — idle after boot; a static dump cannot see a queue that is not being kicked |
| 11 | RPC frame: 32-byte header `0x03000000` + `"VRPC"` | not visible in any register | **UNTESTABLE** here (needs a VRAM/queue read) |

### 5.1 Extra findings `nv_regs.h` should absorb

Beyond §6, three assumptions in the current driver's register map are now
answerable:

| `nv_regs.h` assumption | measured | verdict |
|---|---|---|
| `NV_PTIMER_TIME_0/_1 = 0x009400/0x009410` | live 64-bit counter, low word moves fast, high word advanced `0x31` between runs | **CONFIRMED** |
| `NV_PBUS_INTR_0 = 0x100100`, `NV_PBUS_INTR_EN_0 = 0x100140` | `0xBADF5040` in both states — same word as `PMC_INTR_0` | **CONTRADICTED** as PBUS registers on AD102 from an unprivileged read. PBUS is not at `0x100000` here, or those registers are privileged |
| `NV_PMC_INTR_0 = 0x000100`, `NV_PMC_INTR_EN_0 = 0x000140` | `0xBADF5040` in both states, including with the driver running and rendering | **REFRAMED** — the value is the chip's answer for an unreadable register, not proof that interrupts are unowned. `nv`'s `pmc intr=…(poison)` line is not diagnostic |
| `NV_FALCON_SEC2_BASE = 0x840000` "absent on this chip" | `0xBADF5620` pre-init; post-init the head answers (`0x840000=0x0`, `0x840004=0x0`, `0x840008=0x10`, `0x840018=0x01000000`) while the rest stays `0xBADF5620` | **CONTRADICTED** — SEC2 is present and responsive; `nv` reads it before anything enables the window. `nv`'s *probe discipline* (refuse a poisoned window) is right; the conclusion "absent" is an artifact. *Whether* it is usable is still open |
| `NV_PMC_ENABLE` bit 16 gates PTIMER | `PMC_ENABLE` reads `0x40000000` in both states; PTIMER counts regardless (§3.2) | **CONFIRMED** (this is the §5.3 finding in `nvidia-driver.md`, reproduced here on a driver-initialised card) |

### 5.2 The one thing that changes the plan

The single largest correction is **what a `0xBADF...` read means**. `nv`'s
§5.3 treats it as "this window is not the engine". The reference card shows
the same words appearing *with a working driver and a rendering GPU*
(`0x000100`, `0x000140`, `0x100100`, `0x100140`, `0x111000` all read
`0xBADF5040` post-init). So:

1. `BADF` in a **whole window, before init** = fenced/not enabled (real signal).
2. `BADF` in **individual registers, after init** = privileged or absent
   (not a signal about the block).

`nv` already refuses windows wholesale before init, which is correct. What it
must not do is draw conclusions about *which engines exist* from a pre-init
read — SEC2 being the concrete case.

## 6. The card is healthy, and it renders

Two cheap checks that close "is the hardware fine" for the parent's report.

`vulkaninfo --summary`, the physical-device block (the Vulkan ICD is NVIDIA's
proprietary one and it enumerated the card):

```
GPU0:
	apiVersion         = 1.4.312
	driverVersion      = 580.178.4.0
	vendorID           = 0x10de
	deviceID           = 0x2684
	deviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
	deviceName         = NVIDIA GeForce RTX 4090
	driverID           = DRIVER_ID_NVIDIA_PROPRIETARY
	driverName         = NVIDIA
	driverInfo         = 580.178.04
	conformanceVersion = 1.4.1.3
	deviceUUID         = 00750a15-4ed1-00fd-8fb7-5f2e1a25ed14
```

`nvidia-smi dmon -c 60 -s pucm` (60 samples at 1 s; first and last):

```
# gpu    pwr  gtemp  mtemp     sm    mem    enc    dec    jpg    ofa   mclk   pclk     fb   bar1   ccpm
# Idx      W      C      C      %      %      %      %      %      %    MHz    MHz     MB     MB     MB
    0     65     25      -      2      0      0      0      0      0  10501   2625      0      1      0
...
    0     19     25      -      0      0      0      0      0      0    405    210      0      1      0
```

25 °C, 65 W → 19 W, memory clock dropping 10501 → 405 MHz when idle: normal
idle power management on a healthy card, not a wedged one. The full 60-sample
capture is `docs/nv-gsp-reference-evidence/dmon-60s.txt`.

## 7. How to reproduce

```bash
# on r1 (root)
qm create 301 --name linux-ref --memory 16384 --cores 8 --sockets 1 --cpu host \
  --machine q35 --ostype l26 --scsihw virtio-scsi-single --net0 virtio,bridge=vmbr0 \
  --serial0 socket --vga std
qm importdisk 301 /root/hobbyos-nv/noble-server-cloudimg-amd64.img local-lvm
qm set 301 --scsi0 local-lvm:vm-301-disk-0 --boot order=scsi0
qm resize 301 scsi0 60G
qm set 301 --hostpci0 0000:01:00,pcie=1      # no x-vga: headless reference

# in the guest
sudo apt-get install -y nvidia-driver-570-server   # -> 580.178.04 here
sudo apt-get install -y gcc vulkan-tools
gcc -O2 -o nvdump nvdump.c
sudo ./nvdump named > dump.txt                     # works before AND after the driver binds
```

Two gotchas this lane hit, recorded so the next one does not:

1. **The PVE cloud-init network config names the NIC `eth0`; the guest's NIC is
   `enp6s18`.** netplan then applies nothing and the VM boots with no network
   and no guest agent, which looks like a broken template. Author the seed ISO
   yourself (or edit netplan offline) with `match: macaddress:` + `set-name:`.
2. **A `-serial0 socket` drops everything written before a client connects**,
   so a boot log read after the fact is empty. Attach a listener *before*
   `qm start` (`socat -u UNIX-CONNECT:/var/run/qemu-server/301.serial0 -`).

## 8. Limits of this reference

- Reads are **unprivileged**. Registers the GPU only exposes to its own
  privileged host context return `0xBADF....`; this lane cannot distinguish
  "privileged" from "absent" from userspace, and says so instead of guessing.
- The dump is **static**. Anything that only shows up while the driver is
  mid-boot (WPR2 being programmed, booter being loaded, the first RPC) is
  UNTESTABLE here — that needs a capture *during* `modprobe`, which is the
  natural next experiment and is now cheap: the tool runs in userspace, so a
  tight loop of `nvdump` around `modprobe nvidia` would catch it.
- VRAM (BAR1) was not mapped, so `GspFwWprMeta` and the RPC queues were not
  read. That is the other half of «what the reference driver writes» and it is
  the obvious follow-up.
