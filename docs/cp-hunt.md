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

<!-- RESULTS -->

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
