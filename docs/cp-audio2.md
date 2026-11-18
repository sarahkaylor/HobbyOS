# cp-audio2 — audio dropouts in game boots (lane H5-audio2, wave 8b, cp-hunt H1-01)

Base: integ16 = 1d81ffd6.  Host for every number: **strix** (Zen 5, KVM,
QEMU 10.2.1, `-smp 8`, `-m 12G` repro boots, `-m 32G` P9 boots), usually with
other lanes' guests beside it.  Evidence: `docs/cp-audio2-evidence/*.txt`.
Design of the audio stack: `docs/cp-audio.md`.

## 1. Result

H1-01 is two things on top of each other, and only the first is the kernel's.

**A. Kernel xrun cascades (fixed).**  The driver counted a TX message as
played the moment QEMU handed it back, and QEMU hands it back early, in two
ways:

1. *Grab-ahead.*  QEMU returns a message when its mixing engine has *copied*
   the frames into a 1024-frame mixing buffer at a 10 ms audio tick, not when
   they have played: hw_ptr runs up to ~21 ms ahead of the music, so of
   winealsa's 40 ms buffer (4 × 10 ms) only ~19 ms is real slack.
2. *Catch-up after a stall.*  QEMU's audio backends are rate-controlled
   against QEMU's virtual clock (`audio_rate_get_bytes`): time in which the
   device had nothing is *owed* and paid back by draining up to the whole
   mixing buffer per tick — 2.2× real time — until paid (or reset past 65536
   frames: "Resetting rate control" on QEMU's stderr).  strix's host stalls
   (26–107 per boot, `[CONS] stalls=`) freeze the whole guest; after each,
   the device eats the writer's buffer twice as fast as the writer fills it:
   one xrun per 10 ms writer tick, 19 from one 270 ms stall in the first idle
   repro.

Fix (`src/kernel/linux_snd.c`, 44b83122 + a213a9c1 + 044b517e): the underrun
rule runs on a play clock, `vis = min(hw, clk)`, `clk` advancing at real
time within `[hw − buffer, hw + period]`.  A device at real time or slower is
unchanged (`vis == hw`: a late writer still underruns); a device that runs
ahead cannot drain the writer faster than the music plays.  avail, hw_ptr,
DELAY, poll and drain stay on hw.  Kernel xruns: idle repro 363 → 1 per
150 s, game boot 534 → 2–15 per 5.5 min (final kernel: 2, 2) (§4).

**B. The audible zeros are winealsa's lead-in (not fixed; Wine side).**  The
kernel xruns were not what put the zeros in the WAV: with A fixed the game's
WAV dropouts did not go down (base 5; lane 6, 5).  The new `[SND] zeros` rows
(every exact-zero run the writer sends; QEMU never makes zeros) show that
every zero run in the game's stream is one or more WRITEIs consisting
entirely of `period − held` zeros — `alsa_write_data`'s lead-in, written
when `data_in_alsa_frames == 0 && held_frames < period`, i.e. when the
game's padding fell below 10 ms at a winealsa tick.  Two kinds:

- *starvation* (q≈0, 2–29 consecutive lead-ins, 50–220 ms): the game did not
  render for several ticks; densest in the save's loading, as H1 saw;
- *into a non-empty buffer* (450–1741 frames still queued by the play clock
  when the run began): winealsa's played-frames estimate `min(data_in_alsa, avail)`
  counts the whole free space as played, which is only right when the last
  tick filled the buffer.  A candidate patch is in
  `docs/cp-audio2-evidence/winealsa-accounting.txt` — mixed results in the
  model, not built, needs approval (§6).

Neither kind lines up with the host stalls (`since_gap_ms` is seconds to
minutes for most runs) or with a kernel xrun.  Why the game's audio thread
does not top up its WASAPI buffer within a 10 ms tick (wake latency of its
event wait, its own loading work) is not measured here.

A second kernel step was tried and reverted: reporting the play-clock
position to user space (62f90f9a) to stop the lead-in.  Three game boots with
it: 15, 12, 11 dropouts — no better, and it cost smaller TX messages.
044b517e restores hw-based reporting and keeps the clock for the xrun rule.

## 2. QEMU 10.2's output path (what the repro and the host model show)

- `audio_timer` every `timer-period` (10 ms); a late timer fires once and
  re-arms from now.
- At each tick `virtio_snd_pcm_out_cb` copies queued TX messages into the free
  part of the voice's mixing buffer (`wav`: 1024 frames) and returns every
  message whose last frame it copied (`return_tx_buffer` + notify).
- The `wav` backend then writes `min(owed, live)` frames, `owed =
  elapsed × rate − written`, reset when outside [0, 65536]
  ("Resetting rate control (N frames)" on QEMU's stderr, in the console log).
- QEMU_CLOCK_VIRTUAL keeps running while the QEMU process is descheduled or
  stopped, so a host stall is owed time.
- Seen in the guest's own rows: `[SND] stat … max100ms=10560` (frames the
  device returned in 100 ms; 4800 is real time) in every 10 s window that had a
  host stall; `[SND] xrun … wr_ago_us=382 done40ms=3840` (the writer had
  written 0.4 ms before, the device returned 80 ms of frames in 40 ms).

## 3. Repro and instruments

**Guest programs** (`tools/cp/audio2/`, built by `build.sh` with musl-gcc
against a static alsa-lib 1.2.14 from the pinned tarball; nothing downloaded):

- `ATONE.ELF SECS [hz= lbuf_ms= fill= stall_ms= stall_every= fmt=]` — the
  winealsa model: plug → mmap_emul → hw (the staged alsa.conf block,
  embedded), FLOAT_LE client format, `period_time_near(10 ms)`,
  `periods_near(4)`, start threshold 1, stop threshold = buffer; a timer thread
  with winealsa's `alsa_timer_loop` (select() sleep, ±period/2 drift clamp)
  running `alsa_write_data` (avail_update, XRUN → silent recover + reset +
  prepare, the lead-in, the copy), and a "game" thread that fills the local
  buffer on each event (`fill=1`: one period per event, an engine that keeps
  little padding).  Rows `[AT] stat` every 10 s, `[AT] total`, `[AT] done`.
- `ALOAD.ELF SECS threads= duty= mode=cpu|mix` — guest-side load (page
  faults, munmap shootdowns, futex ping-pong).
- `run-atone.sh RUN SECS "ATONE ARGS" ["ALOAD ARGS"]` — one test-mode wave boot
  with `CP_AUDIO_WAV` (QEMU `wav` audiodev only), then wavgaps.py and
  wavcheck.py; the WAV is deleted.  `HOSTSTALL="MS EVERY COUNT"` runs
  `hoststall.sh`: SIGSTOP/SIGCONT of this run's own QEMU (found by the WAV
  path on its command line), the whole-guest stall H4-kglitch used.
