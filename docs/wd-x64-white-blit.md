# WD — x64 windowed browser paints WHITE: root cause is fork-side

Lane WD (OS x64 display path). Branch `x64-fb-flush` (base `b8e69a9`).
Status: **OS side exonerated with on-device evidence; bug is in the fork's
WK5WindowDriver paint path (WA's file).** Handoff + OS groundwork below.

## 1. Repro (current disk, KVM q35, virtio-gpu-pci, -vga none, -smp 1)

Boot `disk.img` (ARCH=intel MODE=desktop, browser = wf1b 182 MiB ELF ->
100 MiB flat `::/BROWSER.BIN`), F1 -> Apps menu (preselected BROWSER) -> Enter:

```
[WIN] BOOT mode=windowed start=HOME.HTM
[WIN] created pref=320x240
[WIN] page id=8 viewport=320x240
[WIN] geom x=2 y=34 w=1020 h=706
[WIN] load-ok url=HOME.HTM ms=1957
[WIN] frame view=320x240 nonZero=307200 checksum=0x39878dc5
[WIN] painted rect=1020x706 at 2,34
[WIN] frame view=320x240 nonZero=307200 checksum=0x39878dc5   (repair re-blit)
[WIN] painted rect=1020x706 at 2,34
```

QMP screendump: window chrome correct (focused title bar `(96,166,255)`,
menu bar `(206,208,214)`, taskbar, wallpaper outside the window), but the
whole 1020x706 content rect is pure `(255,255,255)` — exactly 720120 px +
frame ring = 91.6% of the screen, i.e. the white region is EXACTLY the
browser blit rect.

## 2. Root cause: the browser writes WHITE; the OS presents it faithfully

With `WD_X64_FLUSH_PROBE=1` the kernel logs the fb state at every flush:

```
[WD] SYS_MAP_FB pid=1  phys=0x74A00000 v2va=0x16F0000000   (desktop)
[WD] SYS_MAP_FB pid=3  phys=0x74A00000 v2va=0x16F0000000   (browser)
[WD] n=2 rect=(0,0,1024,768) P1=0xFF0E1327 ...               (desktop gradient)
[WD] SYS_FLUSH_FB pid=3
[WD] n=4 rect=(0,0,1024,768) P1=0xFFFFFFFF ...P6=0xFFFFFFFF  (right after the
                                                              browser's blit)
[WD] n=6 ... P1..P6 = 0xFFFFFFFF                              (repair re-blit)
```

- Both processes map the SAME physical framebuffer (0x74A00000) at the v2
  slot 0x16F0000000 — there is no per-process shadow and no lost second
  writer; SYS_FLUSH_FB presents whatever the kernel fb holds.
- Immediately after the browser's own SYS_FLUSH_FB (the flush that follows
  its `paintAndBlit`), the KERNEL fb is `0xFFFFFFFF` at every sampled point
  in the window. The visible scanout shows exactly that (screen == kernel
  fb byte-for-byte, verified at all monitor points and by the title
  bar/gradient being correct in the same frame).
- Therefore the browser (pid 3) *wrote* pure white into the fb, and the OS
  presented its white faithfully. The OS fb + flush + virtio-pci scanout
  path is coherent and works for any writer.

### The fork's own checksum is a white-frame canary, not "render is correct"

`0x39878dc5` is the FNV-1a (offset 2166136261, prime 16777619) of an
ALL-WHITE 320x240x4 frame — computed exactly:

```
FNV-1a(all-white 307200 B) = 0x39878dc5      (= the printed [WIN] checksum)
FNV-1a(HOME.HTM page)      = 0x759431c5      (= the WK-3 FPC in browser.md §11)
```

The old ARM runs printed the same `0x39878dc5` while their *older* binary
still showed the page on screen; the current merged intel build BOTH
readbacks white (checksum) and paints white (fb). Note browser.md §11 WN3:
the merged-tree windowed WebProcess "renders an EMPTY DOM even for fixtures"
— an empty document paints WebKit's default white page. The x64 wf1b build
is the first merged windowed binary tested on-screen; it hits that
regression.

## 3. OS-side proof the display path is not the bug

An OS-only chroma program (no WM, no fork code, a bare second process:
SYS_MAP_FB -> fill -> SYS_FLUSH_FB), booted as `::/DESKTOP.BIN`:

- program: `src/user/wdmap.c` (probe green top half, page bg bottom half,
  white spine at x=512 + band at y=384); build + install commands in its
  header.
- screendump: `(100,200)=(64,160,96)` #40a060, `(900,740)=(16,32,48)`
  #102030, `(512,*)` white — the x64 scanout shows the explicit colors
  pixel-perfect.
- kernel probe `n=2` samples the same pixels at flush time:
  P1/P2/P6=0xFF40A060, P3=0xFF102030, P4/P5=0xFFFFFFFF — kernel fb ==
  screen, and arbitrary second-process blits present correctly.

Combined with the baseline (desktop pid 1 paints title bar / wallpaper /
taskbar that all show), every suspicion in the brief is closed:
1. map_fb/flush are NOT per-process shadow, no second-writer drop — the
   browser's data (white) provably reaches the scanout.
2. scanout format / dual-buffer — desktop colors and chroma colors flow
   through the same TRANSFER_TO_HOST_2D + RESOURCE_FLUSH path and are
   correct; nothing is swizzled or dropped.
3. x64 fb locking/DMA — nothing drops a second writer; byte-coherent.

## 4. Handoff to the fork lane (WA owns WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp)

Boundary: `WK5State::paintAndBlit()` writes the frame it reads back from
Skia into the OS fb. On the current intel build that readback AND the
on-screen result are all-white while the OS presents correctly.

- File: `WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp` (wf1b lane),
  `paintAndBlit()` — the `skImage->readPixels(SkImageInfo::MakeN32Premul(w,h),
  pixels.data(), w*4, 0, 0)` readback / the merged render producing an
  empty DOM (browser.md §11 WN3). The printed checksum `0x39878dc5` must
  become the real page's `0x759431c5` for HOME.HTM.
- Acceptance: on x64, QMP screendump content rect must show #40a060 probe
  on #102030 bg (the ARM old-binary screenshots in
  `wk5/ARM/WK5-4-01-window.png` are the target look); the fb must not be
  `0xFFFFFFFF`.
- OS side is complete and upstreamable as-is; nothing here needs a fork
  change to function.

## 5. OS groundwork shipped in this branch

- `src/kernel/arch/x64/trap.c`, `src/kernel/virtio_gpu_x64.c`:
  `WD_X64_FLUSH_PROBE` (default 0) adds map-fb/flush attribution and kernel
  fb sampling at flush time — re-enable with
  `make ARCH=intel MODE=desktop CFLAGS+=-DWD_X64_FLUSH_PROBE=1 hobbyos.elf`
  to re-audit any future "lost blit" report (also handy for WA to watch the
  browser's own writes land).
- `src/user/wdmap.c`: standalone chroma positive control (full-screen
  known colors, self-verifying at the kernel fb).
- `wd_x64_runner.py`: KVM q35 runner (F1->menu->Enter), drives the browser
  or any Apps-menu app and QMP-screendumps + pixel-samples.
- Evidence runs live in `/tmp/wd-x64-{baseline,probe,clean3}` (PPM + serial)
  and `/tmp/wd-chroma/wm.img` (chroma disk).
