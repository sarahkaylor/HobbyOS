---
name: hobbyos-x64-remote-build
description: "Use when a macOS dev box needs a trustworthy x86_64 HobbyOS build: tar the working tree, scp it to linuxci (192.168.0.25), run make ARCH=intel, pull hobbyos.elf/disk.img back, and verify the image actually boots."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, x86_64, intel, remote-build, linuxci, scp, make, ci]
    related_skills: [hobbyos-build-and-run, hobbyos-run-tests, hobbyos-screenshot, hobbyos-proxmox-gpu]
---

# HobbyOS: Remote x64 Build (macOS → linuxci)

## Overview

The macOS host is the **ARM** development environment. Its x86_64 lane survives on
workarounds: no Homebrew musl toolchain can link `SDLAPP.BIN`, `ld.lld` must be on
`PATH` unqualified, ICU's `configure` link tests get handed to Mach-O `/usr/bin/gcc`
unless they go through `tools/cc-x86-none-elf.sh`. Those fixes work, but a Mac x64
tree drifts from what a Linux box produces — and the interesting x64 artifacts (an
image that boots icewm + the X server + games) are exactly where drift shows up.

`linuxci` (192.168.0.25) is the reference builder: Ubuntu 26.04, 48 cores, the same
clang/lld/llvm **21.1.8** the project pins, mtools/dosfstools/qemu 10.2, nested KVM
for boot validation. This skill drives it end to end:

**tar the current working tree → scp → `make ARCH=intel disk.img` → pull artifacts
→ verify the ELF arch → boot the image under KVM and look for the desktop.**

Everything is reproducible from the Mac with one command; the remote workspace is
throwaway and deleted on success.

## When to Use

- You changed kernel/userland code and need an x64 image you trust to boot.
- A Mac-side x64 build fails on vendor weirdness (SDLAPP musl, ICU driver, ld.lld
  not found) and you want to know whether the *code* is broken or the *host* is.
- You need `hobbyos.elf` / `disk.img` for an x64 gate, a GPU/RDMA lane, or a boot
  check without burning the Mac's cores for 6 minutes.
- You want a red/green answer that includes "does it boot", not just "did it link".

**Don't use for:** ARM builds (build locally — `ARCH=arm` is the Mac's primary
target, see [[hobbyos-build-and-run]]), interpreting test results
(→ [[hobbyos-run-tests]]), or GPU-over-RDMA deploys (→ [[hobbyos-proxmox-gpu]]).

## Quick Start

Run from anywhere inside the repo:

```bash
S=skills/hobbyos-x64-remote-build/scripts/remote_build.sh

$S                    # cold build, full verify, artifacts in build/x64-remote/<stamp>-<rev>/
$S --check            # preflight only: repo, host reachability, toolchain, disk
$S --reuse            # incremental rebuild in the last COMPLETED remote workspace
                      # (a plain run deletes its tree, so seed one with --keep first)
$S --no-boot-check    # skip the 75 s KVM boot validation
$S --extra "MODE=test BROWSER_SKIP=1"
$S --with-tools       # also stage the in-OS LLVM (/TOOLS) into the image
$S --host 192.168.0.25 --jobs 48
```

On success it prints a summary table and leaves:

| Artifact | What it is |
|---|---|
| `hobbyos.elf` | the x64 kernel, ELF32/Intel-80386 multiboot1, entry `0x70000030` |
| `disk.img` | 1024 MiB FAT16 image: kernel + every userland `.BIN` + X11/icewm + browser |
| `boot.log` | serial capture from the KVM boot check (the proof it reached the desktop) |
| `build.log` | full remote make log (warnings included) |
| `manifest.txt` | git rev/branch/dirty count, host, jobs, timings, sha256 of both artifacts |

## What the pipeline actually does

1. **Local preflight** — asserts the repo root (`Makefile` + `src/kernel/arch/x64`),
   records `git rev-parse` and the dirty-file count, verifies `git`/`zstd`/`scp`.
2. **Remote preflight** — ssh reachability, `x86_64` check, core count, free space in
   `$HOME`, and every tool the x64 `disk.img` build invokes
   (`clang clang++ ld.lld llvm-objcopy llvm-ar llvm-nm llvm-readelf make tar zstd
   mcopy mmd mdir mkfs.fat meson xkbcomp python3`). Missing tools are named with the
   exact `apt-get` line that fixes them.
