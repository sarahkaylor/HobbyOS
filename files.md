# Files (X11) — a GPL X11 file browser for HobbyOS (plan v2, 2026-11-01)

Status: **v2 — implemented and validated on intel.** The GPL survey in §1 stands
(v1), but its conclusion did not: instead of forking `Worker`, this tree now
has its own GPL-2.0-or-later browser, written against libX11 + Xft.
Source: `src/user/x11/apps/xfiles/main.c` (~1,100 lines). Build:
`third_party/x11/build-clients.sh` → `XFILES.BIN` (2.85 MB). Runs on the
IceWM desktop; screenshots + video in `docs/xfiles-demo/`.

---

## 0. v2 — what shipped

A first-party browser was the v1 runner-up (§1.4, "writing our own") and is now
the pick, because the two GPL alternatives both carried a dependency the tree
does not have: Worker needs C++ exceptions (117 `throw` sites, `std::async` in
7 TUs, no unwinder in `libcxx.a`), and xfm needs libXt/Xaw3d *and* source
surgery against 1990s widget internals. A 1,100-line C program on the
libraries already in `third_party/x11` costs less than either, and it is GPL
by construction.

| Item | Value |
|---|---|
| Source | `src/user/x11/apps/xfiles/main.c` + `README.md` + `COPYING` |
| Licence | GPL-2.0-or-later (matches `README.md:402-406`) |
| Libraries | libX11, libXft, libfontconfig, libfreetype, libXrender — all already in the prefix |
| Image | `XFILES.BIN` 2,854,168 bytes (intel), built by the `clients` stage |
| Features | list view + drawn icons, keyboard/mouse navigation, wheel, scrolling, path prompt, sort (name/size/time), hidden-file toggle, type-ahead, mkdir/rename/delete with a `y/N` prompt, open-in-EDITOR / launch-through-HBHOST, help overlay, resizable window, double-buffered paint |
| Integration | `src/user/x11sess/gen-icewm-menu.sh` — first start-menu entry (`Files (X11)`) plus a toolbar entry; added to the `SKIP` list so it is not re-listed as a hosted native app |
| Test harness | `tools/xfiles_demo.py` — boots qemu, launches the app from the start menu, drives it over QMP, captures screenshots and an MP4 |

**Validation (intel, 2026-11-01, KVM):** boot → IceWM → start menu → Files
(X11) → selection → path prompt (`/X11/FONTS`) → sort → help → `/HOME` →
new folder (`DEMO` created, status `created DEMO`) → open `SHTEST.TXT` in
`EDITOR.BIN` (a second X client, hosted through `HBHOST.BIN`). Evidence:
`docs/xfiles-demo/01..20*.png` and `docs/xfiles-demo/xfiles-demo.mp4`
(35.6 s, 5 fps). Each step is asserted against the app's own stderr log lines,
so the run fails loudly if a key does not land.

### 0.1 What changed from v1

| Item | v1 | v2 | Why |
|---|---|---|---|
| Pick | Worker 5.3.1 (GPL-2+) | **first-party `XFILES.BIN`** | the GPL alternatives each need a missing dependency; this needs none |
| Central work | C++ exceptions decision (patch-out vs unwinder) | **a 1.1k-line C program** | measured: the app is smaller than the fork it would replace |
| New libraries | none | **none** | libX11 + Xft were already built |
| Evidence | library builds, crash backtraces | **booted, driven, screenshotted, recorded** | a working artifact beats a plan |
| Worker | the pick | retired to §1.4 (revisit if a real two-pane manager is wanted) | still the best GPL *maintained* option, still carries exceptions |

---

## 0.2 The v1 selection record (kept: it explains why this is not Worker)

Goal: a **GPL-2-compatible** file manager, as a native X client on the default
IceWM desktop, that the tree can build for both arches without inventing a
toolkit it does not have.

Audience: agent sessions + maintainers. §0.1 is what changed, §1.2 the survey
with measured verdicts, §4 the work, §9 the risks.

---

## 0.3 TL;DR of v1 (superseded by §0)

**The GPL constraint has no cheap answer. The pick is `Worker` 5.3.1
(GPL-2+), because it is the only GPL file manager that needs no new X
libraries at all — and the price is C++ exceptions, which this tree
deliberately lacks.**

Measured on this host, 2026-11-01 (commands in Appendix A):

