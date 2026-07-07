---
name: hobbyos-screenshot
description: "Use when you need to SEE HobbyOS's screen — capture the QEMU framebuffer as a PNG, either from local qemu on the Mac (QMP screendump) or from a remote Proxmox VM, with a black-screen sanity check."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [hobbyos, qemu, screenshot, qmp, screendump, framebuffer, proxmox]
    related_skills: [hobbyos-build-and-run, hobbyos-gui-test, hobbyos-proxmox-gpu]
---

# HobbyOS: Screenshot the Display

## Overview

HobbyOS renders to a QEMU framebuffer — there is no scrollback or text you can grep for the GUI. To observe what's actually on screen you take a **QMP `screendump`**, which writes a `.ppm`, then convert it to `.png` so it's viewable. This skill covers both capture paths: **local qemu on the Mac** and a **remote Proxmox VM** (`192.168.10.174`). Always end with the black-screen check — an all-black image means the desktop never rendered, which is a real defect, not a capture glitch.

Bundled helpers (in this skill's `scripts/`):
- `ppm2png.py <in.ppm> <out.png>` — dependency-free PPM→PNG (pure Python zlib/struct).
- `screenshot_local.sh [qmp-sock] [out.png]` — one-shot local capture from a running qemu.

## When to Use

- Verifying the desktop / a GUI app actually drew something.
- Capturing visual evidence before/after a graphics change.
- Debugging "did it boot to the desktop?" when serial is silent.

Don't use for: *driving* the UI with clicks/keys (→ [[hobbyos-gui-test]]) or serial/log capture (that's plain text — read the serial file directly).

## Prerequisite: QEMU must expose a QMP socket

`make run` alone has no QMP endpoint. Launch (or relaunch) with one:

```bash
# From repo root. Cocoa window still opens; screendump works regardless of -display.
make run QEMU_ARGS="-qmp unix:./qmp-sock,server,nowait"

# Headless variant (no window) — good for automated capture:
make run QEMU_ARGS="-display none -qmp unix:./qmp-sock,server,nowait"
```

For a fully scripted capture that boots, screenshots, and validates in one shot, prefer `make desktop_test` (→ `run_desktop_test.py`), which already wires a QMP socket and checks for a blank image.

## Local capture (Mac)

With qemu running and a `./qmp-sock` present:

```bash
bash skills/hobbyos-screenshot/scripts/screenshot_local.sh ./qmp-sock /tmp/hobbyos.png
```

What it does (do this by hand if the script isn't deployed): send `qmp_capabilities`, then `screendump` to a `.ppm`, then convert:

```bash
# Talk to QMP over the unix socket (needs socat; `brew install socat`)
( printf '{"execute":"qmp_capabilities"}\n'; sleep 0.2; \
  printf '{"execute":"screendump","arguments":{"filename":"%s"}}\n' "$PWD/shot.ppm"; \
  sleep 1 ) | socat - UNIX-CONNECT:./qmp-sock
python3 skills/hobbyos-screenshot/scripts/ppm2png.py shot.ppm /tmp/hobbyos.png
```

Then **view** `/tmp/hobbyos.png` (open it / attach it) — capturing without looking proves nothing.

## Remote capture (Proxmox VM)

The Proxmox host is `192.168.10.174` (SSH key `~/.ssh/mac_to_r1`). Each VM exposes a QMP unix socket at `/var/run/qemu-server/<VMID>.qmp`. Screendump writes on the *remote* box, so capture there and copy back:

```bash
VMID=225   # e.g. the nvidia consumer VM; use your target VM id
ssh -i ~/.ssh/mac_to_r1 -o StrictHostKeyChecking=no root@192.168.10.174 "
  ( sleep 0.4; echo '{\"execute\":\"qmp_capabilities\"}'; sleep 0.3;
    echo '{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"/root/ss${VMID}.ppm\"}}'; sleep 2 ) \
  | socat - UNIX-CONNECT:/var/run/qemu-server/${VMID}.qmp 2>&1 | tr -d '\r' "
scp -i ~/.ssh/mac_to_r1 -o StrictHostKeyChecking=no root@192.168.10.174:/root/ss${VMID}.ppm /tmp/ss${VMID}.ppm
python3 skills/hobbyos-screenshot/scripts/ppm2png.py /tmp/ss${VMID}.ppm /tmp/ss${VMID}.png
```

This mirrors the repo's `screenshot.sh`. See [[hobbyos-proxmox-gpu]] for VM ids and the wider remote workflow. Note: a Linux guest sitting at a **VGA text console** (e.g. VM 225's login tty) screendumps fine, but you can't *type* into it over QMP text — use `qm sendkey` for that.

## The black-screen check (do not skip)

A completely black PPM means the framebuffer never rendered — a genuine failure. Validate before trusting a capture:

```bash
python3 - <<'PY' /tmp/hobbyos.png.ppm 2>/dev/null || true
PY
# Simpler: check the PPM pixel bytes are not all zero (matches run_desktop_test.py logic)
python3 -c "d=open('shot.ppm','rb').read(); print('BLANK/BLACK - desktop did not render' if not any(d[100:]) else 'OK: non-blank image')"
```

If blank: the bug is upstream (desktop/graphics/init), not the screenshot. Investigate boot/serial before re-capturing.

## Common Pitfalls

1. **No QMP socket.** `make run` without `QEMU_ARGS=...-qmp...` has nothing to connect to. Relaunch with the socket.
2. **Stale socket / zombie qemu.** A leftover `./qmp-sock` from a dead qemu refuses connections. `rm -f ./qmp-sock` and `pkill -f qemu-system`, then relaunch (see [[hobbyos-build-and-run]]).
3. **Capturing but not viewing.** The PPM/PNG is the whole point — open it. A green "screendump returned" is not evidence.
4. **Missing `socat`/`python3`.** `brew install socat`; python3 ships with the venv/macOS. The `ppm2png.py` helper has no third-party deps.
5. **Remote path confusion.** `screendump` writes on the machine QEMU runs on. For a Proxmox VM that's the Proxmox host — write to `/root/...` there, then `scp` back.

## Verification Checklist

- [ ] QEMU launched with a reachable QMP socket.
- [ ] `screendump` returned `{"return": {}}` (no error).
- [ ] PPM converted to PNG and actually opened/viewed.
- [ ] Black-screen check ran; a blank result was treated as a render defect, not a capture retry.