3. **Tarball** — `git ls-files -z --cached --others --exclude-standard`, i.e. the
   *current working tree* (uncommitted edits included, `.gitignore` respected),
   minus build outputs, **plus** three gitignored vendor caches that make the build
   offline-capable (`third_party/x11/dl/`, the ICU source/data archives). ~175 MB
   compressed, created in <2 s.
4. **Transfer** — one `scp` with compression off (the payload is already zstd):
   ~5 s on the LAN.
5. **Extract into a fresh directory**, `rm -f hobbyos.elf hobbyos.bin` first. This is
   the single most important correctness rule in the skill (see pitfalls).
6. **Build** — `make -j<min(nproc,32)> ARCH=intel TOOLS_SKIP=1 disk.img`, log to
   `build.log`, bounded by `--timeout` (default 5400 s).
7. **Boot check** — copies OVMF to a per-run file and boots `disk.img` with
   `-enable-kvm -display none -serial file:boot.log` for `--boot-seconds` (75),
   then requires `Mode: DESKTOP`, `ICEWM.BIN`, `XHOBBY.BIN` and no panic.
8. **Pull + verify** — artifacts come back, `verify_artifacts.sh` re-checks the ELF
   arch, the multiboot1 magic, the FAT contents, and the boot log locally.
9. **Manifest + cleanup** — writes `manifest.txt`, then deletes the remote workspace
   unless `--keep`/`--reuse`.

## Cost

| Step | Cold | Warm (`--reuse`) |
|---|---|---|
| tar + scp + extract | ~8 s | ~8 s |
| `make -j32` (48-core builder) | **340–380 s** | 4–30 s |
| boot check | 75 s | 75 s (`--no-boot-check` to skip) |
| pull back (1.0 GiB image) | ~10 s | ~10 s |
| **total** | **~7.5 min** | **~1.5 min** |

Default `--jobs` is `min(nproc, 32)`: past 32 the parse/X11 stages don't scale and
the shared-archive window widens (see the `libc.a` pitfall).

## Artifacts and booting them on the Mac

```bash
# The pulled image boots the same way a local one does:
make run_intel QEMU_ARGS="-drive file=<artifact-dir>/disk.img,format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1"

# Headless proof, no window:
qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 4096M \
  -pflash /opt/homebrew/share/qemu/edk2-x86_64-code.fd -display none \
  -serial file:/tmp/mac-boot.log \
  -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
  -drive file=<artifact-dir>/disk.img,format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
  -device virtio-tablet-pci -action shutdown=poweroff
grep -E 'Mode: DESKTOP|ICEWM.BIN|XHOBBY.BIN' /tmp/mac-boot.log
```

`hobbyos.elf` is a **boot-time** artifact for symbol work (`addr2line`, `objdump`);
the disk carries a debug-stripped copy at `/boot/hobbyos.elf`, so do not expect the
image's copy to be byte-identical to the pulled one.

## Pitfalls (all measured, not theoretical)

**The shared `hobbyos.elf` is the wrong-arch trap.** `$(TARGET)` is one root file
linked from whichever `ARCH` ran last, and each arch tracks staleness only through
`obj/<arch>/.mode`. An "intel" build in a tree that last built ARM silently keeps the
AArch64 kernel; the guest then hangs at `BdsDxe: starting Boot0002` or boots to a
black screen with no OS output. The skill therefore extracts into a **fresh**
directory and deletes `hobbyos.elf` again on `--reuse`, and `verify_artifacts.sh`
fails the run if the ELF is not `ELF32 / Intel 80386`. **The x64 kernel is
deliberately ELF32/80386 (multiboot1, entry `0x70000030`) — an "ELF64 / x86-64"
answer here is a bug, not a fix.**

**`disk.img` needs a browser and an in-OS LLVM toolchain.** `disk.img` depends on
`browser-staged` and `tools-staged`. Browser: satisfied from
`third_party/webkit-hobbyos/cache/browser-x64.bin.xz` (20 MB, committed) — that is
why the default tarball ships `cache/` but not the 660 MB of `*.part-*` WebKit
snapshot sources (`--full-browser` re-adds them for `make browser`). Tools: the LLVM
`dist-staging` tree is gitignored (227 MB) and **cold-builds LLVM in hours**, so the
default is `TOOLS_SKIP=1` — the image simply ships without `/TOOLS`. Opt in with
`--with-tools`, which rsyncs `third_party/llvm-21.1.8/dist-staging/` once into
`$HOME/hobbyos-x64-builds/cache/` and links it per build. Never let a "missing
tools" build run unattended expecting it to finish.

