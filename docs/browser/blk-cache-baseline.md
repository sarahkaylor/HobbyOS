# Block-cache browser/apps/desktop baselines (pre-cache kernel @ 71b980c)

Lane: `blkcache/browser` — capture E2E baselines on the CURRENT (cache-less)
kernel so the block-level read cache can be validated against identical runs
later. Branch `blkcache/browser` @ `71b980c` (`make run completeness …`, tip
of main lineage this lane is based on).

## How the runs were wired (and what was fixed)

- **Browser runner self-hosts the proxy.** `tools/run_browser_accept.sh`
  starts `tools/wiki_proxy.py` itself on `--port` and writes
  `/tmp/wiki-proxy-port-<instance>` (header lines 118-137). So "start the
  proxy" = pass `--port 8811 --instance blkcache`; nothing external to
  start. Port 8811 verified free before each run.
- **FIX (env): the runner's default fixture dir is dead.**
  `FIX_DIRS` defaults to `/home/sarah/webkit-lanes/fs-v1/HobbyOS/…/fixtures`,
  which no longer exists. The live wk3 fixture set is
  `/home/sarah/webkit-hobbyos/HobbyOS/continuation/wk3/fixtures` (FIX01..FIX05.HTM,
  IMG01.PNG, PROBE01.JS) plus the repo's `tests/fixtures/browser` (HOME/TALL).
  Override with `WIKI_PROXY_FIXTURES` (runner reads it, line 61).
- **FIX (harness): `make desktop_test` is NOT shared-host safe.**
  `run_desktop_test.py` calls `pkill -9 -f qemu-system-aarch64` /
  `qemu-system-x86_64` unconditionally (line 26-27; also after every
  screendump). While taking the baseline, other lanes' VMs were live
  (x64-blk-bench-wave, blkcache-selfhost, blkcache-ios) — a stock
  `make desktop_test` would have killed them. Added
  `tools/run_desktop_test_scoped.py`: identical acceptance flow (boot
  `MODE=desktop_test`, wait `SCREENSHOT_READY`, QMP screendump, non-blank
  check) but kills ONLY the qemu whose cwd == the tree.
- **FIX (timing): stock 30 s SCREENSHOT_READY bound is too tight for this
  host's ARM TCG boot.** First scoped run at the stock 30 s bound died
  mid-boot (the editor test had legitimately only reached File→Open). Re-run
  with a 180 s bound — the stall below is a product-side stall, not boot
  slowness (boot completed cleanly, 0 kernel faults).
- Baseline disk/hobbyos.elf (desktop-mode, pre-rebuild) snapshotted to
  `evidence/blkcache/artifacts/` (untracked): `disk-img-desktop-seeded.bin`
  (sha256 d15c0f22cb9ee358516ce85712902abec9ebd2962f6b02dc61b3cbe2b4d8289a),
  `hobbyos-elf-desktop-seeded.elf` (07f38c06d9505394799f6628e37d754daf8410e4015b85f84c77e2d6572b1bac).
- `BROWSER.BIN` verified on the seeded `disk.img` (87,515,376 B) and the
  webkit build exists at
  `~/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess` — no disk
  rebuild was needed for the browser legs.

## Run 1 — browser acceptance, fixture mode  → **PASS (8/8)**

Command (worktree = `/home/sarah/hobbyos-lanes/blkcache-browser`):

```sh
WIKI_PROXY_FIXTURES="/home/sarah/webkit-hobbyos/HobbyOS/continuation/wk3/fixtures:/home/sarah/hobbyos-lanes/blkcache-browser/tests/fixtures/browser" \
flock -w 7200 /tmp/fs-accept-blkcache.lock \
tools/run_browser_accept.sh --mode fixture --port 8811 --instance blkcache \
  --evdir evidence/blkcache/fixture
```

Decisive lines (runner stdout):
```
proxy self-check HTTP 200 28275 B ; host-side robots.txt marker lines: 35
fixture self-check HTTP 200 267 B ; host-side /fixture/FIX01 marker lines: 1
{"verdict": "pass", "n_pass": 8, "n_gated": 0, "n_fail": 0, "notes": [], ...}
verdict=pass n_pass=8 n_gated=0 n_fail=0 rows=8
```

report.json: `evidence/blkcache/fixture/report.json`. Key rows (id, status,
load_ms, checksum, frames):
```
fixture-FIX01  pass  6985  0x118e12f6  frames=2
fixture-FIX02  pass  14595 0x7b023900  frames=2
fixture-FIX02D pass  22174 0x334959fb  frames=2
fixture-FIX03  pass  29929 0x334959fb  frames=0
fixture-FIX04  pass  37906 0xf0e9e9e0  frames=0
fixture-FIX05  pass  45132 0xb75d3854  frames=2
fixture-HOME   pass  52675 0xb75d3854  frames=0
fixture-TALL   pass  60093 0xc7577449  frames=0
```
Full boot markers present (`[WIN] BOOT` 1, `[WIN] created` 1, `geom` 1);
`[WIN] load-ok` 9, `net fetch` 8, `net persist` 8, `FATAL` 0, `IDLESTUCK` 0.
Netlog 12 lines (10 fixture serves at expected byte sizes 267–817, 1 relay
self-check, 1 proxy-start). PAGE-NET.HTM copy-back 817 B
sha256 47e0fe49aabd13c429bba225ab3241fa4a7d8c40ad84d4e371311f96e25349f7
(= TALL, the last row, expected). 9 screenshots.

