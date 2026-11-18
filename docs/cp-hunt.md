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

**22 game boots** on strix, 2026-11-18 02:21–06:42, integ15 code: 11 P8 to the main menu (6 Venus, 4 lavapipe, 1 Windowed under IceWM; 11/11 reached it) and 11 P9 on Venus (3 fpsprobe at 1280×720 Low, 3 at 1920×1080 Low of which one hit a harness gap, 1 ten-minute play + save, 2 reloads, 1 28-minute soak, 1 hand-driven Settings boot), plus a census of 128 older game boots.  Headline:

* **No kernel fault in any boot of this lane** (0 `User process fault`,
  `FATAL`, `#PF in user space`, `[LCOMPAT] fault`, panic, `[TLBTIMEOUT]`,
  `[STALLBREAK]` rows).  Every such class in the census of 128 older game
  boots was last seen on 2026-11-17 by 12:36 (wave 5 / early wave 6 trees).
* **No early exit (H1-02) in this lane's boots** (0 of 22 launches); the old run
  logs have 6, in two populations (2.5–2.9 s and 20–24 s after launch).
* **Audio: underruns are real** (H1-01): 2 of 2 audio boots, 10 and 22
  dropouts (exact-zero 10 ms periods inside loud audio) and 7 / 49
  `snd_pcm_recover) underrun` rows from alsa-lib in the guest, densest in
  the save's loading, ~1 per 2 minutes in the world; WAV length / wall time 0.962–0.967 (the stream starts
  after the launch; no long gaps).
* **One ~8 s freeze in 28 minutes of play** (H1-15): Venus fences unretired
  7.7 s while the host's own sampler stalled 8 s; host side.
* **A 1–2.5 minute pre-launch stall in 11 of 22 boots** (H1-03): QEMU start →
  launcher 64–142 s instead of 6–58 s.  The three with the stage on record
  sat in the firmware (twice) or behind a console that did not reach the
  host (once), each while the host's I/O pressure was 50 % "full"; a
  `cache=unsafe` A/B is proposed.
* **Instruments that mislead**: the stall watchdog's "console silent >
  20 s" (15 of 22 boots, 101 of 128 old ones) is a false positive every time
  it was checked (H1-14, root cause: an unsigned wrap; one-line diff), `[BADELR]` fires on the game's PE image (H1-06),
  the Venus fence-timeout rows are spent in the first 6 s (H1-07).
* **P9 fps verdict (≥ 30 fps world median at 1280×720 Low):** **met at the median, not with margin**: the three 1280×720
  fpsprobe boots give world medians 29.0, 33.4 and 30.0 (median of medians
  **30.0**, p10 20.6–26.4); the two 1920×1080 boots 29.8 and 33.0 (p10 17.4,
  26.3) — the same as 720p, so the frame is not GPU-fill bound; the 10-minute
  play boot 30.5 (p10 21.8) and the 28-minute soak 35.9 (p10 23.9).  The 9070
  XT was 33–54 % busy in the world phases.  That is ~3–5 fps above cp-k7's
  integ11 numbers (25.5–27.9), on integ15 (C1-k7 and C2-ntsync merged; not an A/B, other lanes' guests differ).
* **Soak (28 min in the world):** no fps decay (phase medians 34.5 → 43.0,
  scene-dependent), the game's resident memory 4309 → 4473 MiB (+5.9
  MiB/min, flat over the last two phases), virtual anon +33 MiB, threads 48
  throughout: no leak.
* **Visual review** (737 screendumps, numbers only): no black, torn,
  garbage or stuck frame after the menu; Venus and lavapipe main menus have
  the same mean colour (63.2 / 59.8 / 81.8 both).  **Input**: across 88 P9
  run logs, 274 menu taps moved the focus exactly one row, 0 were dropped;
  but after Esc back from a sub-menu (save list, Settings) the main menu's
  focus could not be followed in 3 of 3 boots (H1-12), which blocked both
  reloads.
* **Harness gaps found and worked around**: P9 menus at 1920×1080 (H1-08,
  `fpsprobe-blind.p9`), a stale seeded prefix that makes every boot re-run
  wineboot's update (H1-04).

## The catalog (sorted by severity × rate)

Format per the wave-8 overrides: ID, symptom, rate (k/n boots, boot IDs),
first-seen evidence, signature, suspected layer, severity, status.  "This
lane's boots" are the `h-*` boots below (integ15 code); "old boots" are the
census of 128 earlier game boots (§ Old logs), quoted only to show whether
something recurs.

