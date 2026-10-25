# GB-034-V — gate evidence (VERDICT: RED, clause 3 of 4)

Ticket: GB-034-V (#90) — Verify GB-034: DOOM launches from the desktop and renders a playable frame (T5)
Verifies: GB-034 (#9), shipped on `main` @ `9db1388` (the sha this verdict names)
Verifier: farm Verifier, DEEP (DeepSeek v4.1 Flash, dgx1), host `swarm-farm-2`, own worktree
Run date: 2026-10-25 UTC. Independence: I authored neither GB-034, the DOOM port, nor GB-091's rig.

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | serial proves `DOOM.BIN` **loaded and ran** | **PASS** — `[LAUNCH] DOOM.BIN`, `[WAVE-TRY] DOOM.BIN`, `Loading program for scheduler (v2 AS): DOOM.BIN`, `[ARRIVED] DOOM.BIN rc>0`, `[ALIVE-SET] slot=3 pid=3 st=3 parent=1 ... DOOM.BIN` (process alive for the whole run). The engine's own stdout goes to its window, not the serial console (`grep -c 'I_InitGraphics\|W_Init' serial.log` = **0**); what proves it *ran* is the frame: the Freedoom "PHASE 1" title is drawn after launch, and by ~31 s the attract demo's E1M1 view with a live status bar |
| 2 | centre screendump non-blank **and non-uniform** | **PASS** — region `(480,270)-(1440,810)`: **178 distinct colours**, `518400/518400` non-zero pixels (100.0 %). Desktop background for the same region is 61 colours / identical histogram to boot, so the 178 is the game, not the desktop |
| 3 | at least one injected keypress **changes** the frame | **FAIL** — see below |
| 4 | `replay.jsonl` exists, no magic launcher slot | **PASS** — `replay.jsonl` written by both runs; slot resolved **by name** from the `[MENU]` dump: `[rig] DOOM.BIN slot: found by name = 18` |

## Clause 3 — the failing check, verbatim

Instrument: QMP `input-send-event` (virtio-input qcodes), per the PLAN-014 note — **not** `send-key`.
Every event is recorded to `run_*/replay.jsonl` as `{"t": "input-send-event/key", "qcode": ...}`.

Four `ret` presses on the title screen, 0.8 s apart (frame before vs after; `06_input_frames_diff.log`):

```
02_baseline vs 11_after_ret   changed pixels: 12/2073600 (0.001%)  max channel-sum delta: 467
11_after_ret vs 12_after_ret  changed pixels:  6/2073600 (0.000%)  max channel-sum delta: 467
02_baseline vs 03_final       changed pixels: 19/2073600 (0.001%)  max channel-sum delta: 467
```

The residue is the taskbar clock: the whole frame is otherwise static for the three seconds the four
keypresses span. Same result with `esc` (`07_harness_probe.log`): `12/2073600` changed. During the
attract demo (32 s after launch) the frames move 38–40 % per snapshot, but that is the self-playing
demo; `esc`, `esc`, `ret`, `up` (`09_harness_demoinput.log`, `10_demoinput_diff.log`) never produced a
menu — all four frames are E1M1 gameplay, so no key was consumed.

### The instrument is live — this is not a dead harness

Same run, one key later, `f4` (desktop's "close focused window"):

```
11_after_esc vs 12_after_f4   changed pixels: 2071117/2073600 (99.880%)  max channel-sum delta: 765
```

The DOOM window closed and the desktop returned (105 colours). So virtio key events reach the desktop,
and the launch path itself is driven by them (`f1`, `home`, `down` ×18, `ret`).

### The failure is not DOOM-specific — shared desktop→window pipe

Control app: `CONSOLE.BIN` (slot 0 by name), same harness, keys `l`, `s`, `ret`:

```
11_harness_console_control.log:  [LAUNCH] CONSOLE.BIN in serial: True
02_baseline vs 11_after_l     changed pixels:  9/2073600 (0.000%)
11_after_l  vs 12_after_s     changed pixels: 12/2073600 (0.001%)
12_after_s  vs 13_after_ret   changed pixels: 21/2073600 (0.001%)
```

`13_after_ret.png`: the CONSOLE window is the focused taskbar tab and shows `user@hobgobos:/$` with no
typed text. Characters reach **no** focused window on this tree — not just DOOM. The owner is the
desktop's focused-window stdin forwarding (`forward_key_to_focused` → `desktop_send_hook =
window_write_default`, `src/user/desktop.c:141,268,285`), the same layer GB-088 cross-linked as
"one shared desktop→window pipe". DOOM's own backend is not exonerated — its stdin parser drops a lone
`ESC` byte (`doomgeneric_hobbyos.c` `kq_drain`, `if (c == '\033') { ... continue; }` when not followed
by `[`) — but that cannot explain CONSOLE.BIN.

## Two instrument defects found on the way (fix these or every future re-gate "passes")

1. **The minted gate command cannot fail.** `python3 tools/evidence_rig.py --game DOOM --arch intel`
   exits **0** while launching nothing:
   ```
   [rig] DOOM.BIN slot: found by name = 18 (launching 18, by name)
   ```
   no `[LAUNCH]` line in `serial.log`, and `boot.ppm` vs `gameplay.ppm` differ by
   `45/2073600 (0.002 %)` — the taskbar clock. Cause: `evidence_rig.QMP.key()` sends legacy
   `send-key` (PS/2 i8042) while every key-consuming path on this tree reads virtio-input
   (`get_events`) / stdin, so the rig's `home`/`down`/`ret` are discarded. A gate that reports success
   having measured nothing is not a verdict (gaming.md §4.2).
2. **`make disk.img` stages no IWAD**, so a built image cannot run DOOM at all: `mdir -i disk.img ::/`
   shows `DOOM BIN 893464` and no `WAD` (`01_disk_precondition.log`). The gate as minted omits the
   `tools/fetch_doom_wad.sh` step. Ran it: Freedoom Phase 1 0.13.0, 28 795 076 bytes,
   sha256 `7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d`; after staging,
   `DOOM BIN 893464` + `DOOM1 WAD 28795076`.

## Build / environment notes

- `make disk.img ARCH=intel` first failed on `mkfs.fat: No such file or directory` (the tool exists at
  `/usr/sbin/mkfs.fat`; `/usr/sbin` was not on PATH). Build log `build_intel.log` (exit 2), successful
  rebuild `build_intel2.log` / `build_intel3.log` (exit 0) with PATH including `/usr/sbin`.
- `third_party/llvm-21.1.8/dist-staging` staged once from farm-1 (`192.168.0.230`) per §11.5.
- ARM was not run: this gate names `--arch intel` and the RED is architecture-independent (the shared
  desktop→window pipe), so a second arch would not change the verdict.

## Provenance of the frames

Raw `.ppm` screendumps are gitignored (regenerable); the committed `.png` beside each is the same
frame, and `frames.sha256` pins the raw artifacts.

```
f8b35cf3...  run_input/02_baseline.ppm      (title screen, 4 s after launch)
e630bd99...  run_probe/12_after_f4.ppm      (desktop after F4 closed the DOOM window)
7873bb3d...  run_dwell/03_final.ppm         (attract-demo gameplay, ~31 s, no input)
957593c0...  run_console/13_after_ret.ppm   (CONSOLE window, "ls"+Enter typed, nothing echoed)
```

Harness: `gb034v_harness.py` (extends `tools/evidence_rig.py`'s QMP client with virtio-input keys).
Stats/diff tools: `ppmstats.py`, `ppmdiff.py`, `topng.py`.
