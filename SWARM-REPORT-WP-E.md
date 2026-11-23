# SWARM-REPORT-WP-E — the present gate (PLAN.md §3 E, rung N0)

Branch `nv/wp-e-present`, worktree `wp-e`, cut from `ce4c44af`.
Claim tags: **MEASURED** = a log line or a dump byte in this tree's evidence;
**SOURCE** = read in the code; **INFERRED** = reasoning over the two.

## 1. Outcome in four lines

* A full 1920x1080 B8G8R8A8 frame (**8,294,400 B**) mmaps from `/dev/fb0` and
  reaches the scanout: the pixel at **(1919,1079) is `(255,0,0)`** in a QMP
  screendump, and its 64x64 neighbourhood is the magenta block the probe drew
  around it — **MEASURED** (`evidence/nv/n0-postfix-1919x1079.ppm`).
* The defect that *was* in the tree is the slot overrun the plan named: the
  mapping was **10,485,760 B (2560 pages)** over an **8,294,400 B (2025 pages)**
  framebuffer. Fixed, and pinned by a kernel unit test, a host test and a
  guest-side guard (**MEASURED** three ways).
* **The recorded `-ENOMEM` did not reproduce on this tree.** Before the fix,
  both a native and a Linux-ABI (`musl` static-PIE) process mmap'd all
  8,294,400 B successfully (**MEASURED**, §3). So "it fits in the slot" was not
  what was failing here; the slot overrun was. §6 says exactly which
  configurations I did *not* cover, so nobody reads this as "the old log was
  wrong".
* Unit tier 513 → **515 tests, 0 failed**; host clamp suite 40 checks, 0 failed;
  ARM compile-only (§7).

## 2. The failing observation before the fix (MEASURED)

Two, because they are the two halves of the claim.

**(a) Kernel unit tier — the mapping is the slot, not the frame.** With only the
new test added (`test_vm_map_fb_frame`, `src/kernel/vm_test.c`):

```
  Running test_vm_map_fb_frame...
EXPECT_EQ FAILED: (int)(r->len / FRAME_SIZE) == 2025 at src/kernel/vm_test.c:3284
  Left:  2560
  Right: 2025
Tests run:    514
Tests failed: 1
UNIT TESTS FAILED
```

2560 pages = `USER_FB_SIZE` (10,485,760 B); 2025 = one display frame. The extra
**535 pages (2,191,360 B)** are whatever the linker placed after the kernel's
`framebuffer` static (`virtio_gpu_x64.c:72`) — mapped read/write into user space.

**(b) In the guest — the overrun is reachable from user space.** `FBPROBE.BIN`
forks a child that reads one word at mapping + 8,294,400:

```
[FBP] PAST_FRAME read COMPLETED (defect) value=0x0000000000000000
[FBP] past_frame_status=1792                      (0x700 = child exit 7)
[FBP] RESULT FAIL past_frame (slot maps past the frame)
```

`value=0` is not the point — the point is that the read *completed*; one page
past the framebuffer was mapped. (`evidence/nv/n0-prefix.serial.log`.)

In the same pre-fix boot the frame itself was already correct: `mmap_va=
0x16F0000000`, readback PASS, and the dump's corner pixel `(255,0,0)`.

## 3. The `-ENOMEM`, reproduced or not (MEASURED, and this is the honest part)

PLAN §4 and `docs/nv-games-state.md` §7 record
`[LCOMPAT] mmap fail fd=6 len=8294400 off=0 -> -12`. I tried to reproduce it
twice, pre-fix, on this tree, in a QEMU guest (`tools/nv/present_gate.py`):

| probe | pre-fix result | post-fix result |
|---|---|---|
| native `/dev/fb0`, 8,294,400 B (`MODE=fb_test`) | `mmap_va=0x16F0000000`, `RESULT PASS mmap` | same |
| Linux-ABI musl static-PIE, same call (`MODE=fb_compat_test`, `SDLTEST.ELF`) | `sdl test app: fb /dev/fb0 fd=3 mode=1920x1080 bpp=32` | same |

**Neither path returned -ENOMEM, before or after the change.** What I can say
with evidence: `vm_map_fb()` never sized anything from the caller's length
(there was no length), so *any* successful call overshot by 535 pages; and the
only two `-ENOMEM` sources inside it are `vm_region_insert()` and the
`vm_map_page()` loop (SOURCE). The historical log came from a *desktop* boot
with virgl and (for `fd=6`) a process with more descriptors than either of my
probes — that configuration is **not covered here** and I did not claim it.

