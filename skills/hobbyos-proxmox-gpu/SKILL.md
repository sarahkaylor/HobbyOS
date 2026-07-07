---
name: hobbyos-proxmox-gpu
description: "Use when deploying or debugging HobbyOS's x86_64 remote-PCIe / RTX 4090 GPU-over-RDMA path on the Proxmox server (192.168.10.174). Covers VM topology, deploy scripts, serial + screenshot capture on remote VMs, net_pci_client, and the current Xid-79 / MSI-X blocker."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [hobbyos, proxmox, gpu, rdma, pcie, nvidia, x86_64, iommu, remote]
    related_skills: [hobbyos-build-and-run, hobbyos-run-tests, hobbyos-screenshot, hobbyos-gui-test]
---

# HobbyOS: Proxmox / GPU-over-RDMA

## Overview

HobbyOS can share a physical PCIe device (an **RTX 4090**, `0x10de:0x2684`) across VMs over UDP/IP RDMA. This is **x86_64-only** and runs on the Proxmox server **`192.168.10.174`** (SSH key `~/.ssh/mac_to_r1`), not on your Mac. The consumer VM runs `nvidia-smi`; a `net_pci_client` process on the Proxmox host forwards its device access over RDMA to the VM that physically owns the card. This is the hardest, longest-running workstream — reads/serial/screens all go through SSH, and the current frontier is a driver-init blocker, not a build problem. Rehearse the mechanics locally first with the two-VM test (`./run_two_instances.sh`, see [[hobbyos-run-tests]]) before touching the remote GPU.

## When to Use

- Deploying HobbyOS to Proxmox for Intel/PCIe/RDMA/GPU testing.
- Debugging the GPU passthrough: firmware load, DMA mirroring, interrupts, `nvidia-smi`.
- Capturing serial/console/screen from a remote VM.

Don't use for: local ARM/desktop work (→ [[hobbyos-build-and-run]]) or the local RDMA rehearsal (that's `run_two_instances.sh` in [[hobbyos-run-tests]]).

## Topology (measured)

- **VM 225** ("testvm", real Linux) — the **consumer** that runs `nvidia-smi`. Its GPU is a `net_pci_client` vfio-user device on the Proxmox host that forwards over UDP/RDMA. Login is on a **VGA tty**, not serial — drive it with `qm sendkey` + screendump, not `qm terminal`.
- **VM 205** ("HobbyOSHostGPU") — **owns the physical card** (`0000:01:00`). `net_pci_client` also writes VM 205's RAM directly via `/proc/<pid>/mem` for the DMA mirror.
- Both VMs use a vIOMMU: `-machine q35,accel=kvm,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on`.
- `net_pci_client` source: `src/tools/net_pcie/net_pci_client.c`. IOMMU mode: append `--iommu` (e.g. `./net_pci_client /tmp/sock 10.0.2.16 0x10de 0x2684 --iommu`).

For the authoritative, up-to-date topology and commands, also consult the project memory notes **gpu-passthrough-topology** and **gpu-rdma-read-latency-blocker** — this skill summarizes them but they track the live state.

## Deploy paths

**Two HobbyOS instances sharing an `-device edu` card (scripted end-to-end):**
```bash
./run_proxmox_gpu.sh        # builds x86_64 unit-test disk, uploads, creates VM 205 (host) + 206 (guest)
```
It builds `ARCH=intel MODE=unit_tests`, scp's `disk.img`/`hobbyos.elf` to `/root/hobbyos/`, and creates both VMs with `fw_cfg opt/pcishare=host|guest:0x10de:0x2684`, direct `-kernel` boot, serial to files, NVMe disk, and the intel-iommu. Monitor with `tail -f /root/hobbyos/{host,guest}.log` over SSH.

**Deploy the desktop build as a bootable VM (205):**
```bash
make deploy_intel           # builds ARCH=intel disk.img, scp to Proxmox, recreates VM 205 (OVMF/NVMe)
make deploy_run_intel       # same, then attaches: qm terminal 205
```

**Manual VM lifecycle on Proxmox** (`ssh -i ~/.ssh/mac_to_r1 root@192.168.10.174 '<cmd>'`):
```bash
qm start <id> | qm stop <id> | qm destroy <id> --purge
qm terminal <id>            # serial console (only if the guest uses serial)
qm sendkey <id> <key>       # inject a keystroke to a VGA console (ret, a, b, ...)
```

