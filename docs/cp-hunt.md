# cp-hunt — the wave-8 crash and glitch hunt (lane H1-hunt)

Plan: `docs/cyberpunk.md` §6 P8/P9, wave 8.  Branch `cp-hunt` from integ15 =
a23bfbeb (no kernel or product code changed by this lane).  Evidence:
`docs/cp-hunt-evidence/*.txt` (short excerpts only; the screendumps and WAVs
are game content and stayed on strix, then were deleted).

Every number is from **strix** (Ryzen AI MAX+ 395, Zen 5, AVX-512, KVM
`-cpu host -smp 8`, RX 9070 XT on renderD129 through `tools/cp/host-venus.sh`),
tree `~/cp/lanes/hunt` = integ15, Wine `~/cp/wine-cp` (X11 + ntsync +
winealsa), Venus ICD a copy of `~/cp/integ-venus` (`~/cp/lanes/hunt-venus`),
seeded prefix a copy of integ's `obj/cp-p8/pfx-seeded.img` (sha256
`b56d5231…`).  P8 boots: 12 GiB; P9 boots: 32 GiB under
`/tmp/cp-bigguest.lock`.  Other lanes' guests ran beside every boot (H2-early,
H3-winefix, H4-kglitch, the integ16 gate batch); the host sampler lists them.

## Result

<!-- SUMMARY -->

## The catalog (sorted by severity × rate)

Format per the wave-8 overrides: ID, symptom, rate (k/n boots, boot IDs),
first-seen evidence, signature, suspected layer, severity, status.  "This
lane's boots" are the `h-*` boots below (integ15 code); "old boots" are the
census of 128 earlier game boots (§ Old logs), quoted only to show whether
something recurs.

| ID | symptom | rate | first-seen evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
<!-- ROWS -->

### Notes on the rows

**H1-01 (audio dropouts).**  Each dropout is a run of exact digital zero
on both channels, 10–200 ms long, always a whole number of winealsa's 10 ms
periods, with audio at −20…−36 dBFS on both sides — music does not cut to
true zero and back for one period; a starved device does.  They cluster in
the save's loading (game s 147–158 in h-fp2: the phase where the 8 vCPUs are
busiest) and are rare in the world.  The ALSA buffer winealsa asks for is 4 ×
10 ms = 40 ms (`[SND] hw_params … period=480 periods=4`, docs/cp-audio.md
§7), so any 40 ms stall of the game's audio thread (or of the compat
`WRITEI` path behind it) is audible.  The kernel counts xruns but prints
them only at PCM close (H1-11), which never happens before QEMU stops, so the
guest side cannot confirm it yet.  Next steps (not done here): print the
xrun count periodically (H1-11), then decide between a longer device buffer
on the winealsa side (H3-winefix: the 40 ms comes from the WASAPI period the
game asks for, mapped by winealsa) and the scheduling of the game's audio
thread under load (H4-kglitch).

**H1-04 (stale prefix stamp).**  Not a game bug: every game boot of this
lane ran a ~9.5 s `wineboot` prefix update (PreInstall, DefaultInstall,
Wow64Install) because the seeded image (made by integ15 with `wine-ntsync`)
carries the old Wine's `.update-timestamp` while the wave-8 default stage is
`wine-cp`.  It costs boot time, writes the prefix on every boot, ends with
the second `0xC0000135` `[EXITPC]` row (32-bit `rundll32`, no WoW64 Wine) and
opens the window for H1-05.  integ16's own `p8_pfx` will rebuild the image
with `wine-cp`; the harness should refuse or warn on a mismatch.  Proposed
(H2-early owns `tools/cp/p8/`), in `run-p8.sh` before the boot:

```diff
+  # the seeded prefix must have been made by this Wine stage, or every game
+  # boot re-runs wineboot's update (~10 s, cp-hunt H1-04)
+  st="$(/usr/sbin/debugfs -R 'cat /pfx/.update-timestamp' "$CPD" 2>/dev/null | head -1)"
+  wi="$(stat -c %Y "$WINE/share/wine/wine.inf" 2>/dev/null)"
+  [ "$PHASE" = game ] && [ -n "$wi" ] && [ "$st" != "$wi" ] &&
+    echo "== WARNING: prefix stamp $st != $WINE/share/wine/wine.inf mtime $wi: the prefix was made by another Wine stage (re-run P8_PHASE=pfx)"
```