| Candidate | License | App size | New X libs it needs | What was actually verified |
|---|---|---|---|---|
| **Worker 5.3.1** | GPL-2+ | 172,514 LOC C++20 | **none** | 117 `throw` / 76 `catch` sites; `std::async/future/promise` in 7 files; libc surface is small (`fnmatch`, `getmntent`, `inotify_*`, `chown` are the only real gaps) |
| xfm 1.5.4 | GPL-2+ | 22,375 LOC C | libXt + libXaw3d + libXmu + libSM + libICE (102,942 LOC C) | **Builds** on a modern host with flag relaxations, then **crashes at startup** against Xaw3d 1.6.6: its `TextField` subclass re-declares `XtNfont` and `TextField.c:1879` dereferences a NULL font. Coupled to 1990s Xaw3d private structs. |
| xfe 2.1.13 | GPL-2+ | 55k C++ + 17k C | **FOX toolkit** (241,977 LOC C++) | FOX has 66 `throw`s and 3 `catch`es (error signalling, not control flow) — but it is 240k LOC of new C++ before the first icon |
| Xplorer 0.10.0 | **GPL-3** | 34,982 LOC C++ | none (own `libcxxx`) | **License-incompatible with this GPL-2 tree** (clients link statically against `libc.a`), unmaintained since 2012, and does not compile on a current g++ |
| Xnc, Xplore | GPL-2 | ? | Motif (+Xg) | Motif is a bigger toolkit port than Xaw |
| rox-filer, pcmanfm, emelfm2, spacefm, gentoo | GPL-2+ | large | GTK | GTK is a separate multi-month port |
| ~~xfiles v1.1.1~~ | ~~MIT~~ | 7,891 LOC C | none | **retired in v1** (license only) — its build, symbol inventory and screenshot stand |

**Recommendation: Worker**, with the exceptions decision made explicitly at
M1 (two-track: patch the throws out, or bring up `libc++abi` + a DWARF
unwinder — the latter is a capability the whole C++ userland would gain).
Runner-up: **xfe + FOX** if a fork of an active upstream is unacceptable and a
240k-LOC toolkit port is acceptable instead. **xfm is parked**: it needs source
surgery against a toolkit that does not exist in the tree yet, and it was
measured failing.

### 0.4 What changed from v0 (v1 history)

| Item | v0 | v1 | Why |
|---|---|---|---|
| Must-have | simple, X11, portable | + **GPL, compatible with the GPL-2 tree** | the ask changed |
| Pick | xfiles v1.1.1 (MIT) | **Worker 5.3.1 (GPL-2+)** | xfiles is MIT |
| New libraries | none | **none** (unchanged) | Worker is Xlib-only |
| Main risk | image size, X conformance | **C++ exceptions** (no unwinder in the tree) | Worker throws; `libcxx.a` has no `__cxa_throw` |
| xfiles | the pick | **retired / dormant** — pin kept in §2.2 | nothing about it got worse; it fails the license filter |
| xfm | rejected on paper | **investigated, built, parked** — evidence in Appendix B | a GPL-2 app small enough to be worth checking |
| Evidence | host build + run of xfiles | + xfm build/crash backtrace, + Xt/Xaw3d/Xmu/SM/ICE build, + FOX/Worker metrics, + libc symbol diffs | measurements beat adjectives |

---

## 1. Selection

### 1.1 Requirements

1. **Graphical, X11-native** — a real X client, not curses.
2. **Simple** — small enough for an agent session to own; dependency count is
   the size metric.
3. **Works these days** — builds without a patch stack, upstream alive.
4. **Portable to HobbyOS** — no GUI toolkit the tree does not have, C or
   exception-free C++, POSIX surface the repo libc nearly covers, 8.3-safe.
5. **Exercises the X stack** — a conformance test with a payoff.
6. **GPL, compatible with a GPL-2 tree** — `README.md:402-406` declares
   HobbyOS GPL v2 with no "or later" grant, and clients link **statically**
   against `libc.a`, so app + libc is one combined work. GPL-3-only code
   (Xplorer) is therefore out, not merely inconvenient.

### 1.2 Survey