## 4. The change

| file | what |
|---|---|
| `src/kernel/vm.c` | `vm_map_fb_len(grp, fb_phys, want)`: the region and every leaf are sized by the clamp, not by `USER_FB_SIZE`; `vm_map_fb()` = one whole frame; the `/dev/fb0` branch now passes the **translated** window (`mlen`) instead of dropping it; `vm_map_fb_obj()` uses the same clamp |
| `src/include/vm.h` | declares `vm_map_fb_len()`; the slot comment now says a frame is mapped, not the slot |
| `src/kernel/fb_mmap.c` | `hb_fb_map_len(want, image_len, slot_len, &out)` — the one clamp: `round_up_pages(min(want, image_len))`, refused if > `slot_len`. Pure, no device, both arches |
| `src/include/fb_mmap.h` | its contract, with the 8,294,400 / 10,485,760 numbers and why |
| `src/kernel/vm_test.c` | `test_vm_map_fb_frame`: 2025 pages, region == `HB_FB_BYTES`, last leaf == last framebuffer page, **first page past the frame absent**, idempotent |
| `src/kernel/fb_mmap_test.c` | `test_map_len`: the clamp cases in the EL1 tier |
| `src/host/fb_mmap_test.c` | `test_map_len`: the same arithmetic on the host (3 of these rows are the red/green pair in §5) |
| `src/user/fbprobe.c` | the probe: open/mmap/paint/readback + the `past_frame` fork guard |
| `src/kernel/main.c`, `Makefile` | `MODE=fb_test` (probe alone, no desktop) and `MODE=fb_compat_test` (the musl ELF alone); `FBPROBE.BIN` on every disk |
| `tools/nv/present_gate.py` | boots the mode with a QMP socket, waits for the probe's row, `screendump`s, and reads the pixels out of the PPM |
| `evidence/nv/` | the three dumps, the two serial logs, the pixel read-out |

The clamp in one line (`src/kernel/fb_mmap.c`):

```c
if (image_len > slot_len) return -1;
if (want > image_len) want = image_len;          /* never past the framebuffer */
len = round_up_pages(want);  if (len > slot_len) return -1;
```

## 5. Red → green, three tiers

**(a) kernel unit tier** (the arithmetic and the mapping together):

```
before: Tests run: 514 / Tests failed: 1 / UNIT TESTS FAILED     (Left 2560, Right 2025)
after:  Tests run:    515
        Tests failed: 0
        UNIT TESTS PASSED
```

`test_vm_map_fb_frame` and `test_map_len` both print in `evidence/nv/unit-tier-515.log`.

**(b) host suite** (the same arithmetic, no kernel):

```
old rule (clamp to the slot):  fb_mmap_test_host: 40 checks, 3 failed
  FAIL: clamp: a slot-sized request is clamped to the image (2025 pages)
  FAIL: clamp: the result is 2025 pages, never the slot's 2560
  FAIL: clamp: one page past the image still maps the image
new rule (clamp to the image):  fb_mmap_test_host: 40 checks, 0 failed
```

The "old rule" run is `/tmp/fb_mmap_oldrule.c` — the same TU with
`want = image_len;` replaced by `want = slot_len;`, i.e. exactly what
`vm_map_fb()` used to do. Both runs are kept:
`evidence/nv/host-clamp-red.log` and `evidence/nv/host-clamp-green.log`.

**(c) guest guard** (`PAST_FRAME`, one word past the last frame byte):

```
before: past_frame_status=1792 (child exited 7)  RESULT FAIL past_frame
after:  past_frame_status=11   (SIGSEGV)         RESULT PASS past_frame (page past the frame is unmapped)
```

## 6. The screendump and the pixel (MEASURED)

```
make MODE=fb_test ... run QEMU_ARGS="-display none -qmp unix:...,server=on,wait=off"
QMP  screendump {"filename": "/tmp/wp-e-n0-postfix.ppm"}
```

* dump: P6, 1920x1080, 6,220,800 data bytes (`[N0] dump 1920x1080 (6220800 bytes)`).
* `(1919,1079) = (255,0,0)`; `(1918,1079) = (1919,1078) = (255,0,255)`;
  `(0,0) = (960,540) = (32,48,64)`.
