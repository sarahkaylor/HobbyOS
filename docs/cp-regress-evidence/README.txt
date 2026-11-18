C10-regress: does integ11 still do what HobbyOS could already do?  (docs/cyberpunk.md section 8.3)
=====================================================================================================

Trees
  L = integ11 13dc693b, plus this lane's three commits where the row says "+fix"
      (Mac ~/hobbyos-cp-lanes/regress, branch cp-regress; strix ~/cp/lanes/regress)
  R = main 3c1cb61f, the main checkout's HEAD (git -C ~/Documents/GitHub/HobbyOS-cyberpunk fetch
      hobbyos-main main; git worktree add --detach ~/hobbyos-cp-lanes/regress-ref-main 3c1cb61f;
      strix ~/cp/lanes/regress-ref).  main @ b1f3ef56 (what wave 5 merged) + 5 commits: the
      RimWorld x64 render-probe driver, the in-colony battery, and 1ccc102a (gpu_abi 1-D
      transfers > 16 KiB).
Host: strix (Ryzen Zen 5, 32 threads), QEMU 10.2.1, KVM for x86_64, TCG for aarch64.
      Every boot: one QEMU slot (tools/cp/qemu-slot.sh), guest RAM clamped to 12 GiB, inside a
      private PID + mount namespace (the repo GUI harnesses run `pkill -9 -f qemu-system-*`; in
      the namespace that only reaches their own QEMU), private /tmp, -display gtk|sdl -> none.
      Exact commands: commands.txt; the wrapper scripts: harness-scripts.txt.

Regression matrix
-----------------
Verdicts: OK = integ11 at least as good as main; FIXED = red on integ11, fixed here;
SAME = same failure on both trees, not a regression; n/c = not comparable (reference could not
run); NOT RUN = see "Could not run".