| Candidate | License | Size | Deps | GUI | Verdict |
|---|---|---|---|---|---|
| **Worker 5.3.1** (2025-11-15) | GPL-2+ | 172,514 LOC C++20 | Xlib; optional Xft/libmagic/Xinerama/lua/dbus/avfs | own, no toolkit | **SELECTED.** Only GPL candidate that adds no library. Cost is exceptions. |
| xfm 1.5.4 (Debian branch of 1.4.3brl; upstream 2001) | GPL-2+ | 22,375 LOC C | libXt, libXaw3d, libXmu (+SM/ICE for Xt) = 102,942 LOC C | Athena/Xaw3d | **Parked.** Builds (verified) but crashes at startup against Xaw3d 1.6.6 — its widget subclasses are written against 1990s Xaw3d private structs. Needs app surgery *and* a toolkit. |
| xfe 2.1.13 (2026-10-25) | GPL-2+ | 55k C++ / 17k C | FOX (241,977 LOC C++), Xft, PNG, JPEG | FOX | **Runner-up.** Alive and feature-complete, but FOX is 240k LOC of new C++. |
| Xplore 1.2a | GPL-2 | ? | Motif + Xg widgets | Motif | Rejected: Motif is larger and more complex than Xaw; FreeBSD marks it broken/unfetchable. |
| Xnc 5.0.4 | GPL-2 | ? | Motif | Motif | Rejected: same Motif problem. |
| Xplorer 0.10.0 (2012) | **GPL-3** | 34,982 LOC C++ | Xlib, Xpm, own `libcxxx` | own | Rejected: **GPL-3 vs GPL-2 tree** (static linking), dormant 14 years, does not compile on a current g++. |
| rox-filer, pcmanfm, emelfm2, spacefm, gentoo | GPL-2+ | large | GTK | GTK | Rejected: GTK port is a separate multi-month project (glib, gdk, pango). |
| ~~xfiles v1.1.1~~ | MIT | 7,891 LOC C | none | own | **Retired (v0 pick).** Fails the GPL requirement; best technical fit of everything surveyed; kept dormant in §2.2. |
| Twander 3.231 | custom "free for all users" licence (not GPL) | 11k C | Xlib | Xaw | Rejected: not a free-software licence. |
| jfm (2026) | MIT | 24 KB C | Xlib | own | Rejected: MIT, four months old, 4 stars. |

### 1.3 Why Worker

- **It is the only GPL file manager whose port adds no library.** Every
  alternative buys its GUI with 100k-240k lines of new code (Xaw3d, Motif,
  FOX, GTK). Worker draws its own widgets on Xlib, so the X side is already
  done — `third_party/x11` provides libX11, Xext, Xrender, Xcursor, Xpm, Xft.
- **It does its own file operations.** No controller script to rewrite (the v0
  plan needed one because xfiles delegates every mutation to a shell script):
  copy, move, delete, mkdir, rename, filters, bookmarks, two panes.
- **Its POSIX surface is small.** Source audit: `fnmatch` (8 files),
  `getmntent` (1), `inotify_init`/`inotify_add_watch` (2), `chown` (4) are the
  only libc functions this tree lacks; no `dlopen`, no `iconv`, no gettext
  calls, no `poll` (it uses `select`, which the libc has).
- **It is alive and licensed to match**: GPL-2-or-later in every source
  header, a release roughly every six months, and a documented configure path
  for every optional dependency we will disable.

### 1.4 Runners-up and revisit triggers

| Runner-up | Revisit when |
|---|---|
| xfe 2.1.13 + FOX | A fork of Worker's 117 throw sites is judged unacceptable and a 240k-LOC toolkit port is preferred. FOX's throws are error signalling (66 sites, 3 catches), so a `-fno-exceptions` fork of it is mechanical. |
| xfm + Xaw3d | Someone wants Xaw3d for its own sake (it unlocks xman/xedit/xbiff/xmag). Then xfm's widget surgery is worth doing — not before. |
| xfiles (MIT) | The GPL requirement is dropped or reinterpreted ("GPL-compatible is fine"). Still the cheapest port measured here: 7,891 LOC, 6 missing libc functions, builds and runs. |
| Writing our own (~2-3k LOC C on libX11+Xft, GPL-2+) | Worker's exception work proves larger than the fork budget and xfe's FOX proves too big. A real option for a hobby OS — the tree already has a native `FILES.BIN` to model the UX on. |

### 1.5 Licenses

- HobbyOS: **GPL v2** (`README.md:402-406`), no "or later" grant → treat as
  GPL-2-only for combination purposes.
- Worker: **GPL-2-or-later** (every source header) — compatible.
- xfm / xfe / Xnc / Xplore / the GTK apps: GPL-2-or-later — compatible.
- Xplorer: GPL-3 → **incompatible** with a GPL-2-only tree under static
  linking. Flag for the maintainer rather than assume.
- Worker's optional pieces (libmagic, Lua, AVFS, D-Bus) are all disabled, so
  their licenses never enter the picture.

### 1.6 Non-goals

- No new GUI toolkit (that is what killed xfm here).
- No D-Bus/udisks/HAL, no AVFS virtual filesystem, no Lua, no translations
  (gettext off) in v1.
- No thumbnails/image previews in v1.
- Not replacing the native `FILES.BIN` on `DESKTOP=hobby`.

---

## 2. Pinned sources & provenance

### 2.1 Active pin