* the bottom-right 64x64 is **4095 magenta + 1 red** — the red pixel is the only
  one in the frame and it is the framebuffer's last address.
* looked at, not just parsed: `n0-postfix-small.png` (the magenta corner block on
  the fill) and `n0-postfix-corner-zoom2.png` (96x96 nearest-neighbour: the two
  fill regions with a hard boundary, and a single distinct pixel at the corner).

**Could the observation have been negative?** For this *pixel* alone, no — it is
green before the fix too (§2b), which is why the gate is not just the pixel: the
`past_frame` row and the unit tier carry the "the slot cannot walk past the
framebuffer" half. A mapping clamped short would read `(32,48,64)` at the
corner; a mapping that never reached the scanout would read the BGA
clear colour everywhere.

## 7. Gates, exact

```
$ nvswarm sh wp-e 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
Tests run:    515
Tests failed: 0
UNIT TESTS PASSED

$ make fb_mmap_test_host && ./fb_mmap_test_host
fb_mmap_test_host: 40 checks, 0 failed

$ python3 tools/nv/present_gate.py --mode fb_test
[N0] corner (1919,1079)  = (255, 0, 0)     want (255, 0, 0)     OK
[N0] block  (1918,1079)  = (255, 0, 255)   want (255, 0, 255)   OK
[N0] block  (1918,1078)  = (255, 0, 255)   want (255, 0, 255)   OK
[N0] fill   (0,0)        = (32, 48, 64)    want (32, 48, 64)    OK
[N0] fill   (960,540)    = (32, 48, 64)    want (32, 48, 64)    OK
[N0] GATE PASS

$ python3 tools/nv/present_gate.py --mode fb_compat_test
[N0] serial: sdl test app: fb /dev/fb0 fd=3 mode=1920x1080 bpp=32
[N0] GATE PASS
```

**ARM: compile-verified only.** Every source this branch touches compiles for
`aarch64`: `obj/arm/{vm,vm_test,fb_mmap,fb_mmap_test,main,fbprobe}.o` all built
under `ARCH=arm MODE=unit_tests` (§ the coordinator's gate update). The full ARM
**link** is broken at `ce4c44af` for an unrelated, pre-existing reason —
`ld.lld: error: kernel .bss exceeds the limine placement window` — and that
reproduces on an untouched tree, so no ARM boot was attempted and none is
claimed. The shared `mm` change is therefore compile-verified on ARM, measured
on x86_64.

## 8. What is NOT verified

* **The historical `-ENOMEM`** (§3): not reproduced, native or Linux-ABI, on
  this tip. The desktop/virgl + hosted-client configurations it came from are
  not covered by either probe.
* **The real virtio-gpu scanout / the 4090.** This boot's scanout is QEMU's
  **BGA** path (`VirtIO GPU: BGA`, `evidence/nv/n0-postfix.serial.log`) — the
  same static `framebuffer` the 4090's present path will one day fill, but the
  pixels reached the host through QEMU's VGA LFB, not through the card. Nothing
  here is a claim about AD102.
* **virgl / `-device virtio-gpu-gl-pci`**: untested in this lane.
* **Partial-window mmaps** are mapped at their requested length by the clamp
  (host-tested) but no guest run exercised a sub-frame window end to end.
* **ARM boot** (§7).

## 9. Next step

1. Re-run the **m4 gate** (`tools/gate/m4_gl_probe.py`) against this tip: it is
   the configuration that produced the `-12` row, and it now would fail loudly
   if the compat fb0 readback still refused. If it ever returns `-ENOMEM` again,
   `vm_map_fb_len()` has exactly two failure points (region insert, page loop) —
   put one `[FB-MAP]` row on each and the answer is one boot.
2. Then N1 (RimWorld's menu on virgl at 720p), which is the next rung this
   branch unblocks.

## 10. One harness note (cost me a run)

`nvswarm sync` uses `rsync -a`, which preserves mtimes. Restoring a source file
from a backup (`cp` of an older copy) leaves it *older* than the object built
from the wrong version, and `make` then silently reuses the stale object — the
unit tier passed 515/0 while still running the pre-fix `vm.o`. `touch` every
edited file before syncing when you swap a file back in.