| ID | symptom | rate | first-seen evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| **H1-01** | audio dropouts: exact-zero runs of 10–290 ms (whole 10 ms periods) inside loud audio; in h-fp2 all in or near the save's loading, in the soak 7 during loading and 15 over 28 minutes in the world (~1 per 2 min); alsa-lib in the guest reports the underruns | 2/2 audio boots (h-fp2, h-soak1) | `cp-hunt-evidence/audio-dropouts-fp2.txt` | `ALSA lib …/pcm.c:8772:(snd_pcm_recover) underrun occurred` (h-fp2 7 rows / 10 dropouts; h-soak1 49 / 22) | game audio thread starved under guest load + winealsa's 40 ms buffer (Wine / kernel scheduling) | wrong output (audible glitch) | open |
| **H1-15** | ~8 s freeze in the world: Venus ring-1 fences unretired for 7.7 s, fps rows of 1.7 and 4.3, audio dropouts; the host's own 1 Hz sampler stalled 8 s twice at the same moment | 1 in 28 min of soak (h-soak1); none in the other P9 world phases (≤ 1 hitch each) | `cp-hunt-evidence/soak-venus-freeze.txt` | `[VENUS] stall pid=110 tid=185 waits_ms=7754 ctx=12 ring=1` | host (amdgpu / virglrenderer under host load), not the guest | hang (recovers) | open (not ours, most likely) |
| **H1-02** | the game exits 2–3 s (or ~20 s) after launch, right after REDEngineErrorReporter starts; graded rc=0 "launcher-done" | 0/22 here; old run logs: 6 (integ10-venus, k6-venus, console-lvp1, integ13-lvp1, k7l-1, integ15-lvp2) | `cp-hunt-evidence/mine-census.txt` (run-log section) | `[P8] end game status=0 ms=2886` + REDEngineErrorR exits twice + services.exe/explorer.exe `code=0x1` | H2-early's (Wine session teardown) | crash | open (H2-early); grader diff below |
| **H1-03** | 1–2.5 minute stall before the launcher starts: twice in the firmware stage (serial stuck at the firmware's 113 bytes for 78 / 85 s), once after the X session (console bytes not reaching the host for ~145 s, vCPUs waiting in the UART TX path) | 11/22: QEMU start → launcher NFS mount 64–142 s (fast boots 6–58 s); stage identified in h-soak1 (console), h-p8v5 and h-p8v6 (firmware, host I/O) | `cp-hunt-evidence/pregame-console-stall.txt` | `no launcher/Wine row for 120 s before the game: sampling the vCPUs (prof-prestall)`; host `/proc/pressure/io` full avg10 50–55 % during the stall | host I/O pressure (QEMU's flushes / file writes wait behind other lanes' dirty data) | hang (recovers) | open: QEMU `cache=unsafe` A/B proposed below |
| **H1-12** | after Esc back to the main menu from a sub-menu (the save list, Settings), Up/Down no longer show a focus frame: LOAD GAME → NEW GAME then nothing (h-rl1), a taller frame back at LOAD GAME then nothing (h-rl1b), nothing at all after Settings (h-set1) | 3/3 boots that returned from a sub-menu (h-rl1, h-rl1b, h-set1); lanes/play's reload boots (wave 6, wine-build-x) 3/3 fine | `cp-hunt-evidence/reload-seek-continue.txt`, `settings-menu.txt` | `SEEK FAILED menu-continue: main menu row 'CONTINUE' not focused after 6 x up` | game UI × input (where the keyboard focus goes after the sub-menu closes); harness can't follow it | harness (blocks the reload check) | open |
| **H1-08** | P9 `seek` never finds the main-menu focus at a 1920×1080 game resolution | 1/1 seek-based 1080p boot (h-fq1); 0/2 with `fpsprobe-blind.p9` (h-fq1b, h-fq2) | `cp-hunt-evidence/p9-seek-1080.txt` | `SEEK FAILED menu-load: main menu row 'LOAD GAME' not focused after 6 x down` | harness (`boot_p9.canon`) | harness | worked around (`fpsprobe-blind.p9`); diff below (H2-early) |
| **H1-09** | the ≥ 30 fps world target is met only at the median: fpsprobe 720p medians 29.0 / 33.4 / 30.0, 1080p 29.8 / 33.0; p10 17–26; single ~1.5 s rows down to 6–13 fps | 2/5 fpsprobe boots below 30 (h-fp1, h-fq1b) | P9 table below | `world median 29.0 (p10 24.7)` (h-fp1) | game user code + Venus fence/present path (cp-k7 §6); 1080p ≈ 720p, so not GPU fill | perf | open |
| **H1-04** | every game boot re-runs wineboot's prefix update (~9.5 s, writes the prefix) | 22/22 | `cp-hunt-evidence/prefix-stale-stamp.txt` | `wine: failed to start L"C:\\windows\\syswow64\\rundll32.exe": c0000135` (its last step) | harness (seeded prefix from another Wine stage) | perf | open (integ16's p8_pfx rebuilds it; check diff below for H2-early) |
| **H1-10** | ~1.5 s freeze on the first frame after closing the map | 1 of 2 map closes (h-play1) | h-play1 `fps.csv` (game s 752.9) | one `approx 0.61fps` row after `unwind map-close2` | game / Venus (first frame of a scene) | perf | open |
| **H1-13** | 150–260 ms NFS reply loss + resend pauses on the game copy | 21/22 boots, 70 rows (1–8 per boot) | `classify.py` kstall `nfspipe` | `[NFSPIPE] stall inflight=1 issued=518 replies=517 … us=258904` | kernel NFS / slirp | perf | open (known class, recovers) |
| **H1-05** | win32u cannot find the GPU while the per-boot prefix update rebuilds devices | 2/22 (h-fq1b, h-fq2) | `cp-hunt-evidence/win32u-find-gpu-race.txt` | `err:system:find_gpu_from_path Failed to find gpu with path "PCI\\VEN_1002&DEV_7550…"` | Wine (race inside H1-04's update) | cosmetic | open (goes away with H1-04) |
| **H1-06** | `[BADELR]` reports the game's own healthy resumes; the 20-row cap is spent at once | 22/22 (439 rows, capped at 20 per boot); old boots 127/128 | `cp-hunt-evidence/badelr-pe-image.txt` | `[BADELR] picking a user slot with an implausible frame.elr=0x00000001406A5043 … Cyberpunk2077.e` | kernel instrument (`process.c`) | cosmetic (blinds a hang detector) | open; diff below (H4-kglitch) |
| **H1-07** | `[VENUS] fence_wait2 timeout` rows: 16 short-timeout polls in the game's first ~6 s exhaust the lifetime cap | 18/18 Venus boots reach the cap | `cp-hunt-evidence/venus-fence-timeout-cap.txt` | `[VENUS] fence_wait2 timeout pid=109 ctx=12 ring=1 seqno=0x0000000100000011 … done=0x0000000000000000` | kernel instrument (`gpu_abi.c`) | cosmetic (blinds a hang detector) | open; diff below |
| **H1-14** | trap.c's stall watchdog prints `console silent for more than 20s` (+ a CPU dump) about 1 s after the console printed | 15/22 boots (43 rows); old boots 101/128 | `cp-hunt-evidence/watchdog-false-silence.txt` | `[WATCHDOG] console silent for more than 20s` right after an `[X] 1240.402:…trace:fps` row | kernel instrument (`trap.c` watchdog / `uart_last_activity_ms`) | cosmetic (false hang alarms) | open (H4-kglitch: console) |
| **H1-11** | the kernel's xrun count is printed only at PCM close, which a game boot never reaches (alsa-lib's own row confirms H1-01 instead) | 2/2 audio boots | `cp-hunt-evidence/audio-dropouts-fp2.txt` | `[SND] pcm closed: chunks=… xruns=…` absent for the game's stream | kernel instrument (`linux_snd.c`) | cosmetic | open |
| — | benign, every boot (tagged by `classify.py`): `winediag:getaddrinfo`, winemenubuilder 126, `winebth`, `wineusb.inf` copy error, no schannel / Kerberos, `mfc140u.dll` for the Razer Chroma plugin, DXVK EDID / HDR metadata / unknown adapter interface / ignored flags, `wine code=0xC0000135` (winemenubuilder; + syswow64 rundll32 under H1-04) | 22/22 | `*.class.txt` | — | Wine / DXVK | none | not ours |

