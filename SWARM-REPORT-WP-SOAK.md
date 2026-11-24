# SWARM-REPORT — WP-SOAK: regression soak of the merged tree (`b3edb2a2`)

Lane `wp-soak` · branch `nv/wp-soak-regress` · worktree
`/Users/sarah/Documents/GitHub/hobbyos-nv/wp-soak` · build/run box `sarah@192.168.0.250`
(c4, 32 vCPU, `/dev/kvm`, QEMU 10.2.1) · RimWorld instrument on `root@192.168.10.174` (r1,
host only — VM 300 was not touched) · 2026-11-24 05:40–06:40 UTC.

Tags: **MEASURED** = I ran it and read the number · **SOURCE** = read from the tree or a
commit · **INFERRED** = reasoning, not observed. Every count below is quoted from the run
log it came from; the logs are on the c4 box under `/srv/nv/wp-soak/obj/soak/`
(`intel-1..3.log`, `arm-1..3.log`, `xptr-run3.log`, `desktop-intel-*.log`) and
`/srv/nv/soak-base5cc/obj/soak/` for the base control.

## 0. Verdict

The merged tree behaves: **6/6 unit-tier runs green with identical counts**, the intel
desktop boots and paints (frame looked at), the ARM desktop and unit links both pass the
relocation gate, the X11 click path delivers **3/3 presses to the client**, and the
RimWorld instrument dismisses the Steam modal with **3 presses** on the *merged kernel*
and measures **50.51 fps median** on the menu it reveals.

One real defect was found and fixed — not in the merge, in the **fixture that grades it**:
`tools/gate/xptr_click.py` could not report a pass on this tree at all (its verdict regex
does not accept the `to_container=` field its own client prints), so the click gate exited
1 while the client had received every press. That is exactly the class a single green run
in another lane hides, and it is committed here as a fix plus a QEMU-free `--self-test`.

## Red/green table

| # | check | merged `b3edb2a2` | base `5cc95b9d` (control) | verdict |
|---|---|---|---|---|
| 1 | intel unit tier ×3 | **584 / 0 PASSED** ×3 (60 s, 58 s, 56 s) | 583 / 0 PASSED | **green, no drift** |
| 2 | ARM unit tier ×3 | **468 / 0 PASSED** ×3 (191 s, 127 s, 133 s) | 465 / 0 PASSED | **green, no drift** |
| 3 | `[RELOC]` boot row, ARM ×3 | `entries=0x1A1 applied=0x1A1 slide=0x3FA400000 outside=0 status=0 ok` — **identical in all three** | row absent (no pass) | **green** |
| 4 | ARM unit link reloc gate | `reloc gate OK -- 417 kernel pointer word(s), every one applied at boot` | **0 gate lines**, `Type: EXEC` | **green** |
| 5 | ARM desktop link reloc gate | `reloc gate OK -- 411 …`, `Type: DYN` | 0 gate lines, `Type: EXEC` | **green** |
| 6 | ARM `MODE=test` link reloc gate | `reloc gate OK -- 411 …` | n/a | green (across-build count, §4) |
| 7 | intel desktop boot + screendump | menu ready **5.0 / 6.0 / 6.0 / 5.0 s**; frame **looked at** — empty desktop, Apps button, clock, pointer | 6.0 / 6.0 / 6.0 / 6.0 s, same picture | **green** |
| 8 | X11 click path (`xptr_click.py`) | **presses=3 releases=3 motions=3 to_container=0 rc=0**, gate `PASS` on two boots, exit 0 (captured on the repeat) | not run (needs the fixed libX11) | **green** |
| 9 | the committed click gate's *grader* | **red on the merged tree**: `client_rc=-`, `xptr: FAIL`, exit 1, with `presses=3 … rc=0` in the same log | — | **defect found** (§3) |
| 10 | RimWorld instrument (`n1_ptr_menu_measure.py`) on the merged kernel | modal `0x0e40610827f1e4d3` → menu `0xcb920d0f8c49a201` after **3 presses**; `[MENU-FPS] median=50.51 p10=48.54 p90=51.81`; 2 dumps byte-identical | — | **green** (§5) |
| 11 | `.bss` (ARM desktop) | `__bss_end=0x41525D10` | `__bss_end=0x41525CE8` | **+0x28 = +40 B**, all five `[RELOC]` counters (§6) |
| 12 | console rows | one `[BLKQ] mmio` row | two `[BLKQ] mmio` rows | timing sample, not code (§6) |
| 13 | Apps-menu contents | 4 entries fewer (`BROWSER.BIN`, `HOME.HTM`, `TALL.HTM`, `DEJAVU.TTF`) | present | **harness provisioning**, not the merge (§6) |

## 1. The three intel runs and the three ARM runs (MEASURED)

