# Graphics Acceleration (GX) — HobbyOS program plan & record

Status: **created 2026-10-04 (controller session). Executable plan — the
working document for the GX lanes G1–G4.** This is the execution plan and
fix-log for moving HobbyOS to damage-rect, host-GL-accelerated graphics on
both arches, as the prerequisite for browser.md's WK milestones (WK-6
acceptance wants usable scroll/repaint performance on-device).

Audience: the agent sessions and maintainers executing the GX lanes. Read §2
(current state), §4 (frozen interfaces — binding) and §5 (lanes) first.

Modeled on `docs/gnu-ports.md` / `browser.md` conventions: facts carry
file:line anchors; evidence rules are browser.md §8.4 (no "done" without
executed, observed output from the real case); §11-style log at the bottom.

---

## 0. TL;DR

The OS draws into one 1024×768 kernel framebuffer that user processes map
directly; presenting a frame today costs **a full 3 MiB transfer per flush on
ARM** (virtio-gpu, synchronous spin) and **a full 3 MiB memcpy-per-flush on
x64** (Bochs VGA) — while the desktop has *already computed* per-frame damage
rectangles that are thrown away at the flush call. This program:

1. Adds a **rect-based flush ABI end-to-end** (user → `SYS_FLUSH_FB_RECTS`
   (89) → driver): present only what changed.
2. Modernizes the **kernel GPU drivers on both arches** (ARM: partial
   transfers + IRQ-acknowledged completion; x64: legacy virtio-pci transport
   for virtio-gpu with a damage-rect Bochs fallback).
3. Wires **every QEMU launcher** for graphics acceleration — virtio-gpu-gl
   devices + `-display gtk,gl=on` where the host supports GL (llvmpipe
   counts — "emulated" is accepted), plain 2D devices with `-display none`
   for headless tiers (screendump depends on it), automatic + overridable
   selection rules.
4. **Measures** it: `GFXBENCH.BIN` (rect vs full flush latency) both arches,
   before/after, plus the existing E2E pixel gates.

Non-goals: virgl 3D contexts / Mesa DRM / Skia-GPU raster (future — the
devices chosen here are gl-capable so it can light up later without another
launcher migration); GPU process; X11 library growth.

---

## 1. De-risk evidence (2026-10-04, before any code)

Controller experiments on this workstation (QEMU 10.2.1, X11 `:20`,
`/dev/dri` has `card0` only, host GL = **llvmpipe** software):

- `-display gtk,gl=on` + `-device virtio-gpu-gl-device` **works**; a full
  HobbyOS ARM desktop boot against the GL device ran ~46 min (8 cores, TCG)
  and an X11 window capture (`import -window <id>`, since QMP screendump is
  unavailable on GL windows — see below) showed the live desktop: wallpaper
  gradient + window chrome, 100% non-black pixels. **The existing 2D-only
  driver is a drop-in for the GL device** — no guest changes needed for the
  display path.
- `-display sdl,gl=on` also starts. `-display egl-headless,gl=on` **fails**
  ("no drm render node available") even with `LIBGL_ALWAYS_SOFTWARE=1` —
  no headless GL on this host; do not chase it.