### Notes on the rows

**H1-01 (audio dropouts).**  Each dropout is a run of exact digital zero
on both channels, 10–200 ms long, always a whole number of winealsa's 10 ms
periods, with audio at −20…−36 dBFS on both sides — music does not cut to
true zero and back for one period; a starved device does.  They are densest
in the save's loading (game s 147–158 in h-fp2, 7 of the soak's 22: the
phase where the 8 vCPUs are busiest) and keep coming at ~1 per 2 minutes in
the world (15 in the soak's 28 minutes).  The ALSA buffer winealsa asks for is 4 ×
10 ms = 40 ms (`[SND] hw_params … period=480 periods=4`, docs/cp-audio.md
§7), so any 40 ms stall of the game's audio thread (or of the compat
`WRITEI` path behind it) is audible.  The guest confirms it: alsa-lib, in
the game process, prints `snd_pcm_recover) underrun occurred` (7 rows in
h-fp2, 49 in the 28-minute soak; wavgaps counts only the zero runs ≥ 5 ms
between loud audio, 10 and 22).  The kernel's own xrun counter is printed
only at PCM close (H1-11).  Next steps (not done here): correlate the
underrun rows with the guest's CPU load (THRW), then decide between a longer device buffer
on the winealsa side (H3-winefix: the 40 ms comes from the WASAPI period the
game asks for, mapped by winealsa) and the scheduling of the game's audio
thread under load (H4-kglitch).

