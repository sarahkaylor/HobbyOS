# w10-fstime evidence — RTC-backed FAT16 dirent timestamps → INCREMENTAL in-OS make

Lane: `selfhost/w10-fstime` · Branch: `selfhost/w10-fstime` · Worktree: `~/hobbyos-selfhost-lanes/w10-fstime`

## 1. The defect (proven on the PRE-FIX kernel)

In-OS file writes left the FAT16 dirent `date`/`time` fields zeroed
(`fat16.c` create/mkdir paths) and the kernel stat path returned a constant
`st_mtime = 0`. Result: every in-OS file "looks like" 1980-01-01 / epoch 0,
so GNU make can neither see a target newer than its source nor a source
newer than a stale target — incremental rebuilds are impossible.

Serial + raw-disk evidence (pre-fix `MODE=selfhost` boot, `::/W10` probes):

- A freshly `touch`-created file stats as `mtime=0 ctime=0 UTC=1970-01-01`.
- Raw dirent of an in-OS-created file (`A.TXT`, `A.O`): `date=0 time=0`
  (FAT-decodes to INVALID / 1980) while host-staged files carry real
  `2026-10-14 16:05:30` fields.
- `touch A.C` does NOT advance mtime (`mtime=0` stays `0`), and the make
  that follows does NOT rebuild `A.O` even though its source was just
  rewritten — dependency tracking is dead.

| file | what | key lines |
|---|---|---|
| `evidence-w10/pre-fix-boot.serial` | full serial | `A.TXT: stat=0 size=0 mtime=0 ctime=0 UTC=1970-01-01`; STEP3 touch → no rebuild |
| `evidence-w10/pre-fix-boot.trim.txt` | trimmed excerpt | same, readable |
| `evidence-w10/pre-boot-w10.dump` | host-side dirent dump before boot | staged files 2026-10-14 |
| `evidence-w10/post-boot-prefix-w10.dump` | dirent dump after pre-fix boot | `A.TXT date=0 time=0`, `A.O date=0 time=0` (INVALID/1980) |

Probe sources (reproducible): `W10PRE.SH`, `PRERUN.txt`, `STATP.C`,
`P1/{A.C,MAKEFILE-PROBE}`.

## 2. The fix (kernel)

- `src/kernel/fat16.c`: `fat16_stamp_entry()` writes the FAT16-encoded RTC
  time into a dirent, sourced from the kernel REALTIME clock
  (`hb_clock_realtime_ms()`, kernel/time.c — boot-anchored RTC, UTC).
  Encodings follow the FAT16 spec: `date = ((year-1980)<<9)|(month<<5)|day`,
  `time = (hour<<11)|(min<<5)|(sec/2)` (2-second resolution).  Applied on:
  create (`fat16_open`), `fat16_mkdir`, write/appends (`fat16_write`),
  `O_TRUNC` (`fat16_truncate`) and `ftruncate` (`fat16_truncate_to`).
  `fat16_entry_epoch()` decodes a dirent back to epoch seconds.
- `src/kernel/fs.c`: `file_stat_fd` / `file_stat_path` now return real
  `st_mtime = st_ctime = st_atime` from the dirent (FAT has no atime).
- Timezone: **UTC everywhere**.  The RTC is UTC on both arches (QEMU
  default), there is no TZ database, and userland `time()`/sysinfo 6 use the
  same clock — so FAT stamps and `stat` agree.  (Host-side mtools staging
  writes the host's local time; guest RTC is UTC, so in-OS stamps are +5 h
  vs the CDT-staged files — both 2026-10-14, date-consistent.)

## 3. Acceptance (POST-FIX kernel, serial evidence)

`evidence-w10/post-fix-boot.serial` + `post-fix-p3.serial`:

- Create/write/mkdir carry real times: `A.TXT` `mtime=1792012324
  UTC=2026-10-14 21:12:04`; raw dirents `date=23886 time=43394`; `D1` dir
  `mtime=1792012374 UTC=2026-10-14 21:12:54` (`post-fix-w10.dump`).
- **2-TU project** (`P1`, explicit rules): first `make` compiles
  (`P1-COMPILING-A` + `/CP.BIN A.C A.O`); `sleep 3s`; second `make` — no
  compile line.
- **recipe-less phony, literal verdict** (`P3`): first `make` compiles
  (`P3-COMPILING-A`), second `make` → `MAKE.BIN: Nothing to be done for
  'all'.`; rewrite source (`cp A2.C A.C` — the kernel's real
  `O_TRUNC`+write path) → `make` compiles exactly `P3-COMPILING-A`.
- **5-TU project** (`P2`): full build compiles B..F; `sleep 3s`; `make` —
  no compile; rewrite only `D.C` → `make` compiles exactly
  `P2-COMPILING-D` (+`/CP.BIN D.C D.O`), no B/C/E/F.
- Rewrite (not `touch`) for the rebuild trigger: the in-OS `TOUCH.BIN` only
  creates (open O_CREAT, no data, no utimens), so it never moves mtime;
  `cp` exercises the kernel's truncate-and-write path which now stamps.

## 4. Failing-first unit tests (kernel tier)

`src/kernel/fat16_test.c`, four new deterministic range tests:

- `test_fat16_rtc_create_stamp` — create → dirent year >= 2026 and decoded
  epoch within [now − 2 d, now + 5 s].
- `test_fat16_rtc_write_updates_mtime` — append after a bounded ≥2 FAT tick
  wait advances the decoded mtime by ≥ 2 s.
- `test_fat16_rtc_stat_readback` — `file_stat_path`/`file_stat_fd` return
  the dirent's decoded epoch (≥ 2025-10-14), ctime == mtime.
- `test_fat16_rtc_mkdir_stamp` — mkdir → dirent year >= 2026, epoch in range.

## 5. Gates

| gate | result |
|---|---|
| `./run_unit_tests.sh` (ARM, MODE=unit_tests) | see `w10-fstime-report.json` |
| `EXTRA_QEMU_ARGS='-enable-kvm' ./run_unit_tests_intel.sh` (x64) | see report |
| `make test` wave (ARM integration) | see report |

## 6. Two-second-resolution caveat (documented contract)

FAT16 timestamps have 2-second resolution.  GNU make treats a target whose
mtime equals its prerequisite's mtime as up to date.  Consequence: two
writes to a file that land inside the same 2-second bucket produce the same
mtime, so an in-OS `make` run issued within the same FAT tick after a
rebuild may consider the target current even though content changed (or
may skip a just-rewritten source).  The acceptance probes therefore sleep
≥3 s between build and re-check, mirroring how real in-OS builds should
sequence: writes ≥2 s apart always produce distinguishable mtimes.  In-OS
`make` runs that rebuild quickly after a change should expect this
granularity (equal mtimes = up to date), exactly like GNU make on any
low-resolution filesystem.