## Capturing state from a remote VM

- **Serial console (HobbyOS or serial-configured Linux):** drive `qm terminal <id>` non-interactively with pexpect. `test_nvidia.py` logs into VM 225 and runs `lspci -nn` / `dmesg | grep -i nvrm` / `nvidia-smi`; `dump_serial.py` just captures the serial dump. Exit `qm terminal` with **Ctrl-O** (`\x0f`).
- **Screenshot (VGA console / desktop):** QMP `screendump` on the VM's socket `/var/run/qemu-server/<id>.qmp`, then `scp` the `.ppm` back and convert — exactly the [[hobbyos-screenshot]] remote path (the repo's `screenshot.sh` does VM 225).
- **VGA-only login (VM 225):** `qm terminal`/pexpect **can't** reach a VGA getty. Log in and type via `qm sendkey`, read the result via screendump. This is the only reliable way to run `nvidia-smi` on 225 today.

## Current frontier & blockers (as of the last GPU session)

The system reaches **sustained bidirectional GSP RPC** — the furthest yet. State to be aware of before changing anything:

- **Read latency is solved — do not "re-optimize" it.** The per-BAR-read slowness was caused by (a) a synchronous `printf` trace on every read and (b) polling the poison-locked `PMC_INTR` (BAR0+0x100) every iteration. Fixes: sample the trace, and rate-limit the 0x100 poll to ~50 ms → reads ~0.2 ms, 0 retries. **Do NOT shorten the 500 ms RDMA timeout or add retries** — resends create duplicate host responses that desync the req/resp stream (this regressed before).
- **DMA mirror must cover ALL RAM regions.** Guest RAM splits at the PCI hole (a low ~2 GB region + a high 1 GB region at `iova=0x100000000`); the firmware working set is >1.4 GB scattered across both. A single-region mirror silently drops the high region → GPU DMA-reads zeros. `g_ram_regions[]` tracks every >1 MB region; verification is **sampled, log-only** on purpose (a full per-2 MB verify starved the driver and tripped the soft-lockup watchdog).
- **Open blockers to `nvidia-smi` working:**
  1. **No real interrupts.** Real GSP/RM interrupts are **MSI-X**, which HobbyOS doesn't capture; polling `PMC_INTR` returns NVIDIA poison `0xbadf5040`. Needs host-side **MSI-X capture + forwarding**.
  2. **RPC coherence vs. write-clobbering.** The `/proc/PID/mem` bidi-diff uses "host wins" and can overwrite guest-authored RPC command pages. Needs a fast, non-clobbering dedicated sync channel for the small GSP RPC-queue regions.
  3. **Guest VGA console wedges** during sustained RPC (a vCPU stalls mid-printk), so the shell can't be driven to confirm `nvidia-smi`. Give VM 225 a serial console (`console=ttyS0`) or investigate the vCPU stall.

Instrumentation already present: `[RTT STATS]`, `BIDI SYNC ... clobber=/fwd_off=`, `[RDMA] DMA VERIFY OK`. Use it; don't rip it out.

## Common Pitfalls

1. **Running GPU/RDMA on ARM.** It's `#ifdef __x86_64__`; build `ARCH=intel`.
2. **`qm terminal` on VM 225.** VGA-only login — use `qm sendkey` + screendump instead.
3. **Shortening the RDMA timeout / adding retries.** Known regression — leave the 500 ms timeout alone.
4. **Assuming a single-region RAM mirror is enough.** It isn't — the high region holds hundreds of MB of firmware.
5. **`pkill` over SSH matching your own command.** Kill by PID or use `qm stop`; a broad `pkill -f` can nuke your session.
6. **Editing VM 117's config.** `/etc/pve/nodes/r1/qemu-server/117.conf` is a read-only reference example — copy its `hostpci0` line, don't modify it.

## Verification Checklist

- [ ] Built `ARCH=intel`; disk + kernel uploaded to `/root/hobbyos/`.
- [ ] vIOMMU flags present on both VMs (`intel-iommu,...,caching-mode=on`).
- [ ] Serial captured via pexpect for serial guests; `qm sendkey` + screendump for VM 225's VGA login.
- [ ] DMA mirror covers all `g_ram_regions[]` (low + high); verify lines present.
- [ ] Did not shorten the RDMA timeout, add retries, or strip instrumentation.
- [ ] VMs stopped/cleaned up (`qm stop`) when done; no stray VMs left running.