| Source | URL | sha256 |
|---|---|---|
| `Worker-5.3.1.tar.bz2` (1,799,974 bytes) | `http://www.boomerangsworld.de/cms/worker/downloads/worker-5.3.1.tar.bz2` | `ab597b09192c4cdb0c15024d0f4d3bc177b8a4ee70e5b7efa74b7c8f10237047` |

- Release date 2025-11-15 (`ChangeLog`). The project publishes no git history;
  the tarball is the unit of provenance. Upstream also ships `.sig` files —
  pin one and record the key at M0.

### 2.2 Retired pin (kept for the record — the v0 pick)

| Source | URL | sha256 |
|---|---|---|
| `xfiles` v1.1.1, commit `11b23b0a1035fbc6e252a1fbe3e27d1aed649191` | `https://github.com/phillbush/xfiles/archive/refs/tags/v1.1.1.tar.gz` | `a39617f57f5814436dd5926d161ac80a606841a3f909680d99f7d0e734eda8b0` |

### 2.3 Dormant pins (only if the xfm contingency is ever taken)

| Source | sha256 |
|---|---|
| `xfm_1.5.4.orig.tar.gz` (Debian/Ubuntu branch) | `2c4c3f7c4c0473963a1d434303c0cd743f7c6bcb8a153d3aca74303c05f56cc2` |
| `libXt-1.3.1.tar.xz` | `e0a774b33324f4d4c05b199ea45050f87206586d81655f8bef4dba434d931288` |
| `libXaw3d-1.6.6.tar.xz` | `a41c3e37135ad616b1f28bbde70002afbf3cb59a30df34141f829d32eadc8646` |
| `libXmu-1.3.1.tar.xz` | `81a99e94c4501e81c427cbaa4a11748b584933e94b7a156830c3621256857bc4` |
| `libSM-1.2.6.tar.xz` | `be7c0abdb15cbfd29ac62573c1c82e877f9d4047ad15321e7ea97d1e43d835be` |
| `libICE-1.1.2.tar.xz` | `974e4ed414225eb3c716985df9709f4da8d22a67a2890066bc6dfc89ad298625` |

---

## 3. Current-state snapshot (grounding)

All facts checked 2026-11-01 in this tree; anchors are `file:line`.

| Fact | Anchor |
|---|---|
| Default desktop is IceWM (`make`, `make run`); `DESKTOP=hobby` is the old one | `README.md:125`, `:146`, `:195-206` |
| Real X server + static client libs are pinned and built (libX11 1.8.14, Xext, Xrender, Xfixes, Xdamage/Xcomposite/Xcursor/Xrandr/Xinerama/Xpm 3.5.20/Xft 2.3.9 + fontconfig 2.18.3 + freetype 2.14.3; xorg-server 21.1.25) | `third_party/x11/pins/*.txt` |
| **No** Xt, Xaw, Motif, GTK, Qt, FOX, Tk in the pins | `grep -inE 'libxt\|libxaw\|motif\|lesstif\|gtk\|^qt\|fox\|tk-' third_party/x11/pins/*.txt` → none |
| C++ userland is `-std=c++17 … -fno-exceptions -fno-rtti`; no unwinder and no typeinfo runtime in `libcxx.a` | `Makefile:332-343`, `Makefile:354` |
| `libcxx.a` defines no `__cxa_throw` / `_Unwind_RaiseException` (only `__cxa_throw_bad_array_new_length`) | `nm -g --defined-only obj/intel/libcxx.a \| grep -E '__cxa_throw\|_Unwind'` |
| libc++ vendored is **21.1.8**, so C++20/23 language features are available | `third_party/libcxx-21.1.8` |
| Clients link **static** and install as **flat** `.BIN`s through `cross/link-flat.sh` | `third_party/x11/README.md` §Toolchain model |
| Big static X clients already boot: `XEYESR.BIN` 1,110,584 B, `XCALCR.BIN` 1,119,496 B, `ICEWM.BIN` 4,824,648 B | `third_party/x11/build/intel/bin/` |
| The client-side Xlib *shim* (`src/user/x11/`) is a different target: one window per process, 8×8 font, no Xft/Xpm/Xrender, no grabs | `src/user/x11/README.md` §Behaviour notes |
| Loader limits: legacy flat path 1 MB (`USER_INITIAL_CLEAR_SIZE 0x100000`); the v2 exec path used by spawned programs allows `USER_IMG_SIZE` (128 MiB) | `src/include/process.h:42`, `src/kernel/program_loader.c:1073` |
| Userland may use FP/SIMD (F1.5); ARM userland drops `-mgeneral-regs-only` | `Makefile:312-317`, `src/kernel/arch/x64/fpu.c`, `browser.md:2681` |
| libc has `select`, `statvfs`, `sigaction`, `utime`, `symlink`, `readlink`, `lstat`, `setlocale`, `nl_langinfo`; **lacks** `fnmatch`, `getmntent`, `inotify_*`, `chown` | `nm -g --defined-only obj/intel/libc.a` |
| A native file manager already ships: `FILES.BIN` (list view, icons, FAT move-on-drop, NFS mounts) | `src/user/files.c`, `Makefile:2576` |
| IceWM start menu is generated at disk-build time; X clients get explicit `prog` lines; session binaries are skipped | `src/user/x11sess/gen-icewm-menu.sh:48`, `:93-106` |
| Tree declares GPL v2 (no "or later") | `README.md:402-406` |

