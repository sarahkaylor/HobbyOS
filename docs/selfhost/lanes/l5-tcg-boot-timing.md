# Lane L5 — measured timing evidence for the acceptance budget (2026-10-14)

All measurements taken on the workstation (64-core x86-64 host, QEMU 10.2.1,
TCG only for ARM). Cited from `docs/selfhost/acceptance.md` §4 (budget).

## 1. ARM boot under TCG — headless, 8 cores / 16 GB (real measurement)

Boot target: `~/.hermes/cache/scratch/l5/disk-real-copy.img` — a 1 GiB copy
of the workstation `disk.img` AFTER lane-L5 staging (`/SRC` added; boot files
untouched). Exact QEMU line = Makefile ARM branch, `-display none`,
`-serial file:`:

```text
qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 8 -m 16384M \
  -accel tcg,thread=multi -bios $HOME/.local/share/AAVMF/AAVMF_CODE.fd \
  -display none -serial file:BOOTLOG.serial \
  -drive if=none,file=IMG,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
  -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
  -semihosting -action shutdown=poweroff
```

Run 1 (5 s poll granularity): "Booting AArch64 OS" already present at the
first poll (≤5 s); "Mode: DESKTOP" in the same window. Serial grew to the
`[MENU] count=128` dump — desktop userland alive.

Run 2 (1 s poll granularity, elapsed from `ps etimes` of the VM process):

| marker | elapsed | meaning |
|---|---|---|
| `Booting AArch64 OS` (kernel C entry, main.c:354) | ~9 s | kernel booted, marker asserted by S4/S5 |
| `Mode: DESKTOP - Launching desktop...` (main.c:507) | ~9 s | first user program launched from /BOOT kernel |

Budget figure used in acceptance.md: **≈ 5–10 s per bare boot to a stable
kernel/user marker under MTTCG on this host** (poll noise ±2 s). Two boots
per acceptance run (S4 + S5) → ≈ 10–20 s total QEMU overhead.

Serial evidence: `~/.hermes/cache/scratch/l5/boot-smoke.serial` (run 1),
`boot-smoke3.serial` (run 2). QEMU was killed via `fuser -k <image>` after
the desktop marker (desktop runs forever; never wait for it to exit).

## 2. Host-clang cost of one kernel TU (real measurement)

Exact kernel flags (Makefile ARM branch): `-O2 -Wall -Wextra -g -Isrc/include
--target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only`.
Host clang 21.1.8. 3 runs each, wall time via `/usr/bin/time`:

| kernel TU | per-compile (s) |
|---|---|
| `src/kernel/main.c` (small head) | 0.05 / 0.06 / 0.06 |
| `src/kernel/program_loader.c` (mid) | 0.22 / 0.22 / 0.23 |
| `src/kernel/fat16.c` (heavy: LFN + full FAT16) | 0.59 / 0.59 / 0.58 |

Kernel = **71 TUs** (66 C + 5 asm — 66 `find src/kernel -name '*.c'`, 5
`*.s`). Weighted-serial estimate ≈ 0.25–0.4 s × 71 ≈ **18–28 s of pure clang
CPU on the host** for the whole kernel; libc (31 TUs, small headers) ≈
2–5 s; acceptance user set (HELLO/SH/LS/CAT) negligible.

The 5-10× TCG multiplier for the in-OS compile is marked **VERIFY until
M2** (no in-OS toolchain exists yet to measure against); see acceptance.md §4
for the derivation and the hobytest plan.

## 3. FAT16 slack measurement (sizing math input)

Staged 30,108,339 payload bytes into a 1 GiB disk (32 KiB clusters —
`mkfs.fat -F 16` auto-pick at 1 GiB, 8 sectors/cluster): free went
939,114,496 → 878,231,552 B, i.e. **60,882,944 B (~58 MiB) consumed ≈ 2.02×
the file bytes**. Small-file payloads pay ~2× slack on 32 KiB clusters; this
is why OUT.IMG (smaller clusters, see acceptance.md §3) and the ≤256 MiB cap
matter.

## 4. Other numbers recorded

- Kernel/artifacts: `hobbyos.elf` 1,353,296 B; `hobbyos.bin` 214,400 B;
  `bootloader/BOOTAA64.EFI` 315,392 B.
- Biggest user bins in `obj/arm` (informational — none are in the AD-9 set):
  browser.bin 87,564,512 B; big.bin 30,167,040 B; sqltest.bin 759,872 B;
  cxx_t.bin 624,544 B; desktop.bin 553,088 B; sh.bin 191,072 B.
- Free on the workstation disk (re-measured today): 938,786,816 B
  (~895 MiB; the brief's 939,114,496 B was measured earlier — delta is a
  live-disk artifact, a concurrent desktop VM was writing temp files).