Same tree, three back-to-back runs each, `fresh_disk` before every run (the tier script
does that itself) and the run's own `qemu.log` copied out before the next run overwrote it:

```
intel run 1 rc=0 elapsed=60s     Tests run: 584   Tests failed: 0   UNIT TESTS PASSED
intel run 2 rc=0 elapsed=58s     Tests run: 584   Tests failed: 0   UNIT TESTS PASSED
intel run 3 rc=0 elapsed=56s     Tests run: 584   Tests failed: 0   UNIT TESTS PASSED

arm   run 1 rc=0 elapsed=191s    Tests run: 468   Tests failed: 0   UNIT TESTS PASSED
arm   run 2 rc=0 elapsed=127s    Tests run: 468   Tests failed: 0   UNIT TESTS PASSED
arm   run 3 rc=0 elapsed=133s    Tests run: 468   Tests failed: 0   UNIT TESTS PASSED
```

Elapsed time includes the (incremental) build, not just the boot: ARM run 1 paid a
re-link/recompile after the arch switch, runs 2–3 did not. **No run printed a different
count, and no run printed an `EXPECT_* FAILED` row** (`grep -c` = 0 in all six logs).

The test-name sets are also identical run to run (sorted `Running test_*` names diff
empty for intel 1↔2, 1↔3 and for ARM 1↔2, 1↔3) — a count that did not change because the
same tests ran, not because a different set happened to total the same.

**Where 584/468 come from, against the base control** (MEASURED, name diffs):

- intel merged − base = exactly one suite banner: `Running test_nv_vram_scan_and_judge`
  (WP-A4's BAR1 instrument). `nv_test.c`/`nv_vram.c` are `#ifdef __x86_64__` (SOURCE:
  `src/kernel/nv_test.c:19`, `src/kernel/nv_vram.c:27`), so x86-only: **583 → 584**.
- ARM merged − base = exactly three: `test_reloc_folded_rodata`,
  `test_reloc_real_table`, `test_reloc_pass_stats`. **465 → 468.**
- The four round-4 branches therefore add **no intel-visible test**; the intel +1 is the
  BAR1 lane that rode along in the same 13-commit range (`git log 5cc95b9d..HEAD`).

Arch safety: `hobbyos.elf` is one file shared by both arches, so before each arch switch
the tree got `rm -f hobbyos.elf .arch-stamp` and the ELF was identified before the run
(`file`: `ELF 64-bit … AArch64, static-pie` for ARM; `ELF 32-bit … Intel 80386` for intel —
the intel kernel is genuinely ELF32 and objcopy'd from the x86-64 link, SOURCE
`Makefile:1092`).

## 2. The relocation gate, on three different ARM links (MEASURED)

```
MODE=unit_tests : .rela.dyn=417  applied 417/417  outside=0   reloc gate OK -- 417 …word(s), every one applied at boot
MODE=desktop    : .rela.dyn=411  applied 411/411  outside=0   reloc gate OK -- 411 …word(s), every one applied at boot
MODE=test       : .rela.dyn=411  applied 411/411  outside=0   reloc gate OK -- 411 …word(s), every one applied at boot
```

Both ARM links are `ELF 64-bit … static-pie` / `Type: DYN`; the base tree's ARM links are
`statically linked` / `Type: EXEC` and print **no gate line at all** (0 occurrences of
`reloc gate` in its build log). So the gate is new with the merge and it passes on every
ARM link this soak built.

The 417 vs 411 gap is not drift: the unit image contains the three new reloc tests, whose
own folded tables are 6 pointer words (`g_reloc_probe` 3 + `g_reloc_names` 3, per
`SWARM-REPORT-WP-RELOC.md` §5) — **417 − 411 = 6**, and the boot row's `entries=0x1A1`
(417) agrees with the gate on the unit image.

## 3. The click path — and the gate that could not pass (MEASURED, defect)

`tools/gate/xptr_click.py` boots the intel desktop (`q35,vmport=on` + `virtio-tablet-pci`,
`DESKTOP=hobby`), launches `tests/fixtures/ptr/xptr.c` from the Apps menu with key events,
moves the pointer into the client's own rect and sends three press/release pairs over QMP.

**Run A, committed bytes of the merged tree:** the client received everything and said so

```
[XP] ButtonPress n=3 button=1 x=240 y=161 win=0x200002 state=0x0
[XP] ButtonRelease n=3 button=1 x=240 y=161 win=0x200002
[XP] done presses=3 releases=3 motions=3 exposes=1 to_container=0 rc=0
[INFO] motion=True press=True release=True client_rc=-
xptr: FAIL — no button event reached the client
```