**`libc.a` truncated under parallel make (a real repo race, fixed here).** Cold
parallel builds used to die with
`ld.lld: error: obj/intel/libc.a: Archive::children failed: truncated or malformed
archive (offset to next archive member past the end of the archive...)`.
`third_party/icu-78.3/build-target.sh:201` runs a **nested**
`make obj/$(ARCH)/libc.a` when the archive is absent, so the outer `make -jN` and the
inner make wrote the same archive at once. The Makefile now publishes the archive
atomically (`ar rcs $@.$$$$ $^ && mv -f $@.$$$$ $@`). If you see this error again,
some *other* archive is being written in place — fix it the same way, don't just
lower `-j`.

**`ssh linuxci` must not be left to DNS.** `linuxci` has a **stale DNS record
pointing at 192.168.0.39**, which is not this machine. `~/.ssh/config` pins
`Host linuxci → HostName 192.168.0.25`; keep that pin, or pass `--host
192.168.0.25`. Symptom of forgetting: `Permission denied (publickey)` or a timeout
against a box you have never touched.

**`scp` does not expand `$HOME` or `~` on the remote side** (OpenSSH ≥9 uses SFTP).
`scp host:'$HOME/x'` copies a literal `$HOME/x`. Always pass absolute paths
(`/home/sarah/...`) or `-O`.

**Non-interactive ssh inherits a bare `PATH`.** `linuxci` is fine (`/usr/bin`), but if
the builder ever needs a keg-only tool, prefix it explicitly; on the Mac side,
harness-spawned shells do not pick up Homebrew's `ld.lld`.

**A green link is not a good build.** Every run without `--no-boot-check` proves the
image reaches the icewm desktop; a build that links but never gets past `BdsDxe`
still exits non-zero here.

**Cold X11 builds want the internet unless you ship `third_party/x11/dl/`.** That
cache is gitignored (30 pinned tarballs, 25 MB) and without it
`third_party/x11/fetch.sh` curls x.org mid-build. Measured failure: `curl: (35) Recv
failure: Connection reset by peer` on `xcb-proto` → `[x11] ERROR: build.sh did not
produce .../XHOBBY.BIN`. The skill ships it (plus ICU's source/data archives), so a
build with no outbound network still succeeds. If you add another vendor stage with
a download cache, add it to `EXTRA_INCLUDES` in the script.

## Reading a failure

| Symptom | Cause / fix |
|---|---|
| `ERROR cannot ssh to linuxci` | key not installed for `sarah` (password login is disabled) — see `references/linuxci.md` |
| `missing on linuxci: meson` | `sudo apt-get install -y meson` (X11/pixman stage, fails ~5 min in) |
| `missing on linuxci: xkbcomp` | `sudo apt-get install -y x11-xkb-utils xkb-data` |
| `x11-staged ... XHOBBY.BIN` missing | the X11 server build failed; read `third_party/x11/build/intel/logs/*.log` |
| `fetch: ... curl: (35) ... reset by peer` | X11/ICU download cache missing from the tarball — see `EXTRA_INCLUDES` |
| `truncated or malformed archive` | parallel in-place archive write — atomic-rename the recipe (see pitfall) |
| build ok, boot check red | kernel panic or desktop never came up; inspect the pulled `boot.log` |
| `verification FAILED ... AArch64` | stale shared ELF — rebuild fresh (`rm hobbyos.elf`) |
| `--reuse` found nothing | only workspaces that produced a `disk.img` are reused; seed one with `--keep` |

## Verified state (2026-11-10)

| Check | Result |
|---|---|
| cold build, `-j32`, 48-core builder | 340 s, 0 error lines, 1464 warnings (vendored code) |
| `hobbyos.elf` | ELF32 / Intel 80386, multiboot1, entry `0x70000030` |
| `disk.img` | 1024 MiB, `/boot/hobbyos.elf` + ICEWM/XHOBBY/XEYES at the FAT root |
| KVM boot check | `Mode: DESKTOP (icewm)`, XSESSION + XHOBBY + ICEWM + XEYES alive, 0 panics |
| same image booted **on the Mac** | `Mode: DESKTOP`, ICEWM.BIN + XHOBBY.BIN launched, 0 panics (TCG on Apple Silicon, so slower; XEYES lags the 300 s window) |

Host facts, dependency installs and access recovery: `references/linuxci.md`.
