---
name: hobbyos-gui-test
description: "Use when testing HobbyOS's desktop/GUI: inject synthetic mouse and keyboard over QMP to click menus and type, then validate the framebuffer. Covers the auto-launch desktop_test path and manual QMP input driving."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos]
metadata:
  hermes:
    tags: [hobbyos, qemu, gui, qmp, input, desktop, framebuffer, testing]
    related_skills: [hobbyos-screenshot, hobbyos-build-and-run, hobbyos-run-tests, hobbyos-proxmox-gpu]
---

# HobbyOS: GUI / Desktop Testing

## Overview

HobbyOS's desktop has no test hooks you can call directly — you exercise it the way a user would, by injecting **synthetic mouse and keyboard events over QMP** and then observing the framebuffer with a screenshot. Two paths exist: the automated **`desktop_test`** mode (boots the desktop, auto-launches a UI app, self-shuts down) for pass/fail CI-style checks, and **manual QMP input driving** (`input-send-event` / `send-key`) when you need to click a specific menu item or type into an app. Pair every input sequence with [[hobbyos-screenshot]] — injecting clicks proves nothing without seeing the result.

## When to Use

- Validating the window manager, menus, or a GUI app (editor, pong, desktop).
- Reproducing a UI bug that needs a specific click/keystroke sequence.
- Confirming the graphical framebuffer + event subsystem after a change.

Don't use for: pure logic testing of a GUI app (→ host golden tests in [[hobbyos-run-tests]], far faster) or headless kernel/syscall tests.

## Path 1 — Automated framebuffer test (preferred for pass/fail)

```bash
make desktop_test          # ARM;  runs ./run_desktop_test.py
make desktop_test_intel    # x86_64
```

`run_desktop_test.py` boots `MODE=desktop_test` headless with a QMP socket, waits (≤30 s) for the kernel to print `SCREENSHOT_READY`, takes a `screendump` to `src/user/actual_qemu.ppm`, kills QEMU, and **fails if the image is all black**. Use this as the quick "does the desktop still render + auto-launch work?" gate. `DESKTOP_TEST_AUTO_LAUNCH` in the build makes the desktop launch a UI app without manual input.

## Path 2 — Manual QMP input driving

Boot with a QMP socket, then script the interaction. Coordinates are **absolute** (the VM uses `virtio-tablet`), origin top-left, in screen pixels.

```bash
make run QEMU_ARGS="-display none -serial file:serial.log -qmp unix:./qmp-sock,server,nowait"
```

Event vocabulary (send as one JSON object per line after `qmp_capabilities`):

```jsonc
// Move pointer to absolute (x=20, y=26)
{"execute":"input-send-event","arguments":{"events":[
  {"type":"abs","data":{"axis":"x","value":20}},
  {"type":"abs","data":{"axis":"y","value":26}}]}}
// Press & release left button (a click = down then up)
{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":true}}]}}
{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":false}}]}}
// Type a key by qcode
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"a"}]}}
```

The repo's `test_input.py` is the working reference: it opens `./qmp-sock`, moves to the File menu, clicks, opens the Editor, and types `a`/`b`, capturing `serial.log`. Copy its structure — connect, read the greeting, send `qmp_capabilities`, then drive events with small `sleep`s between UI actions so the desktop can repaint. Read one QMP reply line after each command so the socket buffer doesn't stall.

**Sequence discipline:** move → button-down → button-up is a click; insert ~0.5–1 s between distinct UI actions (menu open, app launch) to let the compositor catch up; screenshot after each meaningful step to confirm state before proceeding.

## Remote VGA console (Proxmox Linux guests)

For a Linux guest on Proxmox whose login is on a **VGA text tty** (not serial — e.g. VM 225), QMP `send-key` won't reach the getty reliably; use Proxmox's own key injection and screendump:

```bash
ssh -i ~/.ssh/mac_to_r1 root@192.168.10.174 "qm sendkey 225 ret"       # press Enter
ssh -i ~/.ssh/mac_to_r1 root@192.168.10.174 "qm sendkey 225 r; qm sendkey 225 o; qm sendkey 225 o; qm sendkey 225 t"
```

Then screendump to read the screen (see [[hobbyos-screenshot]] remote path). This is the documented way to log in and drive VM 225 for `nvidia-smi`; see [[hobbyos-proxmox-gpu]].

## Common Pitfalls

1. **Not reading QMP replies.** After each `execute`, read the response line. Skipping reads eventually blocks the socket and the sequence "freezes" (looks like an OS hang but isn't).
2. **No settle time.** Firing clicks back-to-back before the desktop repaints clicks the wrong target. Add `sleep`s and screenshot to confirm.
3. **Relative vs. absolute coords.** This VM is absolute-pointer (`virtio-tablet`); send `abs` axis events with real pixel coordinates, not `rel` deltas.
4. **The desktop never exits.** A manual `make run` desktop runs forever — `pkill -f qemu-system` (or `proc.terminate()` in a script) when done. Automated `desktop_test` handles its own shutdown.
5. **Judging by logs alone.** GUI state lives in the framebuffer — take a screenshot; serial only shows what the app chose to print.

## Verification Checklist

- [ ] QEMU launched with QMP socket (and `-serial file:serial.log` if you need output).
- [ ] Each input command's QMP reply was consumed.
- [ ] Settle delays inserted between distinct UI actions.
- [ ] A screenshot confirms the expected on-screen state (not just a non-error QMP return).
- [ ] QEMU terminated afterward (no zombie process).