| # | Row | R (main 3c1cb61f) | L (integ11) | Evidence | Verdict |
|---|-----|-------------------|-------------|----------|---------|
| 1 | AVX / low-VA / GS probes | (probes are Cyberpunk-era) | PASS (integ11 batch, integ-gates14.txt) | probes-avx-lowva-gs.txt | OK |
| 2 | Doom ELF, doomgeneric, KVM -cpu host | PASS, 14/14 frames distinct, 12.3 % non-black, 173 colours | PASS x2, 14/14 distinct, 12.3 %, 172 colours | doom-elf.txt | OK |
| 3 | Doom ELF, -cpu qemu64 (FXSAVE path) | PASS (main's harness has no DOOM_ELF_CPU knob: default KVM model) | PASS, [FPU] xsave=0 size=512, 14/14 distinct | doom-elf.txt | OK |
| 4 | RimWorld to its main menu (virgl, renderD129, L86 driver) | PASS: Steam modal dismissed at +646 s, main menu at +649 s after launch (same driver behaviour) | PASS: Steam modal dismissed at +65 s, main menu on screen at +66 s (OCR: Tutorial, Options, Credits, Quit; New colony visible in the screendump) | rimworld.txt | OK (10x faster) |
| 5 | RimWorld in-colony battery (main b8d79881) | NOT RUN | NOT RUN | rimworld.txt | NOT RUN |
| 6 | IceWM desktop, x86_64 (run_icewm_test.py: X up, taskbar, start menu, CONSOLE typed input, XCALC clicks, F4 + close button; real-Xlib xeyes autostarts) | PASS (session +36.9 s) | PASS (session +22.2 s) | desktop-gui.txt, x64-icewm-*.png | OK |
| 7 | desktop_test x86_64 | PASS | PASS | desktop-gui.txt | OK |
| 8 | ARM boot to the native desktop (desktop_test, TCG, -m 12G) | PASS | PASS (EDITOR + Open File dialog, arm-desktop-test-integ11.png) | desktop-gui.txt | OK |
| 9 | ARM desktop apps 13 launches (run_desktop_apps_test.py) | n/c (QMP socket never appeared, x2: the harness' 60 s QMP wait covers its own disk.img rebuild) | PASS 13/13 | desktop-gui.txt | OK |
| 10 | ARM games / antfarm / console | games PASS; antfarm, console n/c (QMP wait) | PASS / PASS / PASS | desktop-gui.txt | OK |
| 11 | ARM xeyes | n/c (QMP wait, x2) | 1 check red: "Apps menu covered (panel pixels=0)"; the screendump shows the menu drawn on top of XEYES; the panel is (255,255,255), the test expects (232,234,240). desktop.c and run_xeyes_test.py are identical on main -> stale test colour | desktop-gui.txt | SAME (test) |
| 12 | x86_64 nano | PASS | PASS | desktop-gui.txt | OK |
| 13 | x86_64 xcalc (native desktop, intel click path) | FAIL "XCALC never started" | FAIL, same line | desktop-gui.txt | SAME |
| 14 | x86_64 apps_test | FAIL at FIND.BIN (7/10) | FAIL at FIND.BIN (7/10), 6/10 before it PASS | desktop-gui.txt | SAME |
| 15 | x86_64 filedialog_test | FAIL, 60 s timeout | FAIL, 60 s timeout | desktop-gui.txt | SAME |
| 16 | x86_64 pong_test | FAIL, 60 s timeout (build inside the budget) | PASS (47 s); a rerun timed out the same way as R | desktop-gui.txt | OK |
| 17 | ARM files_nav / xcalc / nano (native desktop) | not run (R's ARM harness boots all hit the QMP wait) | n/c: QMP wait / boot-marker budget | desktop-gui.txt | n/c |
| 18 | x86_64 kernel unit tier | 448 run, 4 failed (badvec x3, irq-mask budget) | 463 run, 2 failed (virtio_gpu_test.c:214 :244) -> +fix 463 / 0 | unit-tiers.txt | OK, FIXED |
| 19 | ARM unit image links (linker.ld __bss_end <= 0x45600000) | FAIL (same ASSERT) | FAIL: __bss_end 0x4570cdb0, 1101232 B over -> +fix 0x4558cda0 | unit-tiers.txt | FIXED |
| 20 | ARM kernel unit tier (TCG, -m 12G) | with the window fix (scratch): hangs in test_fs_record_locks, timeout | +fix: 436 run / 0 failed (before the GPU-row fix: 3 failed, incl. gpu_abi_m2_test.c:64) | unit-tiers.txt | FIXED |
| 21 | host tier (make -k host_tests) | 37/41: gaming.md x2, panic_sink_allowlist, tools/rw/rw_gameplay_test.py (auto-discovered, wants a live VM) | 38/40: gaming.md x2 -> +fix 39/41 (+spawn_reap_test PASS) | host-tier.txt | OK (gaming.md x2 SAME) |
| 22 | host tier leaves ./EDITOR.BIN_host asleep under init | leaks (same code since 4a613c71) | leaks -> +fix: 0 left, guard red before / green after | host-tier.txt | FIXED |
| 23 | main's glibc-2.31 ABI probes (HELLO, DIRPROBE, MMAPRO, MAPFSYNC, MUNMAPWB, LOCKHOLD pair) | all markers, same rows (HELLO DONE, DIRPROBE done, 3 VERDICT PASS, LOCKHOLD 8 s then 1 s) | all markers: HELLO DONE, DIRPROBE done, 3 VERDICT PASS, LOCKHOLD 8 s holder then waiter | main-abi-probes.txt | OK |
| 24 | ARM build (default) | rc=0 | rc=0 (+fix too) | runs-index.txt | OK |
| 25 | castscan (gate 6) | - | 371 sites, 0 new | runs-index.txt | OK |

Commits on cp-regress (all test or host-mock code; no kernel code outside KERNEL_MODE_UNIT_TEST)
  f307b9f5 unit: unix_test scratch from the frame pool -- the ARM unit image fits the placement window again
  f85026a3 unit: GPU rows follow the 512 MiB backing cap; the M2 routing row is per-arch
  1686d13e host: spawn2() children die with the host test -- make host_tests no longer leaks ./EDITOR.BIN_host
  (+ this evidence commit)

Bisect
  - ARM unit image over the window: red on main 3c1cb61f as well (same ASSERT), so it came in
    with main, not with Cyberpunk.  Largest unit-only statics: unix_test.c's three ring buffers
    (1.5 MiB).  Pushed over by main's growth; nothing to bisect on the Cyberpunk side.
  - gpu_abi_m2_test.c:64 on ARM: ac113440 (cp/venus, C6-venus), masked because the ARM unit
    image never linked after the main merge.  0940b579 (P7) already guarded the sibling row.
  - virtio_gpu_test.c:214/:244: main's 53a315cc (64 MiB -> 512 MiB cap), never the tests.
  - EDITOR.BIN_host leak: 4a613c71 (2026-05, main), not Cyberpunk.
  - No row is red on integ11 and green on main: nothing to bisect into 058fd3d5..13dc693b.

Gates for this lane's commits
  The three kernel-side files are entirely inside #ifdef KERNEL_MODE_UNIT_TEST: clang -E of each
  without it is 0 tokens, so every non-unit kernel (probes, ext2/exec/pfx gates, P2, P5, P8) is
  the integ11 one byte for byte; compat.c is host-test code only.  Run on cp-regress:
  1 build (intel MODE=test, rc=0); 4 unit x86_64 463/0, ARM 436/0; 5 host 39/41 (gaming.md x2);
  6 castscan 371/0 new; 7 ARM build rc=0; check-no-game-files (cp and rw): OK.
  Gates 2, 3, 8, 9 and P2/P5/P8 not rerun: unchanged binaries (see above); integ-gates14.txt
  stands for them.

Notes on how to read the rows
  - "QMP socket never appeared" / 60 s timeouts: the native-desktop harnesses start QEMU
    through `make run`, whose disk.img rebuild (FORCE_ARCH relinks hobbyos.elf, disk.img
    depends on it) runs inside their 60 s QMP wait.  Under strix's load (4 slots, 6 lanes)
    that wait is often too short.  Building first (prebuild column of chain.sh) fixes it
    only partly, because the relink still rebuilds the image.  Not a product signal.
  - The strix lane trees' .git is a stale clone: in such a tree t4_compat_selftest reports
    "the flip modified src/host/compat.c" for ANY edit to compat.c.  The host row for +fix was
    therefore run in a clean-git snapshot (strix ~/cp/regress-work/hostcheck).

Could not run
  - RimWorld in-colony battery (main's tools/rw/rw_gameplay_test.py, not on integ11): it needs a
    live VM already parked in a colony.  Neither tree's L86 driver gets past the main menu
    ("no_target New colony": the button is on screen, tesseract does not read it), and
    run-rimworld-desktop.sh kills QEMU as soon as the driver returns, so there is no live
    colony VM to point it at on either tree.  Not a kernel signal.
  - Doom linuxdoom-1.10 (`make doom_elf_test`, depth-8 PseudoColor): needs the id sources
    (DOOM10_SRC, not in git).  The only doom10.elf on strix is in the user's Trash; not used.
    doomgeneric (third_party/doomgeneric) was built from the tree instead.
  - ARM `make test` (the T3 wave): the full T3 wave is banned (wave6-common); not run on ARM
    either.  ARM coverage here = unit tier + the native-desktop harnesses.
  - OpenJDK sdktest / java.desktop: needs third_party/jdk-27 (a download); not on strix.
  - Gates 2/3/8/9 + P2/P5/P8 on cp-regress: not rerun, binaries unchanged (see Gates).
  - R's ARM native-desktop harness boots (antfarm, console, xeyes, desktop_apps): the QMP wait
    expired on every attempt, so those rows are integ11-only.

Facts for other lanes / the integrator
  - integ11 reaches RimWorld's menu ~10x sooner than main (66 s vs 649 s after launch, same
    harness, same host): the Cyberpunk NFS/readahead work, presumably.
  - Every lane tree that runs `make host_tests` without 1686d13e leaves one ./EDITOR.BIN_host.
  - main's tools/rw/rw_gameplay_test.py matches run_t5_tests.py's tools/*/*_test.py glob, so the
    next main merge turns `make host_tests` red (it tries to reach a QMP socket).  It wants the
    same exclusion run_doom_elf_test.py has (GB-176).
  - The integ batch's "known reds" virtio_gpu_test.c:214/:244 are gone with f85026a3: update
    the parity reference when cp-regress lands.
  - The ARM unit image is 0.45 MiB under linker.ld's line; the next unit-only static over that
    breaks the ARM unit link again.  Big test buffers belong in the frame pool.
  - The Makefile's QEMU_GLFIX_X64 (auto-picked when ~/.local/share/hobbyos-tools/qemu-glfix
    exists) bypasses any qemu-system-x86_64 wrapper on PATH.  A GL boot that must run under
    host-venus.sh's environment has to set QEMU_GLFIX_DIR (or QEMU) explicitly.

Disk (after cleanup): see the report; /srv/nfs/cp-regress-rw deleted; RimWorld-staged
disk.img and obj/rw-desktop removed from both strix trees.