The gate's verdict parser required `exposes=(\d+) rc=` **adjacent**, but the client prints
`exposes=… to_container=… rc=…` (`tests/fixtures/ptr/xptr.c:183`). `dm` was therefore
`None` and a run in which every press arrived was graded FAIL. Reproduced offline on the
recorded row (no QEMU): old regex → `NO MATCH`; exit code of run A → 1.

Not the merge's doing — the client and the gate came from the same commit `f0a091f4`, so
the WP-PTR report's own "PASS" transcript (`presses=3 … to_container=0 rc=0`) cannot have
been produced by the committed grader; a green run there hid it. That is this project's
rule 5 (an observation that could not have been negative) in an instrument, again.

**The fix committed here** (`tools/gate/xptr_click.py`): parse the verdict row tolerantly
(`name=value` pairs, require `presses`, `rc`, `to_container`), grade on
`presses ≥ 1 && rc == 0 && to_container == 0 && ButtonPress row && ButtonRelease row`, and
add `--self-test`, which runs the parser and the grade on five literal rows with no QEMU
(green 3-press row, pre-fix red row, `to_container=3`, an older client row without
`to_container`, and a log with no verdict row). Self-test: **5/5 PASS** (MEASURED).

**Runs after the fix (two boots):**

```
[INFO] motion=True press=True release=True presses=3 to_container=0 client_rc=0
xptr: PASS — a compat X11 client received ButtonPress/ButtonRelease      (rc=0)
```

So post-merge the desktop does receive clicks, and the evidence for it is now a gate that
can also say no.

## 4. Desktop boot and screendump (MEASURED, and looked at)

Intel `MODE=desktop DESKTOP=hobby`, `-display none -enable-kvm -qmp unix:…`, readiness is
the kernel's own `[MENU] count=` row; three `screendump`s 1.5 s apart, saved as PPM and
converted to PNG.

```
[soak] menu ready=True in 5.0s   → desktop-intel-merged-0/1/2.ppm (1920×1080, 6,220,817 B each)
```

**The frame was looked at (vision, not a log line):** a light-grey desktop, a full-width
panel, the **Apps** button bottom-left, a digital clock bottom-right reading `06:00:37`, a
black pointer at centre — no windows, no error text, no garbage, no panic. Frame 0 vs
frame 2 differ in **46 pixels of 2,073,600**, bounding box x 1881–1894, y 1063–1069 — that
is the clock's seconds digit and nothing else, i.e. the desktop is live and the capture is
not torn.

Base control: same picture (vision: identical empty desktop, clock `06:07:24`), menu ready
in **6.0 s** (4 samples each: merged 5.0/6.0/6.0/5.0, base 6.0/6.0/6.0/6.0 — equal within
the 1 s poll granularity; **no measurable boot slowdown**).

## 5. The RimWorld instrument, on the merged kernel (MEASURED)

`tools/gate/n1_ptr_menu_measure.py` needs a staged RimWorld image, OVMF and a GL-capable
host; r1 was **idle** (`pgrep -af '[q]emu-system-x86_64'` empty, load 0.23) so it ran, but
*not* against someone else's image: `disk-ptr.img` was copied to `/root/soak/disk-soak.img`
and the merged tree's intel desktop kernel was staged into it —

```
obj/intel/hobbyos.boot.elf            sha256 36eab792…b25017   (merged build, 3,834,428 B)
::/boot/hobbyos.elf extracted back    sha256 36eab792…b25017   ← the guest booted this kernel
```

VM 300, the 4090 and the n1 lane's fixtures were not touched (the run is virgl/llvmpipe on
the host, `egl-headless,gl=on` + `virtio-gpu-gl-pci`, exactly the round-4 stack).

```
[LOADER] first crc 0xd0cb6fbecb358d2c at t=66s            loading art
[FRAME]  t=842s new crc 0x0e40610827f1e4d3                 the Steam modal (round 3/4's CRC)
[PRESS]  t=865s rounds=1 presses=3 crc_off_before=0x0e40610827f1e4d3 crc_off_after=0xcb920d0f8c49a201 rate=49.0
[DISMISSED] t=865s … after 3 presses
[DUMP]   p-menu1-873s.raw  p-menu2-873s.raw   sha256 59eb50f8…e061b4  (both, 8,294,400 B)
[MENU-FPS] window=+20s..+170s intervals=725 median=50.51 p10=48.54 p90=51.81 min=33.44 max=53.76
[TOTAL] presents=38220 guest_ms=960141
```

The menu CRC is **the same `0xcb920d0f8c49a201`** WP-PTR reported, on a different kernel,
and the two dumps are byte-identical. Looked at with vision: RimWorld's main menu —
*New Colony / Load Game / Options / Mods / Credits / Quit to OS* — with no overlay. Median
**50.51 fps** against the ≥30 gate (WP-PTR measured 51.02 on its own kernel; 0.5 fps apart
is the same present loop).