**H1-06 / H1-07 (instruments that go blind).**  Both rows have a
lifetime budget that the game spends in its first seconds, the exact
failure mode `process.c`'s own comment warns about ("a noisy instrument with
a budget is worse than no instrument"):

* `[BADELR]` flags `elr` in `[0x70000000, USER_VA_BASE)`.  Wine maps every
  `.exe` at 0x140000000, inside that range, so the 20 rows go on the game's
  own healthy resumes in **every** game boot (this lane's and 127/128 old
  ones).  A real `elr=0x7` after that would not print.  Proposed (H4-kglitch,
  `src/kernel/process.c` ~2697):

  ```diff
  -        int implausible = (elr < 0x1000u) ||
  -                          (elr >= 0x70000000u && elr < USER_VA_BASE);
  +        /* a compat (Linux-ABI) process maps Windows PE images below
  +           USER_VA_BASE (Wine: 0x140000000 for every .exe), so there only
  +           the null page is implausible (cp-hunt H1-06) */
  +        int compat = (proc_table[idx].abi_flags & 1u) != 0;   /* PROC_FLAG_LINUX_ABI */
  +        int implausible = (elr < 0x1000u) ||
  +                          (!compat && elr >= 0x70000000u && elr < USER_VA_BASE);
  ```
  (`abi_flags` is group-anchor state; if a thread slot does not carry it,
  read the anchor's.)
* `[VENUS] fence_wait2 timeout` prints the first 16 expired waits of the
  boot.  All 16 come from short-timeout polls in the game's first ~6 s of
  Vulkan (the fence then retires: `done` catches up), so a real GPU hang
  later in a boot would not print.  Proposed (H4-kglitch / C6 owner,
  `src/kernel/gpu_abi.c` ~1409): count every timeout, print only waits whose
  caller asked for ≥ 1 s (`if (fw.timeout_ns >= 1000000000ull && to_rows++ < 16)`),
  and print the running count in an existing periodic row.

**H1-08 / H1-12 (P9 harness).**  `boot_p9.py`'s `seek` / `listseek` read the
highlighted row's screen position against tables measured on the 1280×720
layout.  At a 1920×1080 game resolution the menu is 1.5× larger and `canon()`
passes the 1920×1080 shot through unchanged, so no row is ever found
(h-fq1).  This lane's `tools/cp/hunt/fpsprobe-blind.p9` replaces the two
seeks with the tap counts every 720p boot needed (inputcheck: 10 of 10), and
the OCR `expect`s still guard each step; h-fq1b onwards use it.  Proposed
(H2-early owns `tools/cp/p9/`), untested:

```diff
@@ def canon(name):
-                if im.size == (1920, 1080) and args.xscreen == "1920x1080":
+                if im.size == (1920, 1080) and args.xscreen == "1920x1080" and args.game_res != "1920x1080":
                     return im
-                g = im.crop((0, 0, 1280, 720)) if im.size != (1280, 720) else im
+                if args.game_res == "1920x1080" and im.size == (1920, 1080):
+                    g = im.resize((1280, 720))     # the menu scales with the game's resolution
+                else:
+                    g = im.crop((0, 0, 1280, 720)) if im.size != (1280, 720) else im
@@ def main():
+    ap.add_argument("--game-res", default="1280x720", help="the game's own Resolution setting")
```
(plus `P9_GAME_RES` → `--game-res` in `run-p9.sh`).  H1-12: reload.p9's
CONTINUE seek lost the focus in h-rl1 (evidence file): the reading that fits
is a doubled Up onto the frameless patch-notes button above CONTINUE.  A
seek that loses a visible focus after a tap should tap once back the other
way before giving up; the reload can also take the newest save from the
LOAD GAME list it already opens.

**The P8/P9 grader (known, H2-early).**  The overrides already list the P8
lavapipe exit 2–3 s after launch that the grader records as rc=0
"launcher-done".  `classify.py` flags it (`early_exit`: the game's `[P8] end
game … ms=` below 60 s and no menu OCR; golden-tested).  In the old run logs
it splits into two populations, 2.5–2.9 s (integ13-lvp1, integ15-lvp2,
k7l-1) and 20–24 s (integ10-venus, k6-venus, console-lvp1); in the four of
them whose serial survives, REDEngineErrorReporter exits twice and
services.exe / explorer.exe / plugplay.exe exit with code 1 (the whole Wine
session going down), which a successful boot never shows.  Proposed grader
diff (H2-early, `tools/cp/p8/boot_p8.py` ~287):

```diff
             if "[P8] done" in ser or "System halt" in ser:
-                verdict = "launcher-done"
+                # the game's own process ended and no main menu was seen:
+                # the early exit (cp-hunt), not a pass
+                verdict = ("game-exited-early" if args.phase == "game" and "[P8] end game " in seen
+                           else "launcher-done")
```
(`return 0 if verdict in ("launcher-done", "main-menu")` then makes it rc 1.)

<!-- FPS -->

<!-- MATRIX -->

## Old logs on strix: the census (`classify.py --mine`, `mine_report.py`)

Every `serial.log` / `serial.txt` / `*.serial` under `~/cp/integ1*/`,
`~/cp/lanes/*-logs/`, `~/cp/integ/obj/cp-p8*|cp-p9*` and every
`~/cp/**/obj/cp-p8*|cp-p9*` (595 paths) was read once.  Copies of one boot
(the integ batches copy `serial.log` to `~/cp/integ1N/p8-*.serial`;
`serial.txt` is `serial.log` without CRs) are counted once: the key is the
sha256 of the CR-stripped bytes.  That leaves **147 distinct boots, 128 of
them game boots** (146 duplicates dropped).  Full output:
`docs/cp-hunt-evidence/mine-census.txt`.  The older boots (wave 5 / early
wave 6, before C0-console) interleave rows from two CPUs on the serial
console, so the classifier cuts every kernel row at the next row tag and keys
it on its first six normalised words.

<!-- CENSUS -->


## Harness (`tools/cp/hunt/`, new)

| file | what |
|---|---|
| `run-hunt.sh KIND RUN` | one matrix boot through the unchanged P8/P9 harnesses (`p8v`, `p8l`, `win`, `fp720`, `fp1080`, `play`, `reload`, `soak`, `settings`, `script`), plus a 1 Hz host sampler (9070 XT `gpu_busy_percent`, loadavg, the other QEMU guests every 60 s: C1-k7's `k7-p9.sh` sampler for the whole boot), the classifier, the audio checks (`HUNT_AUDIO=1`: `CP_AUDIO_WAV`, then the WAV is deleted), and the evidence copies into `~/cp/lanes/hunt-logs/` |
| `batch.sh QUEUE` | the matrix runner: one boot at a time from a queue file that is re-read before every boot (editable while it runs), `QUEUE.done` per boot |
| `classify.py` | per-boot signatures: kernel faults, kernel stalls, kernel instrument rows, `[EXITPC]` codes (NTSTATUS names), process exits, Wine `err:` lines (each tagged benign-with-reason or `new`), DXVK / vkd3d-proton `err`/`warn`, REDEngineErrorReporter, the early-exit flag, fps-row gaps, `[THRS]` memory per process, GPU busy; `--aggregate` = k/n per signature over boots; `--mine` = every old serial log, deduplicated by content hash |
| `mine_report.py` | the old-log census by signature with last-seen times |
| `wavgaps.py` | dropouts (exact-zero runs of 5–500 ms between loud audio), gaps (≥ 500 ms silent), edge clicks, WAV length vs wall time |
| `shotcheck.py` | numbers-only screen of the screendumps: black / window-black / partial-black bands, full-width seams (tearing) off static edges, high-frequency "garbage", identical consecutive play shots, mean RGB; `--compare` Venus vs lavapipe menus |
| `inputcheck.py` | menu taps from `seek` / `listseek`: did each tap move the focus exactly one row |
| `prep-settings.sh` | a copy of a seeded data image with UserSettings.json options changed by debugfs (generalises `strix:~/cp/shot/prep.sh`) |
| `soak.p9` | the 28-minute in-world soak (six phases `soak1..soak6`) |
| `tables.py` | the P8 / P9 tables below from the per-boot files |
| `test_hunt.py` + `golden/` | golden tests: classifier output, early-exit flag, aggregation, wavgaps on a synthetic WAV with a planted 20 ms dropout and 1 s gap |