- `repro-batch.sh PREFIX [SECS] [ROWS]` — rows idle / stall / lowpad / load.

**Kernel rows** (rate-limited; `linux_snd.c`):

- `[SND] stat t_ms= frames= wall_frames= max100ms= maxgap_us= maxlat_us=
  maxwr_us= minq= dry=+ xruns=+ held=+ maxahead=` once per 10 s of a running
  stream: frames the device returned vs wall time, the most it returned in any
  100 ms, the longest gap between completions, the longest post→completion
  latency of a TX message, the longest gap between WRITEIs, the fewest frames
  queued at a completion, completions that left nothing in flight, xruns,
  xruns the play clock held back, and how far hw ran ahead of the clock.
- The H4 `[SND] xrun n=` row gains `t_ms= wr_ago_us= io_ago_us= done100ms=
  done40ms=`: who missed the deadline — the writer (time since its last
  WRITEI) or the device (frames it returned in the last 100 / 40 ms).
- The close row gains `held=`.

**Host check** (`tools/cp/audio2/pace-host-test.sh`, no boot, ~1 min): the
kernel's `linux_snd.c` in the LSND_HOST_TEST shim on a host model of QEMU's
path (`LSND_HOST_QEMU=1` in `src/host/lsnd_host_dev.c`: its own timer thread,
the 1024-frame buffer, the rate control, the completion "IRQ"), driven by the
same atone.c under the staged glibc alsa-lib.  Rows calm / stall (5 SIGSTOPs
of 300 ms of the player process) / lowpad.

## 4. Numbers

### 4.1 Cheap repro, A/B (strix, 150 s per row, `repro-batch.sh`)