---

## 4. Gap analysis → the work to schedule

| # | Gap | Size | Note |
|---|---|---|---|
| G1 | **Exceptions.** Worker needs `throw`/`catch` in 30 TUs — 117 throws, 76 catches, concentrated in `hw_volume_manager.cc` (D-Bus/HAL code), `exprfilter_*`, `bookmarkdb*`, `kvpstore`, `pathjumpui`. Two routes: (A) patch out → error codes; (B) bring up `libc++abi` + a DWARF unwinder and build with `-fexceptions`. | **the project's real cost** | B is a capability win for the whole tree; A is a fork of a live upstream |
| G2 | `std::async`/`std::future`/`std::promise` in 7 TUs (`pathjumpui`, `dirbookmarkui`, `persdeeppathstore`, `dmcacheentrynwc`, `worker.cc`, `worker.h`, `virtualdirmode`) | ~7 refactors, or a `<future>` that survives without exceptions | libc++'s `<future>` propagates errors by throwing |
| G3 | C++20 build (`-std=c++20`) plus `-fno-exceptions` **or** `-fexceptions` per G1 | one line in the build recipe | libc++ 21.1.8 is new enough |
| G4 | Disable optional subsystems: D-Bus/udisks/HAL, AVFS, Lua, libmagic, Xinerama, gettext/NLS | configure flags + a few `#ifdef` fixes | shrinks G1 a lot — the biggest throw file is the HAL volume manager |
| G5 | libc: `fnmatch` (8 files), `getmntent` (1), `inotify_*` (2), `chown` (4) | ~150 LOC + 2 stubs | same shape as the xfiles gap; `fnmatch` is worth adding anyway |
| G6 | Config/data layout: `~/.worker` seed, theme/icon files on `disk.img` with 8.3 names | small | Worker writes its config on first run; ship a seed |
| G7 | Integration: `WORKER.BIN` on the disk, IceWM menu entry, `SKIP` list, image-size measurement | small | `WORKER.BIN` is 8.3-safe |
| G8 | Image size: 172k-LOC C++20 binary with libc++ statically linked | measure at M2 | IceWM is 4.8 MB and boots; the v2 exec path allows 128 MiB |

---

## 5. Architecture decisions (binding once approved)

- **AD-1 — GPL-2-compatible only.** GPL-2-or-later apps; no GPL-3-only code
  while the tree is GPL-2-only. (Revisit only with the maintainer's license
  decision — see §10.1.)
- **AD-2 — No new libraries.** If Worker needs something the tree lacks, it is
  a libc/libc++ fix, never a toolkit addition.
- **AD-3 — Explicit fork boundary.** Upstream tag pinned; **every** local
  change lives in `third_party/worker/patches/` as a named patch applied `-p1`
  in order, each with an `upstream-status:` line (upstreamed / local / needs
  rework). A patch must apply to a pristine host build first (two-build rule).
- **AD-4 — Exceptions decided at M1, not discovered at M3.** Two-track spike:
  (A) patch-out build vs (B) `-fexceptions` build; pick on gate evidence and
  record the decision in §11.
- **AD-5 — Seed config, no writes outside `/HOME`.** Static assets go to
  `/WORKER/` with 8.3 names.

---

## 6. Milestones

Gates run on the merged tree; a lane may not tick a gate it did not run.

> **v2 status:** the milestones below describe the v1 (Worker) plan and are
> kept for the record. The shipped work is recorded in §0 and §12: M0-M4 are
> met for **intel** by `XFILES.BIN`; M5 (ARM) is a cross-build check only —
> `bash third_party/x11/build.sh --arch arm --stage clients`.

### M0 — Pin, vendor, freeze
- [ ] T0.1 Fetch + verify the 5.3.1 tarball and its `.sig`; record the key.
- [ ] T0.2 Vendor to `third_party/worker/` (byte-identical) with a `README.md`
      recording pin, patches, configure line.
