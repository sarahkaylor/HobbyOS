# HobbyOS in Parallels Desktop on Apple silicon: measured facts and port plan

Status: **measured, not ported.** Everything below was produced on this machine
(MacBook Pro M5 Max, macOS 27.2.1, Parallels Desktop 26.4.1) on 2026-11-13 by
booting `tools/probe/` as an aarch64 UEFI app inside a Parallels ARM VM and
reading its report back over a raw-sector channel. Nothing here is inferred
from documentation. Raw transcript: `docs/parallels-arm-probe.txt`.

## 1. The two questions that decide everything

| Model | Verdict | Evidence |
|---|---|---|
| x86_64 build in Parallels | **impossible** | `prlctl create x86probe --distribution other --cpu-type x86` → *"Parallels Desktop does not support running Intel-based virtual machines with this operating system on Apple Silicon Macs."* The x86 emulator whitelists Windows 10/11 + a few Linux distros; a custom OS is refused, and even when allowed it is 1 vCPU, 8 GB, no USB, no sound. |
| aarch64 build in Parallels | **possible, needs a platform port** | the VM boots foreign EFI code fine (our probe); the device model just shares almost nothing with QEMU `-M virt` |

Consequence for the game targets: **RimWorld cannot run on an ARM guest.** It is
a Unity 2022.3 x86_64 binary and HobbyOS has no x86 emulation. RimWorld stays on
the x86_64 QEMU path (this Mac, TCG) or the Proxmox Intel box. **Doom is fine** —
`DOOM.BIN` is portable C (doomgeneric) and already runs on aarch64 (§4).

## 2. What the Parallels ARM VM actually exposes

`prlctl create prvtest --distribution other` defaults: `efi-arm64` firmware
(EDK II), 2 vCPU, 2 GB, `hdd0` on `sata:0`, `net0` `card=virtio`, video
`adapter-type=parallels`.

| Subsystem | Parallels ARM VM | HobbyOS `ARCH=arm` expects | Gap |
|---|---|---|---|
| Firmware | EDK II, ACPI (11 tables: FACP GTDT MCFG PCCT APIC PPTT … ) | Limine `protocol: linux`, no DTB use | boot path |
| Console | **no serial at all** (no `SPCR`, no PL011 anywhere) | PL011 @ `0x09000000` | new console |
| Display | GOP linear framebuffer @ `0x20000000`, **1024×768, BGRx, pitch 1024, one mode only** | virtio-gpu over virtio-mmio | new display backend |
| Storage | **no AHCI, no NVMe, no virtio-blk, no virtio-mmio** — disks come from the proprietary vendor device `1ab8:4000` (class `ff`) | virtio-blk over virtio-mmio | no usable driver |
| Network | `1af4:1000` = legacy virtio-net (I/O BAR `0x10006000`, mem BAR `0x10005000`, IRQ 0x11) | virtio-net over virtio-mmio | legacy virtio-pci transport |
| Interrupts | **GICv3**: GICD `0x02010000`, GICR `0x02500000`; GTDT present | GICv2 @ `0x08000000` | new GIC |
| Input | only USB: EHCI `8086:265c`, xHCI `1033:194` — no virtio-input, no PS/2 | virtio-keyboard/tablet over virtio-mmio | new HID |
| PCI | single bus 0, ECAM @ `0x02300000` (MCFG), 5 devices total | no PCIe on ARM (x64-only code) | new enumerator |
| RAM | `0x40000000` + 2038 MiB (Limine/UEFI memmap available) | hardcodes `0x40000000` base | base happens to match |

The lucky part: RAM base matches and the framebuffer is a plain linear surface
at a fixed address the firmware hands over — so a display-only port is the
cheapest real milestone.

The unlucky part: **there is no storage controller to write a driver for.** The
SATA disk is behind `1ab8:4000`, Parallels' own closed device (their guest
drivers are the `prl_*` modules in Parallels Tools). Reverse engineering it is
not a sane first move, which is why §3 routes storage through a boot module.

## 3. Staged port plan

Each stage is independently verifiable with the probe harness.