## Run 2 — browser acceptance, reader mode (Habitat)  →  **PASS (1/1)**

Command: same wrapper, `--mode reader` (topic default Habitat),
`--evdir evidence/blkcache/reader`.

Decisive lines:
```
{"verdict": "pass", "n_pass": 1, "n_gated": 0, "n_fail": 0, "notes": [], ...}
[WIN] net fetch url=http://10.0.2.2:8811/reader/Habitat ok=1 status=200 bytes=1373 effective=http://10.0.2.2:8811/reader/Habitat err=
[WIN] load-ok url=http://10.0.2.2:8811/reader/Habitat ms=8628
[WIN] doc title=|1365|1|4|0 ; [WIN] links 4;/reader/HTML@…|/reader/CSS@…|#sec2@…|https://en.wikipedia.org/wiki/Habitat@…
verdict=pass n_pass=1 n_gated=0 n_fail=0 rows=1
```
report.json: `evidence/blkcache/reader/report.json`. PAGE-NET.HTM 1373 B
sha256 b541d6dda0f71422758cec5cc6dcf3d87fad887d6948798cccb61143d66c3305.
`FATAL` 0, `IDLESTUCK` 0. Real upstream bytes (live REST extract via the
proxy's host curl).

## Run 3 — scoped apps E2E (APPS_T)  →  **fail (PRE-EXISTING RED, reproduced)**

Command:
```sh
python3 tools/run_apps_test_scoped.py /home/sarah/hobbyos-lanes/blkcache-browser blkcache 600
```
Runner exit 1. `make MODE=apps_test run` rebuilt the tree disk in apps_test
mode, booted, drove the real WM through the 10-app harness.

Runner receipt:
```
[scoped] killed own qemu pid=1554272
[scoped] VERDICT=None n_pass=3/10 log=/tmp/apps_test_scoped_blkcache.log
```
`VERDICT=None` because the serial splice corrupted the verdict line
(`APPS TEST FAI5LED`, CALC fault-line interleaved) so the runner's exact
`APPS TEST FAILED` poll never matched; it ran its 600 s budget, then killed
only its own qemu. The runner's naive `n_pass=3` undercounts (console
splices); the TRUE progress below is from the serial state machine.

What the serial actually shows (state machine is strictly sequential —
app_passed() increments; any failure exits the harness):
- Clean line: `[APPS_T] PASS 1/10 FILES.BIN: window+title+content+fb; down-arrow selection, drag & drop move, Enter opens a .TXT; closed`
- `5/10` reached: `[APPS_T] PASS 5/10 HEX.BIN … : window+title+content+fb … ; closed`
  (a 5/10 pass PROVES apps 1–4 passed before it: FILES, CALC, CLOCK, SYSMON)