**H1-11 (xrun count).**  Proposed (`src/kernel/linux_snd.c`, the audio
driver's owner), so H1-01 can be confirmed or ruled out in the guest:

```diff
 static void check_xrun_locked(void) {
   if (S.state == ST_RUNNING && S.stop_thr < S.boundary && avail_locked() >= S.stop_thr) {
     S.state = ST_XRUN;
     S.xruns++;
     S.trigger_us = timer_get_us();
+    /* cp-hunt H1-11: a game never closes its PCM before the boot ends, so
+       the close row never shows them: one row per xrun (first 32, then every 64th) */
+    if (S.xruns <= 32 || (S.xruns & 63) == 0) {
+      struct kline kl;
+      kl_init(&kl);
+      kl_s(&kl, "[SND] xrun n=");
+      kl_i(&kl, (long)S.xruns);
+      kl_s(&kl, " frames_done=");
+      kl_i(&kl, (long)S.frames_done);
+      kl_s(&kl, " chunks=");
+      kl_i(&kl, (long)S.chunks);
+      kl_c(&kl, '\n');
+      kl_emit(&kl);
+    }
   }
 }
```

**H1-14 (watchdog false silence): root cause.**  `watchdog_tick()` reads
`now` at the top, then prints the 30-second `[NFSIO]` row through the normal
console (`wd_puts` → `uart_putc_dbg` → `cons_kputc` → `cons_hw_tx`), and
`cons_hw_tx` stamps `uart_last_activity_ms = timer_get_ms()` — a later
millisecond than `now` once that print (or any other CPU's) crosses one.  The
silence test is unsigned, `now - uart_last_activity_ms < WATCHDOG_SILENCE_MS`,
so `now < last` wraps to ~2⁶⁴ and the watchdog declares 20 s of silence and
dumps the CPUs.  24 of this lane's first 32 rows follow the tick's own
`[NFSIO]` row, the rest follow other console output within 1–8 lines; 101 of
the 128 old game boots carry it.  Proposed (owner of `src/kernel/arch/x64/trap.c`'s
watchdog; H4-kglitch for the console side), with a host-test or probe row
that sets `uart_last_activity_ms = now + 1` and expects no report:

```diff
-  if (now - uart_last_activity_ms < WATCHDOG_SILENCE_MS) {
+  /* cp-hunt H1-14: the [NFSIO] print above (or another CPU) can stamp the
+     console a millisecond AFTER `now`; unsigned now - last then wraps */
+  uint64_t last = uart_last_activity_ms;
+  if (last >= now || now - last < WATCHDOG_SILENCE_MS) {
     return;
   }
```
(x86_64 only: the ARM `trap.c` has no such watchdog compare.)

**H1-03 (pre-game stall).**  In h-soak1 the serial file stopped growing for
~145 s right after IceWM started and the launcher's first row came only at
+150 s (a normal P9 boot launches the game at +25…90 s).  The 20 s vCPU
sample boot_p9 took in the middle shows the guest mostly idle and, in the
kernel, in `cons_hw_tx` / `uart_puts`: it was printing, and the bytes were
not reaching `serial.log`.  The image loader's own timings in that window are
normal, and the Wine `+timestamp` clock agrees with the host clock after the
stall, so the guest itself lost the time waiting on its console.  Two other
boots on strix show the same 120 s trigger and a +150 s launch
(lanes/kglitch-s2 kg-stack1, lanes/play r2-fps-scan5).  Candidates: the
UART TX path waiting on a 16550 that QEMU's main loop is not draining (its
`-serial file:` write blocked; the host was at loadavg 36 with ~79 GB page
cache), or a console lock held across the wait.  The same delay, without
the 120 s trigger, shows in the time from the QEMU start to the launcher's
first NFS mount in this lane's boots: 6–45 s in 8 of the first 13 boots,
71, 87, 90, 127 and 142 s in the other 5 (h-p8v2, h-p8l2, h-p8v3, h-play1,
h-soak1), with identical serial content and no correlation with host load
(max loadavg in the first minute 14–36 either way).  `run-hunt.sh`'s sampler now
records the serial file size and the host's Dirty/Writeback each second, so
the next occurrence shows which (boots from h-p8v4 on).

**H1-03, continued.**  With the sampler's I/O column (h-p8v5, h-p8v6) the
firmware-stage form lines up with the host: the guest sits at the
firmware's 113 bytes while `/proc/pressure/io` "full" climbs to 50–55 % and
Dirty is 2–3.7 GB, and gets through as the pressure drops.  QEMU honours
guest flushes with the default `cache=writeback`, so OVMF's variable writes
to the per-run `ovmf.fd` and the guest's NVMe flushes become host `fsync()`s
that wait behind everyone's dirty data.  Every image these boots use is a
per-run copy, so the flush is not worth a minute.  Proposed (H2-early owns
`tools/cp/p8/boot_p8.py`; the integrator / C6 owns `tools/cp/host-venus.sh`),
then an A/B of boot→launcher time on a busy host:

```diff
-           "-drive", f"if=pflash,format=raw,file={fw}",
+           "-drive", f"if=pflash,format=raw,file={fw},cache=unsafe",   # per-run copies: no host fsync per guest flush (cp-hunt H1-03)
@@
-           "-drive", f"file={disk},format=raw,id=disk0,if=none",
+           "-drive", f"file={disk},format=raw,id=disk0,if=none,cache=unsafe",
@@
-           "-drive", f"file={cpdata},format=raw,id=cpdata,if=none",
+           "-drive", f"file={cpdata},format=raw,id=cpdata,if=none,cache=unsafe",
```
(and the same in `host-venus.sh` lines 112 and 118, and the Venus cpdata drive in `boot_p8.py:111`'s `extra`).
`cache=unsafe` still writes everything back when QEMU exits normally, so a
P9 play boot's data disk keeps its save for the reload; only a host crash
mid-boot loses it, and these copies are throwaway.

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
CONTINUE seek lost the focus in both reload boots on h-play1's data disk,
in two different ways (evidence file), so it is the main menu's state after
the save list with that save set, not a dropped or doubled key (inputcheck
over 88 P9 logs: 0 dropped).  A seek that loses a visible focus should
re-read after a second, tap once back the other way, or fall back to taking
the newest save from the LOAD GAME list it already opened.

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

## P9 fps (strix, Venus, 32 GiB, the P9 verdict for the ≥ 30 fps world target)

World = fpsprobe.p9's scripted 90 s walk (play / soak: every `world` /
`soakN` row); median and p10 of winevulkan's `trace:fps` "approx" rows (each
averages ~1.5 s of guest time); "world min row" is the lowest such row;
"hitches" counts consecutive fps rows more than 2.5 s apart in **guest**
time (`WINEDEBUG=+timestamp`), i.e. a frame that took ≥ 1 s.  (fps.csv's own
`game_s` is the host's read time, which follows boot_p9's screendump cadence,
so its gaps are not the game's.)  9070 XT busy = `gpu_busy_percent` of
renderD129, 1 Hz, over the world phases; it includes the other lanes'
Venus guests listed by the sampler.  The `prof-*` phases (boot_p9's QMP PC
sampler running) are left out: the sampler itself costs frames.

| boot | kind | verdict | world median | world p10 | world min row | rows | hitches (>1 s frame) | menu median | 9070 XT busy (world) med/p90 | game res MiB first→max | kernel stalls | other guests seen | script |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h-fp1 | fp720 | script-stop | 29.0 | 24.7 | 12.89 | 62 | 0 | 145.7 | 35/40 | 509→4543 | venus×16, nfspipe×1 | 3 |  |
| h-fp2 | fp720 | script-stop | 33.4 | 20.6 | 11.34 | 66 | 0 | 207.7 | 33/37 | 432→4559 | nfspipe×8, venus×16 | 6 |  |
| h-fp3 | fp720 | script-stop | 30.0 | 26.4 | 22.46 | 64 | 0 | 215.6 | 35/40 | 592→4544 | venus×16, nfspipe×3 | 5 |  |
| h-fq1 | fp1080 | seek-failed:menu-load | None | None | - | 0 | 0 | 107.2 | -/- | 812→2752 | venus×16, nfspipe×5 | 5 | FAILED menu-load: main menu row 'LOAD GAME' not focused afte |
| h-fq1b | fp1080 | script-stop | 29.8 | 17.4 | 6.64 | 68 | 1 | 106.9 | 52/58 | 509→4547 | venus×16, nfspipe×2 | 5 |  |
| h-fq2 | fp1080 | script-stop | 33.0 | 26.3 | 13.29 | 64 | 0 | 111.4 | 54/61 | 430→4369 | venus×16, nfspipe×3 | 7 |  |
| h-play1 | play | script-done | 30.5 | 21.8 | 0.61 | 386 | 1 | 217.7 | 35/41 | 390→4704 | nfspipe×8, venus×15 | 8 |  |
| h-rl1 | reload | seek-failed:menu-continue | None | None | - | 0 | 0 | 207.7 | -/- | 400→2716 | nfspipe×3, venus×16 | 8 | FAILED menu-continue: main menu row 'CONTINUE' not focused a |
| h-rl1b | reload | seek-failed:menu-continue | None | None | - | 0 | 0 | 211.3 | -/- | 382→2700 | venus×16, nfspipe×1 | 3 | FAILED menu-continue: main menu row 'CONTINUE' not focused a |
| h-set1 | settings | seek-failed:menu-load | None | None | - | 0 | 0 | 221.0 | -/- | 515→2738 | nfspipe×2, venus×16 | 4 | FAILED menu-load: main menu row 'LOAD GAME' not focused afte |
| h-soak1 | soak | script-stop | 35.9 | 23.9 | 1.72 | 1080 | 12 | 213.6 | 37/41 | 459→4490 | nfspipe×7, venus×24 | 7 |  |

### Soak h-soak1: per phase

| phase | game s | fps rows | median | p10 | min | longest fps-row gap s (host read time) | game res MiB (last) | anon MiB (last) | threads (last) |
|---|---|---|---|---|---|---|---|---|---|
| soak1 | 281-558 | 182 | 34.5 | 21.3 | 3.4 | 5.3 | 4403 | 141064 | 48 |
| soak2 | 562-837 | 181 | 33.8 | 25.2 | 5.2 | 4.1 | 4408 | 141081 | 48 |
| soak3 | 840-1129 | 178 | 35.6 | 20.6 | 1.7 | 18.0 | 4449 | 141081 | 48 |
| soak4 | 1135-1410 | 181 | 36.0 | 25.9 | 8.3 | 4.2 | 4446 | 141097 | 48 |
| soak5 | 1412-1685 | 178 | 38.2 | 25.0 | 16.2 | 3.0 | 4473 | 141097 | 48 |
| soak6 | 1687-1964 | 180 | 43.0 | 29.8 | 4.8 | 4.2 | 4473 | 141097 | 48 |

resident growth over the soak phases: 4309 -> 4473 MiB in 28.0 min = 5.9 MiB/min

## The matrix: what ran (strix, one boot of this lane at a time)

| item | asked | ran | notes |
|---|---|---|---|
| 1. P8 to the menu | Venus ×6, lavapipe ×4 | Venus 6 (h-p8v1…6), lavapipe 4 (h-p8l1…4): 10/10 at the main menu (OCR); launch→menu Venus 110–170 s, lavapipe 202–279 s | P8 table below |
| 2. P9 fpsprobe | ≥3 × 1280×720 Low, 2 × 1920×1080 Low | h-fp1, h-fp2, h-fp3; h-fq1 (harness fail, H1-08), h-fq1b, h-fq2 | 1080p via `prep-settings.sh Resolution=1920x1080` + `P9_XSCREEN=1920x1080` + `fpsprobe-blind.p9` |
| 3. play / save / reload | ×2 | h-play1 (pass: 11.5 min, saved); reloads h-rl1, h-rl1b on its disk both stopped at the CONTINUE seek (H1-12); the second play/reload pair was dropped | play1: 11.5 min, F5 quick save, QUIT GAME, `QuickSave-0` on the data disk (playTime 4699.4, pos −1408.62, 1223.81, 23.07) |
| 4. long soak | 1 × 25–30 min | h-soak1 (28.0 min in six phases, + audio) | soak table below |
| 5. audio | 2 boots with `CP_AUDIO_WAV` | h-fp2, h-soak1 | WAVs measured, then deleted |
| 6. window modes / settings | 1 windowed, 1 settings change | h-win1: `WindowMode=Windowed` (`prep-settings.sh`), main menu by OCR, put_image path (`[P7] scanout refused` ×8, 72 `[HBP] path=putimage` rows), 0 faults, 0 flagged shots.  h-set1: the Settings menu driven by hand (`settings-menu.txt`): tabs with 1/3, the Field of View slider moved and moved back live, the Quick Preset right-arrow did not take, the game stayed up with no device-lost row; the return to the main menu hit H1-12, so the world was not re-entered with a changed setting | |
| 7. visual review | all screendumps | `shotcheck.py` over every boot | numbers only; shots stay on strix |
| 8. input | menus | `inputcheck.py` over this lane's and 88 older P9 run logs | |

Slots: from ~05:00 three of the four QEMU slots were held back to back by
other lanes' probe batches, and a boot of this lane waited up to 17 minutes
for one; the second play/reload pair (item 3) was dropped; everything else ran.

### P8 boots

| boot | kind | verdict | launch s | launch→menu s | kernel faults | kernel stalls (rows) | EXITPC≠0 | new Wine err sigs | new DXVK/vkd3d sigs | REDEngineErrorReporter | 9070 XT busy med/p90 | other guests seen |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| h-p8l1 | p8l | main-menu | 41.8 | 202.4 | 0 | nfspipe×3 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8l2 | p8l | main-menu | 106.5 | 278.5 | 0 | nfspipe×3 | 2 | 0 | 0 | yes | 0/0 | 7 |
| h-p8l3 | p8l | main-menu | 60.0 | 219.7 | 0 | nfspipe×6 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8l4 | p8l | main-menu | 78.3 | 266.1 | 0 | nfspipe×2 | 2 | 0 | 0 | yes | 0/0 | 4 |
| h-p8v1 | p8v | main-menu | 38.6 | 156.0 | 0 | nfspipe×1, venus×16 | 2 | 0 | 0 | yes | 63/73 | 4 |
| h-p8v2 | p8v | main-menu | 89.6 | 170.0 | 0 | venus×16 | 2 | 0 | 0 | yes | 4/72 | 4 |
| h-p8v3 | p8v | main-menu | 145.3 | 111.4 | 0 | nfspipe×2, venus×16 | 2 | 0 | 0 | yes | 0/66 | 5 |
| h-p8v4 | p8v | main-menu | 40.0 | 140.6 | 0 | nfspipe×3, venus×16 | 2 | 0 | 0 | yes | 0/73 | 5 |
| h-p8v5 | p8v | main-menu | 102.4 | 141.4 | 0 | nfspipe×2, venus×16 | 2 | 0 | 0 | yes | 0/73 | 3 |
| h-p8v6 | p8v | main-menu | 112.3 | 109.9 | 0 | venus×15, nfspipe×1 | 2 | 0 | 0 | yes | 73/93 | 4 |
| h-win1 | win | main-menu | 116.5 | 125.5 | 0 | nfspipe×4, venus×16 | 2 | 0 | 0 | yes | 1/73 | 3 |


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

Verdicts of the 128 (from each run dir's `verdict.txt` where it survives):
script-stop 38, main-menu 28, launcher-done 26 (mostly wave-5 boots that died
at the fd ceiling), unknown 14, script-done 8, qemu-exited 6, harness
failures 6, first-run 1, game-exited-before-menu 1.

| signature (classify.py key) | old game boots | last seen | in this lane's 22 |
|---|---|---|---|
| `[LCOMPAT] fault sig=11` | 24/128 | 11-17 04:46 | 0 |
| `[TLBTIMEOUT] shootdown` | 10/128 (+1) | 11-17 12:36 | 0 |
| `[STALLBREAK]` | 15/128 | 11-17 12:36 | 0 |
| `[LCOMPAT] rt_sigreturn: bad rip/rsp`, `fault-in-handler sig=11` | 8/128 each | 11-17 04:46 | 0 |
| `[KERNEL] User process fault! Vector: 3` / `13` | 4/128, 1/128 | 11-17 04:20 / 00:03 | 0 |
| `KERNEL PANIC: stack smashing detected (SEC-…)` | 3/128 | 11-17 04:53 | 0 |
| `[KERNEL] FATAL`, `[BADVEC]`, `fork: AS clone failed` | 1/128 each | 11-17 02:35–03:37 | 0 |
| `Cyberpunk2077.e code=0xC0000005` (ACCESS_VIOLATION) | 11/128 | 11-17 04:40 | 0 |
| `CrashReporter … mscoree.dll not found` | 18/128 | 11-17 04:46 | 0 |
| `winediag:NtCreateFile Too many open files` | 4/128 | 11-17 00:40 | 0 |
| `services.exe` / `explorer.exe code=0x1` (Wine session down) | 34 / 33 /128 | 11-17 18:33 | 0 |
| `[VENUS] fence_wait2 timeout` (H1-07) | 95/128 | 11-18 01:59 | 18/18 Venus |
| `[NFSPIPE] stall` (H1-13) | 64/128 | 11-18 01:07 | 21/22 |
| `[BADELR] … Cyberpunk2077.e` (H1-06) | 127/128 | 11-18 01:59 | 22/22 |
| `[WATCHDOG] console silent` (H1-14) | 101/128 | 11-18 01:57 | 15/22 |
| `[VENUS] stall` (H1-15 class) | 2/128 (k7b-2, p7 ns-s-off1) | 11-17 21:19 | 1/22 |

Restricted to the 68 boots whose serial is from 2026-11-17 12:00 on (integ10
onwards) the only kernel-fault row is one `[TLBTIMEOUT]` at 12:36; the
crash classes above are all wave 5 / early wave 6 and gone on today's code.


## Harness (`tools/cp/hunt/`, new)

| file | what |
|---|---|
| `run-hunt.sh KIND RUN` | one matrix boot through the unchanged P8/P9 harnesses (`p8v`, `p8l`, `win`, `fp720`, `fp1080`, `play`, `reload`, `soak`, `settings`, `script`), plus a 1 Hz host sampler (9070 XT `gpu_busy_percent`, loadavg, and from h-p8v4 on the boot's serial.log size, host Dirty/Writeback and `/proc/pressure/io`; the other QEMU guests every 60 s: C1-k7's `k7-p9.sh` sampler, for the whole boot), the classifier, the audio checks (`HUNT_AUDIO=1`: `CP_AUDIO_WAV`, then the WAV is deleted), and the evidence copies into `~/cp/lanes/hunt-logs/` |
| `batch.sh QUEUE` | the matrix runner: one boot at a time from a queue file that is re-read before every boot (editable while it runs), `QUEUE.done` per boot |
| `classify.py` | per-boot signatures: kernel faults, kernel stalls, kernel instrument rows, `[EXITPC]` codes (NTSTATUS names), process exits, Wine `err:` lines (each tagged benign-with-reason or `new`), DXVK / vkd3d-proton `err`/`warn`, REDEngineErrorReporter, alsa-lib underruns, the `[WATCHDOG]` / `[BADELR]` instrument rows, the early-exit flag, guest-time fps hitches, `[THRS]` memory per process, GPU busy, the longest silent-serial stretch; `--aggregate` = k/n per signature over boots; `--mine` = every old serial log, deduplicated by content hash |
| `mine_report.py` | the old-log census by signature with last-seen times |
| `wavgaps.py` | dropouts (exact-zero runs of 5–500 ms between loud audio), gaps (≥ 500 ms silent), edge clicks, WAV length vs wall time |
| `shotcheck.py` | numbers-only screen of the screendumps: black / window-black / partial-black bands, full-width seams (tearing) off static edges, high-frequency "garbage", identical consecutive play shots, mean RGB; `--compare` Venus vs lavapipe menus |
| `inputcheck.py` | menu taps from `seek` / `listseek`: did each tap move the focus exactly one row |
| `prep-settings.sh` | a copy of a seeded data image with UserSettings.json options changed by debugfs (generalises `strix:~/cp/shot/prep.sh`) |
| `soak.p9` | the 28-minute in-world soak (six phases `soak1..soak6`) |
| `fpsprobe-blind.p9` | fpsprobe.p9 with the two menu seeks replaced by fixed tap counts, for game resolutions whose layout boot_p9's row tables do not know (H1-08) |
| `settings.p9` | main menu, then interactive (`ctl`): the hand-driven Settings / reload boot |
| `tables.py` | the P8 / P9 tables below from the per-boot files |
| `test_hunt.py` + `golden/` | golden tests: classifier output, early-exit flag, aggregation, wavgaps on a synthetic WAV with a planted 20 ms dropout and 1 s gap |