## 6. What the unit tiers cannot see (MEASURED / SOURCE / INFERRED)

- **`.bss` moved by exactly 40 bytes.** ARM desktop `__bss_end` 0x41525CE8 (base) →
  0x41525D10 (merged) = **0x28**. That is the five `u64` counters the relocation pass keeps
  (entries, applied, slide, outside, status — 5 × 8 B). Headroom under `linker.ld`'s
  `__bss_end <= 0x45600000` is still ~68.7 MB, so nothing is near the ceiling.
- **A console row changed count, not shape.** `[BLKQ] mmio …` printed twice in the base
  boot and once in the merged boot (`n=2 cpu=0 ms=…`, plus a second transition row in
  base). The source is byte-identical between the trees (md5 `828470a7…` both sides); the
  row is emitted per state transition sampled by a 100 Hz timer, so this is a sampling
  difference, not a deleted print (SOURCE + INFERRED from identical source and one boot
  each way).
- **The Apps menu lost 4 entries in this harness** and therefore every index shifted by
  one: `BROWSER.BIN`, `HOME.HTM`, `TALL.HTM`, `DEJAVU.TTF` are present in the base image
  and absent from the merged image. Not the merge: `Makefile`'s browser rule has **0
  hunks** in `5cc95b9d..HEAD`, and the staging is `if [ -f obj/$(ARCH)/browser.bin ]`. The
  merged image was built in `/srv/nv/wp-soak`, whose `third_party/webkit-hobbyos` does not
  exist (nvswarm never syncs `third_party`), and the browser cache that resolves it is a
  *tracked* repo file (`git ls-tree 5cc95b9d third_party/webkit-hobbyos/cache/browser-x64.bin.xz`).
  The base control was built from `git archive 5cc95b9d`, which carries it. Consequence for
  humans: a fixture that hardcodes a menu index would break here; both fixtures read
  `[MENU]` from the dump, so they do not.
- **Boot time did not move** (5–6 s to `[MENU] count=` on both trees, 4 samples each).
- **`ET_EXEC` → `ET_DYN`** on both ARM modes, and Limine still loads and jumps to it with
  the same entry point and slide `0x3FA400000` — the unverified half of the RELOC report's
  §6 ("desktop ARM was link-checked but not booted") is now at least booted in
  `MODE=unit_tests` three times and in `MODE=desktop` (link only).

## 7. Flakes

**None observed.** Six unit runs, zero failures, zero count differences, and the `[RELOC]`
row is bit-identical across all three ARM boots. The two single-sample differences above
(`[BLKQ]` row count, browser image content) are explained by timing and provisioning
respectively, not by nondeterminism in the kernel tests. Had a run printed a different
count or a different failure set, it would be in this section with its log — that is the
one thing this lane exists to catch.

## 8. What is NOT verified

- **`t3_tier.sh` (the ~45 min userland battery) was not run**, on either arch. The intel
  half is red on this host with *and* without port work (proc/fat write-lock wedges under
  TCG), and the ARM battery is the lane-sized run this soak did not buy.
- **Desktop behaviour beyond "it paints"**: no app was launched from the menu on ARM, and
  the intel desktop was only screendumped (the click path is covered by the fixture, the
  rest of the UI is not).
- **The RimWorld run measured one boot.** One dismissal, one 150 s window; it does not
  prove the menu is clickable through (New colony → world gen), which WP-PTR named as its
  own next step.
- **Base control is one run per tier** (583 intel / 465 ARM). Enough to attribute the
  count deltas by test name; not a base soak.
- **`bad_types` / `RELOC_ST_ALIGN` paths** remain unexercised (WP-RELOC §6), and the
  x86_64 link still has no relocation gate.
- **The `[BLKQ]` sample-count difference** is explained by reasoning from identical source,
  not by instrumenting the transition counter on both trees.

## 9. Files

| path | what |
|---|---|
| `tools/gate/xptr_click.py` | **fixed**: tolerant verdict parsing, `to_container` and `presses` in the verdict, `--self-test` (5 cases, no QEMU) |
| `SWARM-REPORT-WP-SOAK.md` | this report |

## 10. Next step

1. Run `tools/gate/xptr_click.py --self-test` in CI next to any change to
   `tests/fixtures/ptr/xptr.c` — one second, no QEMU, and it would have caught this.
2. The ARM desktop is link-gated but has never been **booted** with a screendump; that is
   the cheapest remaining gap in the RELOC lane's own evidence.
3. If the shared c4 trees are meant to ship a browser, warm
   `third_party/webkit-hobbyos` there (or stop passing `BROWSER_SKIP=1` when building
   desktop images); today the same command yields a 4-entry-shorter Apps menu on that box
   than on a fresh `git archive`.