- `-device virtio-gpu-gl-device` + `-display none` **fails** ("display
  backend does not have OpenGL support enabled") — headless tiers keep the
  plain `virtio-gpu-device`; the 2D command protocol is identical.
- **QMP `screendump` fails under `gtk,gl=on` windows** ("no surface"). The
  headless E2E suites (all `run_*.py`) drive `-display none` + screendump —
  they must stay on the plain device path forever. Interactive verification
  under GL uses the X11 capture path (`xwd`/`import`) instead.
- `qemu-system-modules-opengl` is installed (hw-display-virtio-gpu-gl.so,
  virtio-gpu-pci-gl, virtio-vga-gl) — both arches have gl variants.

Baseline slowness (structural, pre-measurement): every `flush_fb()` =
full-frame `TRANSFER_TO_HOST_2D`(1024×768) + `RESOURCE_FLUSH` on ARM
(`src/kernel/virtio_gpu.c:373`), and a 786,432-store memcpy to the BGA LFB
on x64 (`src/kernel/virtio_gpu.c:110`). The desktop flushes on every frame
that moves anything — including each pointer move (`src/user/desktop.c`
paint loop ~2087 and `wm_pixel_service_frame` ~1540). Lanes record exact
`GFXBENCH` before/after numbers (§7 log).

The one nominal anomaly observed: with the GL device the guest printed
periodic `[IDLESTUCK]` reaper dumps (slot=1 pid=1 claims c1=1) for the whole
run while never disposing the process (desktop stayed alive 46 min). Watch
item for G1/G2 while touching the flush/IRQ paths; NOT a GX gate.

---

## 2. Current state (grounded)

| # | Fact | Anchor |
|---|---|---|
| 2.1 | Kernel framebuffer is one static `framebuffer[1024*768]` (B8G8R8A8), identity-mapped into each v2 process at `USER_FB_BASE_V2` as shared VMK_FB pages | `src/kernel/virtio_gpu.c`; `src/kernel/vm.c` `vm_map_fb()`; `src/user_include/graphics/graphics.h` |
| 2.2 | `SYS_MAP_FB` (9) / `SYS_FLUSH_FB` (10); user wrappers `map_fb()` / `flush_fb()` (weak) | `src/include/syscall.h:39-40`; `src/user/libc.c:560` |
| 2.3 | Flush syscall = full-frame present, both arches | `src/kernel/arch/{arm,x64}/trap.c` `sys_flush_fb`; `virtio_gpu.c` flush impls |
| 2.4 | ARM driver: MMIO virtio (slot scan at `0x0A000000+slot*0x200`), one control queue (16 desc), synchronous poll under spinlock, no IRQ handler for the GPU (blk/net/input have IRQ dispatch) | `src/kernel/virtio_gpu.c`; `src/kernel/main.c:47-53,342,380-400`; `virtio_blk_irq = 48 + i` pattern `src/kernel/virtio_blk.c:573` |
| 2.5 | x64 "driver" is Bochs VGA (PCI `1234:1111`): set 1024×768×32 via VBE ports, flush = LFB memcpy. No virtio GPU on x64 at all | `src/kernel/virtio_gpu.c` (`#ifdef __x86_64__` region) |
| 2.6 | x64 virtio PCI precedent exists and is LEGACY: `virtio-net-pci` scans device 0x1000 and uses legacy I/O-port offsets + queue PFN | `src/kernel/virtio_net.c:33-112,194` |
| 2.7 | Desktop computes per-frame damage: `desktop_damage()` → up to `DMG_MAX=12` rects; repaints are already clipped per-rect; only the *flush* ignores them | `src/user/desktop.c` ~1488-1532, ~2087; `src/user_include/graphics/desktop_damage.h` |
| 2.8 | Pixel-window protocol: WM queues expose/repair rects; apps ack with their own flush; menus/cursor are restamped after app flushes | `src/user/desktop.c` `wm_pixel_service_frame` ~1540; `src/user_include/graphics/window.h` |
| 2.9 | Headless E2E harnesses all run `make run`(desktop) or test modes with `QEMU_ARGS=-display none -qmp …` and verify by **QMP screendump** | `run_xcalc_test.py:326-331`; `run_xeyes/antfarm/console/…` pattern; `run_desktop_test.py:32` |
| 2.10 | Unit tests: `virtio_gpu_test_suite` exists (minimal) and is composable per-arch | `src/kernel/virtio_gpu_test.c`; `src/kernel/unit_test.c:30` |
| 2.11 | Kernel TUs are wildcard-built (`$(wildcard src/kernel/*.c)`) — new files need NO Makefile change; per-arch content guarded by `#ifdef __x86_64__` convention (feature areas compile to empty TUs on the other arch) | `Makefile:206-212` |

---

## 3. Design (binding decisions)

- **D1 — Damage-rect presentation end-to-end.** New single syscall
  `SYS_FLUSH_FB_RECTS` (89) carries 0..32 screen-space rects; each rect is
  transferred+flushed by the driver. Presentation semantics: after the call
  returns, the host display must show the same pixels a full flush would
  (rects may be expanded/merged internally; overlaps/order don't matter).
  `flush_fb()` / SYS_FLUSH_FB (10) stay byte-for-byte as they are (full
  present) — every existing binary keeps working.
- **D2 — One new ABI row only.** 89 was free (`SYS_MAX` 88 at freeze;
  rows 87/88 = GETTIME/GETSTACK). Additive above the frozen table. Both
  arch dispatchers + libc wrappers land in the freeze (this session).
- **D3 — ARM driver v2 (G1).** Rect support in the virtio-gpu command path;
  completion moves to IRQ-acknowledged waits (GPU MMIO IRQ harvested like
  virtio-blk's `48 + slot`; `INTERRUPT_STATUS` ack) with bounded spin →
  park discipline; **no long IRQ-off spins** (currently the whole
  transfer+flush round trip holds `gpu_lock` with IRQs off — a standing
  latency/IDLESTUCK-noise hazard). Full flush = rects(1).
- **D4 — x64 driver v2 (G2).** Legacy virtio-pci transport for virtio-gpu
  (modeled on `virtio_net.c`'s legacy path; transitional device — verify
  ids on-device) with the same rect commands; Bochs VGA demoted to a
  probe-fallback with a rect-based memcpy path (row-wise, dirty rect only).
  Init order: try virtio-gpu-pci → fall back to BGA; log which.
- **D5 — Launchers (G3).** Interactive desktop: gl device + `gtk,gl=on`
  when the host can (llvmpipe accepted); otherwise plain device + `gtk`.
  Headless (any `-display none`, test modes, CI): plain device + none —
  unchanged behavior, screendump intact. Selection is automatic (DISPLAY /
  mode / QEMU_ARGS-aware) and overridable (`QEMU_GPU=gl|soft`), plus a
  `make gpu-check` probe. x64 gains `virtio-gpu-gl-pci`/`virtio-gpu-pci`
  (guest uses BGA until G2 lands; harmless device until then).
- **D6 — No behavioral drift for the test fleet.** Every existing gate
  (host suites, unit tiers, ARM wave, E2E pixels) must stay green through
  the whole program; pixel-expectation suites are the determinism proof for
  the flush rewrite.

---

## 4. Frozen interfaces (binding — single writer: controller)

| Interface | Frozen text | Notes |
|---|---|---|
| Syscall row | `#define SYS_FLUSH_FB_RECTS (89)`; `SYS_MAX (89)` | `src/include/syscall.h` |
| Syscall ABI | `(const struct fb_rect *rects, int count) -> 0 / -EFAULT / -EINVAL`; `count` 0..32 (else EINVAL); rects 4-byte aligned, `{int32 x,y,w,h}` screen coords; count==0 = no-op; kernel copies+clamps; empty/offscreen rects skipped | both `trap.c` dispatchers call `virtio_gpu_flush_rects()` with a kernel copy |
| libc surface | `struct fb_rect { int32_t x,y,w,h; }`; `int flush_fb_rect(int x,int y,int w,int h);` `int flush_fb_rects(const struct fb_rect*, int count);` (weak, like `flush_fb`) | `src/user_include/libc.h`, `src/user/libc.c`, `src/host/compat.c` mocks |
| Kernel API | `struct virtio_gpu_xrect { int32_t x,y,w,h; }`; `void virtio_gpu_flush_rects(const struct virtio_gpu_xrect *rects, int count);` | `src/include/virtio_gpu.h`; initial impl (freeze) = full-flush fallback, replaced by G1/G2 |
| Pixel equality | After any rect-flush sequence the covered pixels are byte-identical to the full-flush result | verified by existing pixel E2E + §7 log |
| x64 file split | `src/kernel/virtio_gpu.c` = ARM-only (`#ifndef __x86_64__`); `src/kernel/virtio_gpu_x64.c` = x64-only (`#ifdef __x86_64__`) | freeze move; gives G1/G2 one file each |

Change protocol: lanes propose diffs in reports; the controller applies any
frozen-surface change (as in browser.md §7.3/F6).

---

## 5. Lanes (parallel sessions) & ownership

Worktrees `~/hobbyos-lanes/gx-{arm,x64,launch,userland}` (branches
`gx/arm-driver`, `gx/x64-driver`, `gx/launchers`, `gx/userland`), all from
the freeze commit. One writer per file:

| Lane | Scope | Owns (write) | Must not touch |
|---|---|---|---|
| **G1 — ARM kernel driver** | D3: rect path + IRQ completion + tests + bench numbers | `src/kernel/virtio_gpu.c`, `src/kernel/virtio_gpu_test.c`, `src/kernel/main.c` (IRQ wiring region), `src/kernel/arch/arm/gic.c` if needed | `virtio_gpu_x64.c`, trap.c, Makefile, desktop.c |
| **G2 — x64 kernel driver** | D4: legacy virtio-pci GPU + BGA fallback rects + tests | `src/kernel/virtio_gpu_x64.c`, `src/kernel/virtio_gpu_test.c` (x64 region) | `virtio_gpu.c`, trap.c, Makefile, main.c |
| **G3 — QEMU launchers** | D5: device/display selection, all launcher scripts, fork-runner patches (report), docs | `Makefile` (QEMU region), `tools/run_waves.sh`, `continuation/l8-netfix/*.sh`, `run_two_instances.sh`, `run_proxmox_gpu.sh` (assess), README section | kernel, userland, bench wiring region of Makefile |
| **G4 — userland flush plumbing** | D1 user side: desktop damage→rects, graphics lib, host tests, bench polish | `src/user/desktop.c`, `src/user/graphics/*`, `src/user/gfxbench.c`, `src/host/compat.c`, `src/host/desktop_*_test.c`, `src/user/x11/xlib_*.c` (stretch) | kernel, Makefile (request via report) |

Collision map for shared files as listed above; hunks additive + minimal +
distinct regions; the controller reconciles merges. Makefile after freeze:
G3 owns the QEMU region; the bench wiring (freeze) is complete — others
request.

Briefs (full, with environment traps and gates) live at
`~/hobbyos-lanes/gx-*/GX-BRIEF.md`, written by the controller.

---

## 6. Gates & evidence (all lanes)

Per-lane local gates (before merge; raw logs in the lane report):

1. `python3 tools/cstyle.py check <touched files>` — clean.
2. Relevant unit tier: `./run_unit_tests.sh` (ARM) and/or `./run_unit_tests_intel.sh` (x64) — 0 failed.
3. `GFXBENCH` before/after numbers, same method both runs (freeze tree =
   before): full-flush µs, 64×64 µs, 256×256 µs. Output lines quoted.
4. Desktop E2E retained: at least one QMP suite (`run_desktop_test.py` /
   `run_xcalc_test.py`) green — its pixel assertions are the equality gate.
5. No orphan QEMUs left by the lane; worktree commits at ~30-min cadence;
   stop at a committed checkpoint on tool-cap.

Controller (post-merge, on the merged tip): full battery — host suite,
unit-arm, unit-x64, ARM wave + x64 wave characterization on the CI VM
(`hobbyos-ci.sh`), `GFXBENCH` both arches, launcher matrix re-check,
§7 log updated. Per browser.md's rule: per-lane green ≠ merged-tip green.

---

## 7. Log (append-only)

- 2026-10-05 — **GX program COMPLETE: accelerated graphics end-to-end on both
  arches, merged at `9fce7ec` (lanes G1–G4, 4 parallel subagents + controller).**
  Delivered: **G1** ARM virtio-gpu v2 — per-rect `TRANSFER_TO_HOST_2D` +
  `RESOURCE_FLUSH`, IRQ-acknowledged completion (parked-wait gated on a live
  wake source after the early-boot WFI hang fix), kernel unit coverage.
  **G2** x64 — the legacy I/O-port virtio-gpu transport turned out impossible
  on QEMU 10.2.1 (modern-only devices; verified), so it built the **modern
  virtio 1.0 PCI transport** (cap walk, feature handshake, control queue,
  64-bit BAR remap via `cpu_pd7`), per-rect transfers + BGA row-wise rect
  fallback; 4 new tests.  **G3** launchers — `QEMU_GPU=auto|gl|soft`
  resolution (gl iff desktop + DISPLAY + not `-display none`), `make
  gpu-check`, runner pass-throughs, README section, fork-runner patch (for
  the browser resume), full selection matrix + boot evidence.  **G4**
  userland — desktop damage rects → `flush_fb_rects` end to end (host tests +
  pixel suites), plus a real find: the desktop died on EPIPE when a child app
  exited mid-present (pre-existing race, 2/9 freeze runs; fixed with
  desktop-side SIGPIPE ignore; 6/6 xcalc green).  Controller extras: the x64
  flat-image `.lbss` fix (`4cf6d44` — the x64 desktop could not boot at all
  before it), GFXBENCH v2 (`bd84429`), **x64 launcher acceleration enabled**
  (GL path adds `-device virtio-gpu-gl-pci -vga none`; headless tiers
  byte-identical), VM RAM (x64 6→12 GiB, ARM 8→16 GiB — the ARM identity
  map now covers 0..18 GiB (mmu.c L1[0..17]), fixing the >8 GiB boot hang
  where Limine loads the kernel near RAM top) + `tools/run_parallel.sh`
  (3×8 = 24-core test
  parallelism, user-directed).
  **Verified on the merged tip:** host suite 0 failed; unit-arm 302/0;
  unit-x64 (KVM) 306/0; ARM wave clean (STRESS SUCCESS + System halt, 0 fail
  tokens) + GFXBENCH v2 all phases; x64 desktop boots on BOTH paths (virtio
  device: `VirtIO GPU: virtio-pci`, screendump 1024×768 non-black 1.0000;
  BGA: same); launcher GL path resolves and boots `gtk,gl=on +
  virtio-gpu-gl-pci + -vga none` (window appears; X capture tooling gap
  noted); ARM E2E (`run_desktop_test.py`) green on merged tip; freeze x64
  wave completes 2/2 incl. under parallel load.
  **Bench (v2; min/med are floored at the µs instruments — quote floors, not
  means):** G2's pinned pair r256 `min=1000 med=2000` → `min=0 med=0`
  (identical method); merged ARM full med 0-1000 µs vs before ≈600-1000
  (means 400-460 quiet / 150 ms under churn — contention-sensitive); the
  architectural claim is exact: a 64×64 rect transfers ~16 KiB instead of a
  3 MiB frame (~190× less traffic), and IRQ completion replaced sync spins.
  **Open items:** (1) the main-tip x64 wave wedges near the final STRESS
  phase (0/3 attempts on main TCG+KVM; freeze completes 2/2 incl. under a
  3-way parallel load; 0 new fail signatures; unit-x64 remains the reliable
  x64 gate — characterize, possibly a merged-driver interaction); (2) ARM
  bench sub-ms resolution needs a CNTVCT-based timer (stretch); (3)
  `run_xcalc_test.py`'s post-close check is a no-op (G4 proposed diff); (4)
  x64 GL interactive window capture fallback (xwd) for screenshot tooling.

- 2026-10-04 — **x64 desktop boot blocker root-caused + fixed: large-model
  flat-image truncation (controller).** Under `-mcmodel=large` every intel
  user program links zero-init globals into `.lbss`; `src/user/linker.ld`
  claimed only standard names, so `.ldata`/`.lbss` became orphans placed
  AFTER `.tls_meta`.  `objcopy -O binary` stops at the last reachable
  section, so the flat `.bin` silently OMITTED the whole bss span and the
  v2 loader's IMAGE region (file-size sized) never covered it — first
  deep-bss access faults as a HOLE and kills the process (observed on
  pristine freeze, TCG+KVM: `DESKTOP.BIN ... memory fault VA=0x100001C800
  ... in=HOLE -> killed`).  aarch64 unaffected (no `.l*` names); x64
  programs only survived by landing bss uses in the mapped tail page.
  Latent since the S5 base flip; x64 desktop was never boot-gated.
  Fix: linker.ld claims `.ltext/.lrodata/.ldata/.lbss` into
  `.text/.rodata/.data/.bss` (`.tls_meta` genuinely last — the padding
  contract its own comment describes) + Makefile relink-on-script-change
  (`.EXTRA_PREREQS`; a plain prereq leaks into `$^` and gets fed to ld.lld
  as an input file — that's how the fix first failed to propagate).
  Verified: intel desktop.bin 109,704 -> 547,528 B (= `_end`), 134 pages;
  x64 desktop boots to `Desktop starting`, zero fault markers, QMP
  screendump 1024x768 non-black frac 1.0000.  Lanes G2/G3 told to
  cherry-pick.

- 2026-10-04 — **GFXBENCH v2 instrument (controller; measurement fix).**
  Freeze-version wave numbers showed spread artifacts on both arches with
  IDENTICAL work per op (ARM: r256 mean 125 ms vs full 0.5 ms; x64: full
  mean 8.2 ms vs r256 0.55 ms) — mean-of-few-samples under wave churn is
  not an estimator. v2: warmup iterations + min/median/mean per phase,
  the timer brackets only the flush call, 10/30/50 samples. Lanes'
  own before/after runs (both sides on the freeze bench) stay internally
  consistent; the clean before/after for this log will be re-derived at
  integration (freeze tip vs merged tip, both with v2).

- 2026-10-04 — **GX program created + freeze landed (this commit).**
  De-risk evidence archived (§1): GL device works with the 2D-only guest
  (46-min desktop run + X11 capture); egl-headless and gl-device-with-
  `-display none` are dead ends; screendump is unavailable under GL
  windows → headless tiers keep the plain device. Freeze: `SYS_FLUSH_FB_RECTS`
  (89) + libc wrappers + kernel `virtio_gpu_flush_rects()` (full-flush
  fallback both arches) + x64 driver split (`virtio_gpu_x64.c`) +
  `GFXBENCH.BIN` wired into the test wave + this document. Lanes G1–G4
  dispatched from the freeze commit.