base = integ16 + the timing rows (d4608be1); lane = the play clock (44b83122;
a213a9c1's one-buffer bound came after these rows).  Booted pairwise, base
and lane at the same time, so both saw the same host load.
`docs/cp-audio2-evidence/repro-ab-strix.txt`.

| row | host stalls (`[CONS]`) | base xruns | lane xruns (held) | base WAV dropouts | lane WAV dropouts |
|---|---|---|---|---|---|
| idle (tone, 40 ms buffer) | 26–27, 7.2–7.5 s | **363** | **1** (159) | 0 | 0 |
| stall (+10 × 300 ms SIGSTOP) | 31–32, 7.3–8.1 s | **306** | **1** | 0 | 0 |
| lowpad (one period per event) | 16–22, 3.2–4.9 s | **11331** | **10** (287) | 0 | 0 |
| load (+ALOAD 8 threads mix) | 102–301, 22–28 s | **2504** | **46** (1248) | 7 (70 ms) | 0 |

Per minute: idle 145 → 0.4, lowpad 4532 → 4, load 1002 → 18.  The lane's one
xrun in idle/stall is the stream start (winealsa's lead-in races the device,
`t_ms=10`).  The load row's 46 are a different thing: the writer's 10 ms
tick was more than 40 ms late 150 times (`[AT] late40=150`) with 8 busy
threads on 8 vCPUs and 22 s of host stalls — a genuinely late writer, which
the clock must and does still report (H5-02 below).

The WAV shows no dropout in most rows because the repro's "game" keeps its
local buffer full, so winealsa's recovery has data and writes no lead-in;
H1's game WAVs had the zeros because the game holds less than a period at
recovery.  base-load (8 lead-ins) is the one row with the WAV signature: 7
exact-zero 10 ms runs.  The kernel xrun count is the measure here.

The first idle boot (instrumented, no fix, no injected stall) is the clearest
picture of the mechanism: a 270 ms host stall (`[AT] maxgap_ms=273.6`,
`[SND] maxgap_us=282690`: writer and device frozen together), then for
~200 ms the device returned 3840 frames per 40 ms (2× real time,
`done40ms=3840`) while the writer had written 4 ms before each xrun
(`wr_ago_us≈4000`): 19 xruns, one per writer tick, until QEMU's debt was paid
(`frames=476640 wall_frames=480246` in the stall window, `483840 / 480010`
in the next).

### 4.2 Host check (`pace-host-test.sh`, no boot)

| run | base xruns after 1 s | lane |
|---|---|---|
| calm | 0, 1 | 0, 0 |
| stall (5 × 300 ms SIGSTOP) | 31, 12 | 0, 1 |
| lowpad | 1837, 1795 | 1, 3 |

Two runs each; PASS limits 0 / 2 / 10.  `docs/cp-audio2-evidence/pace-host-test-ab.txt`.

### 4.3 Game audio boots (P9 fpsprobe 1280×720 Low, Venus, wine-cp2; `run-hunt.sh fp720`, `HUNT_AUDIO=1`)

`tools/cp/audio2/gamecheck.sh`; `docs/cp-audio2-evidence/game-audio-ab.txt`.
Host stalls = the console's `stalls= stolen_ms=` (strix was shared with
another lane's P9 boots the whole time).

| boot | kernel | WAV dropouts (ms) | alsa-lib underrun rows | kernel xruns | held back | host stalls |
|---|---|---|---|---|---|---|
| h-fp2 (H1) | integ15 | 10 (750) | 7 | (no rows) | — | — |
| h-soak1 (H1, 28 min) | integ15 | 22 (1086) | 49 | (no rows) | — | — |
| a2-base-fp1 | base d4608be1 | 5 (454) | 3 | **534** | — | 51 / 9.8 s |
| a2-lane-fp1 | step 1 (a213a9c1) | 6 (410) | 1 | **8** | 1073 | 85 / 16.9 s |
| a2-lane-fp2 | step 1 (a213a9c1) | 5 (400) | 0 | **10** | 1215 | 96 / 17.7 s |
| a2-lane-fp3 | + vis reported (62f90f9a) | 15 (460) | 0 | 13 | 604 | 107 / 30.1 s |
| a2-lane-fp4 | 62f90f9a | — | — | — | — | game exited 2.5 s after launch (REDEngineErrorReporter, H2 class; the PCM never opened) |
| a2-lane-fp5 | 3633a1df (+ zeros rows) | 12 (534) | 0 | 15 | 1009 | 92 / 24.6 s |
| a2-lane-fp6 | 644dcde2 | 11 (550) | 0 | 7 | 495 | 57 / 10.0 s |
| a2-lane-fp7 | final 044b517e | 6 (470) | 0 | **2** | 922 | 92 / 18.4 s |
| a2-lane-fp8 | final 044b517e | 5 (430) | 0 | **2** | 73 | 26 / 2.5 s; no 50 ms service gap in the stream at all |

Kernel xruns: 534 → 2–15 in every lane boot (final kernel: 2 and 2).
alsa-lib's printed underruns: H1 7 and 49, base 3, lane 0–1.  WAV dropouts:
H1 2/2, base 1/1, lane 7/7 boots with audio — unchanged in count (5–15 per
boot; final kernel 6 and 5), because they are winealsa's lead-in (§1 B), not
kernel xruns; fp8 had almost no host stalls (2.5 s, no 50 ms gap in the
stream) and still 5.  The first xrun rows of the base boot
are the grab-ahead pattern (writer on time, `wr_ago_us≈3800`, device at real
time over 40 ms, one xrun per tick); in the step-1 boots (fp1, fp2) every
xrun row left is a writer more than 70 ms late.

Every WAV dropout is one `[SND] zeros` run (fp6: 11 dropouts, 11 game-stream
zero rows), and every zero run is winealsa writing only zeros.  With hw
reported (fp7) the runs begin with `q_hw≈0` but `q_vis` 1298–1741: QEMU had
taken everything, while by the play clock 27–36 ms were still playing — and
winealsa, told the buffer was empty and holding under a period of the game's
audio, wrote silence.  With vis reported (fp5, fp6) they begin with 450–1412
frames queued by both measures: winealsa's estimate over-counts either way
(H5-04).

## 5. Findings (wave-8 format)

| ID | symptom | rate | first evidence | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| **H1-01** | exact-zero 10–290 ms runs in loud game audio; alsa-lib underruns | H1 2/2 (h-fp2, h-soak1); base 1/1 (a2-base-fp1); lane 7/7 | `cp-audio2-evidence/game-audio-ab.txt` | `[SND] zeros n=4 t_ms=20791 frames=480 q_vis=1056 q_hw=1056 call_frames=384 call_all_zero=1` (a2-lane-fp5) | two layers: the kernel's xrun cascades (A) and winealsa's lead-in (B, H5-04/H5-05) | wrong output (audible) | A fixed in 44b83122 + a213a9c1 + 044b517e (kernel xruns 534 → 2–15 per boot, final 2 and 2); B open |
| **H5-01** | the kernel counted QEMU's copy-ahead as played; after every host stall QEMU's rate control pays the stalled time back at 2.2× real time: one xrun per 10 ms writer tick | every base boot with a host stall: repro 4/4 rows, game 1/1 | `repro-ab-strix.txt` (idle1, base1-*), `game-audio-ab.txt` | `[SND] xrun n=7 … wr_ago_us=4273 … done40ms=2880` (writer on time); `[SND] stat … max100ms=10560`; `Resetting rate control (65752 frames)` | kernel (`linux_snd.c` xrun rule) × QEMU 10.2 audio | wrong output | fixed in 44b83122, a213a9c1, 044b517e |
| **H5-02** | a genuinely late writer still underruns under full guest load: the 10 ms writer tick > 40 ms late 150× in 150 s | lane1-load 1/1 (8 busy threads on 8 vCPUs + 22 s of host stalls) | `repro-ab-strix.txt` | `[AT] total … late40=150 xrun_state=43` | kernel scheduling under guest load (not audio) | wrong output | open (the game's guest is ~60 % idle, C1-k7) |
| **H5-03** | QEMU's `wav` file is not a timeline: catch-up writes stalled time back-to-back, so WAV length vs wall time only shows debt past 65536 frames | all audio boots | `game-audio-ab.txt` | `wavgaps: wav_over_wall=0.9513` | harness (QEMU wav) | cosmetic (measurement) | open (documented) |
| **H5-04** | winealsa writes a period of silence into a buffer that still holds 9–29 ms: its played estimate `min(data_in_alsa, avail)` counts all free space as played | every lane boot with zeros rows (fp5, fp6, fp7, fp8) | `game-audio-ab.txt` (fp5, fp6 zeros rows) | `[SND] zeros … q_vis=1258 q_hw=1258 call_frames=182 call_all_zero=1 calls=3` | Wine (`winealsa.drv` `alsa_write_data`) | wrong output | open; candidate patch `winealsa-accounting.txt` (not built; mixed in the model) |
| **H5-05** | the game leaves its WASAPI padding under one period for 2–29 ticks in a row (winealsa then writes only silence: 50–220 ms zero runs), densest in the save's loading | fp5, fp6 (and every lane boot's loading phase) | `game-audio-ab.txt` | `[SND] zeros n=6 t_ms=123631 frames=10560 q_vis=48 q_hw=0 call_frames=380 call_all_zero=1 calls=29` | game audio thread × (its loading work, or its event-wait wake latency in Wine/kernel): not separated | wrong output | open |
| **H5-06** | the game exits 2.5 s after launch right after REDEngineErrorReporter starts | 1/7 lane P9 boots (a2-lane-fp4) | `game-audio-ab.txt` | `[P8] end game status=0 ms=2473` | game / H2-early | crash (no menu) | not ours (H2-early's row) |

## 6. Not done / open

- **The audible dropouts (H1-01 B) are not fixed.**  They are winealsa's
  silence lead-in; the next steps are Wine-side or game-side:
  - H5-04: winealsa's played-frames estimate.  Candidate patch, not built
    (needs approval): `docs/cp-audio2-evidence/winealsa-accounting.txt`.  In
    the model it removes the lead-ins for one client shape and adds real
    xruns for another, so it is not recommended as is.  A better-founded
    version would take `snd_pcm_delay()` as the frames still queued.
  - H5-05: why the game's audio thread lets its padding fall under 10 ms for
    up to 29 ticks in a row (its event-wait wake latency under Wine's ntsync
    path, or its own loading work).  Needs a per-thread wake-latency row in
    the futex/ntsync path (not this lane's files).
- H5-02: a writer that really is late (full guest CPU load) still underruns;
  that is the scheduler's business, not the driver's.
- The host stalls themselves (H1-15 class: 26–107 `[CONS] stalls` per boot on
  a shared strix, up to 1.1 s each) are host-side and not ours.
- One P9 boot kind (fpsprobe 1280×720) only; no 28-minute soak with the
  final kernel.
- Repro round 2 on the final kernel: lane only (§4.1 note); the base row is
  round 1.

## 7. Commands

```
# strix, lane tree; guest programs (static musl alsa-lib from the pinned tarball):
bash tools/cp/audio2/build.sh                      # ~/cp/lanes/audio2-alsa/{ATONE,ALOAD}.ELF
# one repro boot (one slot), or the matrix:
bash tools/cp/qemu-slot.sh bash tools/cp/audio2/run-atone.sh idle1 150 ""
HOSTSTALL="300 10 10" bash tools/cp/qemu-slot.sh bash tools/cp/audio2/run-atone.sh st1 150 ""
bash tools/cp/audio2/repro-batch.sh lane1 150      # idle stall lowpad load; obj/cp-audio2/lane1.batch.txt
# host check (no boot):
bash tools/cp/audio2/pace-host-test.sh
# game audio boots (P9 fpsprobe 1280x720, Venus, 32 GiB, WAV deleted after wavgaps):
CP_WINE_STAGE=$HOME/cp/wine-cp2/stage/CP/WINE WINE_BUILD=$HOME/cp/wine-cp2 HUNT_AUDIO=1 \
  HUNT_LOGS=$HOME/cp/lanes/audio2-logs HUNT_EXPORT=/srv/nfs/cp-audio2 bash tools/cp/hunt/run-hunt.sh fp720 a2-lane-fp1
```

## 8. Gates (strix; `docs/cp-audio2-evidence/gates-strix.txt`)

| gate | integ16 batch | lane |
|---|---|---|
| build | ok | ok (a213a9c1, 044b517e) |
| all probes (64 rows, gate 2) | green | 64/64 rc=0 (a213a9c1); snd, sysc, console rc=0 again on 044b517e |
| `snd` probe with the device (`run-probe-snd.sh`) | — | PASS on both (43 rows, WAV 1000 Hz tone) |
| unit | 463 / 0 | 463 / 0 (both) |
| host_tests | 39/41 | 40/42 (044b517e, the two t5 `gaming.md` reds); a213a9c1's run lost its summary to t4_compat_selftest seeing the strix tree's stale `.git` |
| linux_snd_params / xrun-host / host-alsa / pace-host | — | pass / pass / pass / pass |
| castscan | 373, 0 new | 373, 0 new |
| ARM build | ok | ok (both) |
| P2 | G1 G2 G3 PASS | G1 G2 G3 PASS (a213a9c1) |
| P5 (vkcube d3d11 d3d12 fullscr) | PASS | PASS (a213a9c1) |
| P8 Venus | menu | menu at +215 s (a213a9c1) |

P2/P5/P8 were not rerun on 044b517e: the delta is `linux_snd.c` paths that
only run with the opt-in sound device, which those boots do not have.