| Stage | Work | Verifiable by |
|---|---|---|
| **M1** boot + console | Limine **native protocol** (framebuffer, memmap, RSDP requests) instead of `protocol: linux`; 8×16 FB text console; guard PL011/virtio-mmio/GICv2 init behind a platform probe | kernel boot log painted at 1024×768 in the VM window |
| **M2** userland without a disk driver | Limine loads a small FAT image as a **module**; add a ramdisk backend to the block layer; the 1 GiB `disk.img` is too big for a 2 GB guest — build a trimmed ~256 MiB image | `ls` / `DOOM.BIN` from the ramdisk |
| **M3** interrupts | GICv3 driver (GICD + redistributor frames, SGI/PPI), generic timer from GTDT; start single-CPU, then PSCI secondary bring-up | timer ticks, preemption, `QEMU_SMP`-free single-CPU boot |
| **M4** display | paint the desktop straight into the GOP framebuffer (1024×768 BGRx) — replaces virtio-gpu scanout | desktop + windows on the VM screen |
| **M5** input | USB HID over xHCI (`1033:194`) or Parallels' input path — the heavy lift | keyboard/mouse in the desktop |
| **M6** Doom | already builds for aarch64 (§4); needs M2+M4 to reach the screen | playable Doom in Parallels |

M1 and M2 are a day's work each; M3/M4 another; M5 is the open-ended one.
Nothing here needs RimWorld, and nothing here touches the existing QEMU path if
the new code lands behind a platform probe.

## 4. What works today on this host (ARM in QEMU)

```sh
TOOLS_SKIP=1 BROWSER_SKIP=1 make -j12 ARCH=arm MODE=desktop DESKTOP=hobby disk.img
make run ARCH=arm DESKTOP=hobby MODE=desktop QEMU_ARGS="-qmp unix:/tmp/hb-qmp.sock,server,nowait"
```

`DESKTOP=hobby` matters: `MODE=desktop` defaults to `DESKTOP=icewm`, and the X11
stage then builds `HBHOST.BIN` from `src/user/hbhost/hbhost.c`, which uses
`rdtsc` (`invalid output constraint '=a'`) and cannot compile for ARM.

Evidence: `docs/parallels-arm-desktop.png` (1920×1080 desktop),
`docs/parallels-arm-doom.png` (native `DOOM.BIN`, Freedoom IWAD, gameplay/demo loop,
pid 3 alive with no faults).

Launching a game on the native desktop needs keyboard navigation: the Apps menu
is taller than the screen (DOOM.BIN is menu index 18 of 40 and only ~16 rows are
visible), and it is a press-drag-release menu, so a second independent click on
an item just dismisses it. `tools/probe/launch_app.py` presses Apps, walks the
highlight down with ArrowDown, and confirms with Return. The index comes from the
kernel's own `[MENU] 18=DOOM.BIN` dump on the serial console.

## 5. The measurement harness

| File | Purpose |
|---|---|
| `tools/probe/prl_probe.c` | aarch64 UEFI app: GOP, ACPI, PCI (ECAM + config space), block devices, filesystems, virtio-mmio scan, memory map |
| `tools/probe/build.sh` | clang (`--target=aarch64-windows-gnu`, COFF) + `lld-link /subsystem:efi_application` → `BOOTAA64.EFI`, wrapped in a GPT+ESP image |
| `tools/probe/mkgpt.py` | GPT with one EFI System Partition, because Parallels' firmware is not guaranteed to offer a bare "superfloppy" FAT as a boot device |
| `tools/probe/deploy.sh` | Parallels refuses a raw image and `prl_disk_tool` cannot import one, but a **plain** `.hdd` is literally the raw bytes: create an empty one, `dd` the GPT image into its `.hds` |
| `tools/probe/read_probe.sh` | read the report back with `mtype` at the ESP offset, else the raw sector dump the probe writes at `LastLBA-96` (magic `HBPROBE1`) |
| `tools/probe/click_abs.py`, `launch_app.py`, `ppm2png_min.py` | absolute-pointer input, menu launching, PPM→PNG with stdlib only |

Three things that cost a cycle each and are easy to repeat:

1. A UEFI console does **not** return the carriage on a bare `\n` — output
   staircases across the screen. Emit `\r\n`.
2. `EFI_BLOCK_IO_PROTOCOL`'s GUID starts `21 5b 4e 96`; a wrong GUID silently
   yields "no BlockIo handles" and looks like a firmware limitation.
3. `prlctl capture <vm> --file shot.png` grabs the guest screen (1024×768) with
   no window-manager access needed; `screendump` over QMP is the QEMU-side twin.