- Per-app pass-only extra fragments each seen once: `down-arrow selection`
  (FILES), `shows` (CALC's `2+3= shows "> 5"`), `HH:MM:SS` (CLOCK).
- Verdict printed (spliced): `APPS TEST FAI5LED ( (CALC.BIN)TASKS.BIN fault! … nteract`
  → decode: `APPS TEST FAILED (TASKS.BIN: interact)`.
- `APPS TEST PASSED` NOT present → harness exited with failure at app #6
  (TASKS.BIN), stage `interact`. `[LAUNCH] TASKS.BIN` present.
- Concurrency noise: a `CALC.BIN` fault-loop (`[KERNEL] User process 5
  (CALC.BIN) fault! EC 0x…20 ELR 0x1000004050`, 820k+ lines) — the
  documented nondeterministic form (theme.md §5: “a control run with a
  varied harness hit an unrelated CALC fault-loop”), which also kept QEMU
  alive past the harness exit until the runner's budget killed it.

**Conclusion: reproduces the documented pre-existing red exactly in form —
5/10 apps (FILES, CALC, CLOCK, SYSMON, HEX) pass, then the harness stops at
TASKS.BIN (`interact - window vanished during the functional check`).
NOT a regression from the cache work; cache re-run must show the SAME stop
point.** Raw log (untracked): `/tmp/apps_test_scoped_blkcache.log` (672k churn
from the CALC loop; decisive lines above).

## Run 4 — desktop_test (screendump validation)  →  **fail (stock & scoped), product stall at tip**

Command (scoped equivalent; `make desktop_test` itself would broad-pkill
other lanes' qemu, see “what was fixed”):
```sh
python3 tools/run_desktop_test_scoped.py /home/sarah/hobbyos-lanes/blkcache-browser blkcache 180
```

Stock-bound run (30 s, mirrors `make desktop_test`'s verdict path): exit 1,
`VERDICT=fail (ready timeout)` — QEMU killed before SCREENSHOT_READY.
Extended-bound run (180 s): exit 1, `VERDICT=fail (ready timeout)` — the
editor test NEVER emits SCREENSHOT_READY, even given 180 s. Verdict is
stable: **the desktop_test tier does not reach its screendump gate at this
tip.**

Serial tail (both runs, identical through the stall):
```
[TEST] editor launched.
[TEST] Injecting 'h' 'e' 'l' 'l' 'o'...
[TEST] Moving mouse to File menu...
[TEST] Moving mouse to Open item...
[TEST] Clicking Open item...
[TEST] flush_fb: test_state=other num_windows=1   ← repeats forever (state 6)
```
- Boot clean: `Mode: DESKTOP_TEST` present, 0 `[KERNEL] … fault`, 0 `FATAL`,
  `SCREENSHOT_READY` count 0.
- editor_test state machine (src/user/editor_test.c): `STATE_WAIT_FINISH`
  (6) requires the File→Open dialog text (`Open F…` + a real disk entry,
  `MEMTES`) inside `windows[0].text`; `num_windows` stays 1 — the dialog
  never renders. Prior known-green `SCREENSHOT_READY` receipt in-tree
  (`THEME-VERIFY-REPORT.md`, `theme.md`) predates tip commits `dbaec3c`
  (desktop display mode raise 1024×768 → 1920×1080) and `48e53d7`
  (damage-rect present) — hypothesis (unverified): the mode/menu layout
  change moved the hard-coded `Open item` click target
  (`(20,64)` in editor_test.c) or broke the dialog render path.
- Raw log (untracked): `/tmp/desktop_test_scoped_blkcache.log`.
  Screendump was never taken (no SCREENSHOT_READY), so there is no
  black/blank verdict — consistent with the stock runner's fail path.

**Baseline meaning:** desktop_test is red at this tip in a stable, clean-boot
stall form. Cache re-run must reproduce THIS exact stall (state 6,
num_windows=1, no SCREENSHOT_READY) — any change in form or a new fault is
cache-attributable; this baseline is not a green tier.

## Re-run commands for the cache-validation kernel

In the cache kernel's worktree (same tools; set `WIKI_PROXY_FIXTURES` as
above; keep a fresh snapshot of the desktop disk before the apps/desktop
runs, since those rebuild `disk.img` in place):

```sh
# 0) desktop-mode disk for the browser legs (kernel rebuild is incremental;
#    BROWSER.BIN re-stages from
#    ~/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess — verify:
#    mdir -i disk.img ::/BROWSER.BIN)
make ARCH=arm MODE=desktop hobbyos.elf disk.img
cp -f disk.img evidence/blkcache-cache/disk-img-desktop.bin   # snapshot

# 1) fixture  (expect: verdict pass 8/8)   — use a FRESH evdir
flock -w 7200 /tmp/fs-accept-blkcache.lock tools/run_browser_accept.sh \
  --mode fixture --port 8811 --instance blkcache --evdir evidence/blkcache-cache/fixture

# 2) reader   (expect: verdict pass 1/1)
flock -w 7200 /tmp/fs-accept-blkcache.lock tools/run_browser_accept.sh \
  --mode reader  --port 8811 --instance blkcache --evdir evidence/blkcache-cache/reader

# 3) apps     (expect: SAME pre-existing red — 5/10 pass, stop at TASKS.BIN
#    interact; NEVER run_apps_test.py — broad pkill)
python3 tools/run_apps_test_scoped.py /home/sarah/hobbyos-lanes/blkcache-browser blkcache 600

# 4) desktop  (expect: SAME stall — state 6, no SCREENSHOT_READY;
#    NEVER `make desktop_test` — broad pkill of every host qemu)
python3 tools/run_desktop_test_scoped.py /home/sarah/hobbyos-lanes/blkcache-browser blkcache 180
```

Comparison contract: identical run-to-run *forms* for the two red tiers, and
no new failures in the two green browser tiers (8/8 and 1/1 must stay green;
`load_ms` numbers are the perf signal the cache is expected to improve).

## Raw (untracked) artifacts

- `evidence/blkcache/fixture/` — report.json, serial.log, driver.log,
  net.log.jsonl, step-*.json, 9 PNGs, host-robots.txt,
  host-fixture-FIX01.txt, PAGE-NET.HTM, WNPAGE.HTM, staged disk.img.
- `evidence/blkcache/reader/` — report.json, serial.log, driver.log,
  net.log.jsonl, step-*.json, 2 PNGs, artifacts.
- `evidence/blkcache/artifacts/` — desktop-mode disk + elf snapshots (see
  “wiring” above for sha256).
- `/tmp/apps_test_scoped_blkcache.log`, `/tmp/desktop_test_scoped_blkcache.log`.
- New harness file (committed): `tools/run_desktop_test_scoped.py`.