- [ ] T0.3 Freeze the configure line (AD-4's disabled set) in that README.
- [ ] T0.4 Establish the two-build baseline: pristine Worker built on the host.

**Gate M0:** `git status` shows only the doc + vendored tree; host build of
pristine Worker passes.

### M1 — The exceptions decision (timeboxed spike)
- [ ] T1.1 Host build with `-fexceptions` → list what breaks.
- [ ] T1.2 Host build with `-fno-exceptions` → list every error site; estimate
      patch-vs-unwinder.
- [ ] T1.3 One-TU probe of route B: `__cxa_throw` + a minimal
      `_Unwind_RaiseException` to size the work.
- [ ] T1.4 Record the decision (A or B) in §11 and update §4.

**Gate M1:** a written decision with both error lists attached; no code merged
beyond the probe.

### M2 — Cross build, both arches
- [ ] T2.1 Build Worker against the `third_party/x11` prefix with `cross/env.sh`
      wrappers; link flat.
- [ ] T2.2 libc additions from G5 (+ host tests).
- [ ] T2.3 Record `ls -l WORKER.BIN` for both arches (G8).

**Gate M2:** the build driver emits `WORKER.BIN` for `intel` and `arm`; `nm`
shows an Xlib entry point and a Worker symbol.

### M3 — Integration
- [ ] T3.1 `disk.img` carries `WORKER.BIN` + `/WORKER/` seed config.
- [ ] T3.2 IceWM menu entry (`gen-icewm-menu.sh:93-106`) + `SKIP` list update.
- [ ] T3.3 Default-desktop disk file list unchanged.

**Gate M3:** `mdir` shows the binaries; generated `/ICEWM/menu` has the entry;
`make ARCH=intel disk.img` default file list identical to before.

### M4 — Boots and works (Intel)
- [ ] T4.1 Launch from the menu; screenshot proves both panes and a file list.
- [ ] T4.2 Click-through: enter a directory, select, copy a file to the other
      pane, rename, delete, open a text file in `EDITOR.BIN`.
- [ ] T4.3 Two-minute session with no X errors and no leaked windows.

**Gate M4:** `make run ARCH=intel` + QMP screenshots + serial excerpt.

### M5 — ARM parity
- [ ] T5.1 Same build and checklist under TCG.

---

## 7. Parallel execution plan (sketch — freeze on approval)

| Lane | Scope | Owns | Depends on |
|---|---|---|---|
| L1 `vendor` | M0, M2.1, patch set | `third_party/worker/`, the build-recipe section | — |
| L2 `libc` | G5 (M2.2) | `src/libc/src`, `src/libc/include`, host tests | M0 |
| L3 `cxx` | G1/G2 probe + implementation | `third_party/libcxx-21.1.8/patches/`, the TU patch set | L1 findings |
| L4 `integrate` | M3-M5 gates | `Makefile`, `gen-icewm-menu.sh`, disk recipe | L1-L3 |

One writer per file; the integrator owns `Makefile`, the menu generator and
the disk recipe.

---

## 8. Testing & acceptance

- **Host tier:** pristine Worker builds (two-build rule); libc additions get
  host tests alongside `make x11_lib_test_host`-style targets.
- **In-OS tier:** a unit test is not worth it for a GUI — acceptance is a
  scripted QMP session (`hobbyos-gui-test` skill) driving the mouse and
  asserting on framebuffer pixels at known coordinates.
- **Acceptance (v1 "done"):** both arches build; menu entry launches the app;
  the five M4.2 interactions work; no X protocol errors in the log; the
  default-desktop disk is unchanged.

---

## 9. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | **Exceptions fork debt** — a live upstream releasing ~2×/year, with a ~120-site patch set | prefer route B if the unwinder probe is under ~2 weeks of work; otherwise pin a tag and rebase per release only |
| R2 | `<future>`/`std::async` cannot work without exceptions | refactor to `std::thread` + mutex/condvar in 7 files (design noted in T1.1) |
| R3 | C++20 + libc++ 21 on a freestanding target: `<filesystem>`, `<regex>`, `<chrono>` gaps | Worker uses none of the first two; `std::chrono` appears 11× — verify at M2 |
| R4 | Static image size | measure at M2; IceWM's 4.8 MB proves the loader path |
| R5 | Locale/UTF-8 text rendering in Worker's own widgets | libc has `nl_langinfo`/`setlocale`; verify at M4 |
| R6 | `inotify` (directory watching) does not exist → stale listings | stub to poll-on-focus in v1; record the divergence |
| R7 | Upstream drift/abandonment | GPL-2+, 172k LOC: forking is permitted but expensive — pin releases |
| R8 | D-Bus/udisks removal leaves dead code paths | configure-off + patch set, verified by the host build and M4 |

---

## 10. Open questions (close in M0)

1. **Is the GPL-2 boundary hard?** If GPL-3 app code is acceptable, Xplorer
   (34,982 LOC, no external toolkit) returns to the table as the cheapest
   option by far.
2. **Is a maintained fork of an active upstream acceptable at all**, or would
   "write our own ~3k LOC GPL browser" be preferred over fork debt? If "no
   forks", route B (exceptions bring-up) becomes the only path.
3. Does the guest's keymap path deliver the modifier state Worker's keyboard
   handling expects? Test at M4.
4. Do we want a second file manager on the IceWM desktop at all, or should the
   native `FILES.BIN` be extended instead? (Out of scope, but decide before
   M2.)

---

## Appendix A — Inventories and regeneration commands

**Any C app — real libc gap (host build + symbol diff):**

```sh
make -j && nm -u *.o | awk '{print $NF}' | sort -u > /tmp/app.syms
nm -g --defined-only $REPO/obj/intel/libc.a | awk '{print $NF}' | sort -u > /tmp/libc.syms
comm -23 /tmp/app.syms /tmp/libc.syms     # the actual gap, not a guess
```

**C++ exception dependence:**

```sh
grep -rho '\bthrow\b' --include='*.cc' --include='*.h' . | wc -l
grep -rhoE 'catch\s*\(' --include='*.cc' --include='*.h' . | wc -l
grep -rlE 'std::(async|future|promise)' --include='*.cc' --include='*.h' .
```

Measured this session:

| Subject | Numbers |
|---|---|
| Worker 5.3.1 | 172,514 LOC; 117 `throw`, 76 `catch`; `std::async/future/promise` in 7 files; `std::mutex` 20, `std::atomic` 4 |
| xfm 1.5.4 | 22,375 LOC; 496 undefined symbols of which 130 `X*`; **13** missing: `canonicalize_file_name`, `creat`, `endfsent`, `get_current_dir_name`, `getfsent`, `inflate*` (zlib), `mkfifo`, `mknod`, `setfsent`, `strndup`, `system` |
| xfiles 1.1.1 (retired) | 7,891 LOC; **6** missing: `scandir`, `fnmatch`, `ftello`, `open_memstream`, `pipe2`, `posix_spawnp` |
| FOX 1.6.57 | 241,977 LOC C++; 66 `throw`, 3 `catch` |
| libXt 1.3.1 | 40,146 LOC C; needs `reallocarray`, `stpcpy` |
| libXaw3d 1.6.6 | 38,619 LOC C; needs `tmpnam` |
| libXmu 1.3.1 | 13,987 LOC C; needs `reallocarray` |
| libSM 1.2.6 | 2,746 LOC C; needs `uuid_generate`/`uuid_unparse_lower` (libuuid) |
| libICE 1.1.2 | 7,444 LOC C; nothing beyond Xlib |

---

## Appendix B — Evidence trail for the rejected candidates

**xfm 1.5.4 (GPL-2+) — built, then parked.** On a current host (GCC 14/15,
X.Org 11.0):

- All five toolkit libraries build from pristine tarballs with no patches
  (ICE, SM, Xt, Xmu, Xaw3d) — 103k LOC of C, `--prefix` install.
- xfm builds after three flag-level relaxations (`-fcommon`,
  `-Wno-implicit-function-declaration`, `-Wno-incompatible-pointer-types`) and
  a link-order fix (`--no-as-needed`; the 2001 Makefile puts `-l` flags before
  the objects).
- It then **segfaults at startup**: `TextField.c:1879` in `Initialize()`,
  reached from `createInstallPopups()` → `createPopupQuestions()`; the widget's
  font is NULL and `MAX_FONTH()` dereferences it. Xt also warns
  *"Representation size 8 must match superclass's to override font"* — xfm's
  `TextField` subclass re-declares `XtNfont` against a modern Xaw3d `Text`
  class. The crash survives overriding every font resource
  (`Xfm*Font`, `Xfm.iconFont`, …, `Xfm*fmLog*AsciiSink.font`).
- Conclusion: coupled to 1990s Xaw3d private headers (`TextP.h`, `CoreP.h`,
  `IntrinsicP.h`), so it needs source surgery *on top of* a 103k-LOC toolkit
  port. Not "simple".

**xfiles v1.1.1 (MIT, retired).** Builds clean and runs (screenshot of a
populated directory under Xvfb); 6 missing libc functions. Excluded by licence
only — if the GPL requirement is ever relaxed it is still the best port here.

**Xplorer 0.10.0 (GPL-3).** 34,982 LOC, own `libcxxx` toolkit, no external GUI
deps — technically the closest thing to "xfiles but GPL". Fails on licence
(GPL-3 vs GPL-2 tree), age (2012), and build (`CImage.h:266` returns a
`std::ifstream` as `bool`, rejected since C++11).

**XFile 1.2.1 / xfe 2.1.13.** Both alive and well-maintained (2026 releases),
but XFile needs Motif and xfe needs FOX — a toolkit port either way.

---

## 11. Fix log (append-only)

- 2026-11-01 — **v0**: survey, pick `xfiles` v1.1.1 (MIT); host build + run
  evidence; libc audit (6 missing functions); milestones M0-M5; risks R1-R8.
- 2026-11-01 — **v1**: license constraint added (must be GPL-2-compatible, per
  `README.md:402-406` + static linking). Re-surveyed. Worker 5.3.1 (GPL-2+)
  selected — the only GPL candidate that adds no library; C++ exceptions
  (117 throw / 76 catch, `std::async` in 7 files, no unwinder in `libcxx.a`)
  becomes the central cost, decided at M1. xfm (GPL-2+) investigated in depth:
  toolkit libraries build, app builds, app crashes against Xaw3d 1.6.6 →
  parked with evidence. Xplorer eliminated (GPL-3). xfe+FOX kept as runner-up.
  xfiles retired; its pin, inventory and evidence kept for the record.
- 2026-11-01 — **v2**: wrote the first-party browser instead of forking
  Worker. `main.c` ~1,100 lines, GPL-2.0-or-later, libX11 + Xft only;
  `clients` build stage; IceWM menu/toolbar integration; QMP demo harness.
  In-guest validation on intel: navigation, selection, path prompt, sort,
  help, mkdir (`DEMO`), and opening `SHTEST.TXT` in `EDITOR.BIN` through
  `HBHOST.BIN` — evidence in `docs/xfiles-demo/`. ARM cross-build produces
  `XFILES.BIN` (2,731,568 bytes); boot validation pending.
  Two defects found by the in-guest run and fixed: (1) Xft resolved no font
  because `FcInit()` was never called, so the app silently fell back to the
  console bitmap font — now explicit, with `xfiles: xft font ok` on stderr;
  (2) the demo's QMP socket was shared by the recorder thread and the driver,
  interleaving responses — now locked.

---

## 12. Implementation record (v2)

Files added or changed, all in one pass:

| Path | Change |
|---|---|
| `src/user/x11/apps/xfiles/main.c` | new, ~1,100 lines, GPL-2.0-or-later |
| `src/user/x11/apps/xfiles/README.md` | feature list, build recipe, HobbyOS notes |
| `src/user/x11/apps/xfiles/COPYING` | GPL-2.0-or-later notice |
| `third_party/x11/build-clients.sh` | new `clients` stage; builds `XFILES.BIN` per arch |
| `src/user/x11sess/gen-icewm-menu.sh` | start-menu entry (first row) + toolbar entry + `SKIP` |
| `tools/xfiles_demo.py` | QMP-driven demo: boot, launch, drive, capture PNG/MP4 |
| `docs/xfiles-demo/` | 20 screenshots + `xfiles-demo.mp4` |

Design notes worth keeping:

- **Drawing, not widgets.** Everything (rows, icons, scrollbar, caret, help) is
  painted into one backing pixmap and blitted on expose — no toolkit, no theme
  files, no flicker. Text goes through Xft into the same pixmap.
- **Font fallback chain** `$XFILES_FONT` → `DejaVu Sans-12` → `sans-11` →
  core `fixed`, with `FcInit()` first (see §11, 2026-11-01 v2 entry).
- **Process launch** is `fork()` + `execvp("/HBHOST.BIN", {"/HBHOST.BIN",
  "/EDITOR.BIN", path, NULL})` so editor instances get a virtual display the
  same way the hosted launcher gives them one.
- **Geometry on stderr** (`xfiles: geom WxH+X+Y`) is what lets the demo harness
  click exact rows; the session already relays client stderr to the console.

Measured: intel `XFILES.BIN` 2,854,168 bytes; arm 2,732,736 bytes; both link
only against libraries already in the prefix.
- 2026-11-01 — **v2.1**: third defect found by the verified run — rename
  reloaded with `keep_selection` against the *old* name, so the selection fell
  back to `..` and a following Delete silently did nothing (it even exited the
  directory on Return). `select_name()` now re-finds the new name after a
  rename, and the new folder is selected after `mkdir`. The demo harness was
  rewritten to assert each step on the stderr state log (`xfiles: cwd/prompt/
  mkdir/open/rename/delete`) and to retry a key that does not land; it is
  idempotent on `disk.img` (creates `DEMO`, renames to `DEMO2`, deletes it).
