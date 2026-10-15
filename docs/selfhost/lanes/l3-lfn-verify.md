# Lane L3 — FAT16 LFN build-payload verification ( + gap fixes + permanent tests)

**You are lane L3 of the HobbyOS self-hosting program.** Your COMPLETE instructions are
this file. Read `~/hobbyos-selfhost/selfhost.md` §0, §3 (FAT16 paragraph), §4 (W-A), §7 —
this brief is its L3 slice and binds you.

## Identity & tree

- **Worktree:** `/home/sarah/hobbyos-selfhost-lanes/l3` (branch `selfhost/l3-lfn-verify`,
  based on `main` of `/home/sarah/hobbyos-selfhost`). Seeded (libcxx tree + `_staging`).
- **NEVER touch the workstation repo** `/home/sarah/Documents/GitHub/HobbyOS`. **No pushes.**
- You are the **sole Makefile writer this wave** — wire your test targets carefully,
  keep hunks minimal, in the userland test region.
- Skills to load FIRST: `hobbyos-kernel-constraints` (kernel rules — load-base, dispatch,
  testing), `hobbyos-run-tests` (tiers + verdict reading), `hobbyos-build-and-run` (build
  matrix + deadlock rules + pkill/fuser discipline), `hobbyos-development` (user-program
  conventions for the in-OS test).

## Mission

The FAT16 driver **already implements VFAT long file names** (read+write: `fat16.c:36-72,
412, 454, 569-622, 985, 1100, 1222, 1332`). Your job: **prove it covers the in-OS build
payload's exact needs, fix any gaps, and land permanent tests.** This is verification-first,
not re-implementation. The payload (F2/AD-11) is a real subtree copy placed by `mcopy`,
with long names everywhere: `src/kernel/program_loader.c` (14-char base),
`src/user_include/` (12-char dir), `third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/`
(deep + long components), plus build outputs created **in-OS** by the toolchain
(`graphics_test.o`, nested `obj/` dirs, `hobbyos.elf`).

## Phase 0 — understand before touching (commit a findings note)

1. Read `src/kernel/fat16.c` end to end. You care about: `match_name`, `lfn_*` helpers,
   `find_entry_in_root/subdir`, `alloc_lfn_run`, `fat16_open`, `fat16_read_dir`,
   `fat16_unlink`, `fat16_rename`, `fat16_mkdir`, `fat16_resolve_path`/`_parent`.
2. Find every consumer: which syscalls/paths reach these (`src/kernel/fs.c`, `vfs.c`,
   the `read_dir` syscall path in `trap.c`, the desktop's name usage), and the path-buffer
   limits (max path length constant — grep; matters for deep LFN paths).
3. Read `src/kernel/fat16_test.c` + how `run_unit_tests.sh` runs the unit tier (tests run
   in-kernel against `disk.img`; `fresh_disk` refreshes it — read `run_unit_tests.sh`).
4. Read the existing in-OS test for the big-file path (`src/user/fatbig_test.c`) and its
   Makefile wiring — you will mirror that shape for LFN.

Identify and list (in your report) the **uncovered behaviors** before writing any code.

## Phase 1 — the payload-shape test (host side + in-OS side)

Build a fixture that mirrors the payload shape using the HOST mtools (`mcopy`/`mmd`
support long names; the disk recipe pattern lives in the Makefile `disk.img` target):

- Add test fixtures to a COPY of the flow — do NOT disturb the main `disk.img` recipe
  semantics; add your own fixture files/dirs via the existing `mcopy` pattern in the
  `disk.img:` recipe or a dedicated test disk if that fits the repo pattern better.
- Shape to create (through mcopy/mmd only — that's what the acceptance does):
  - `::/src/kernel/program_loader.c` (deep path, long basename)
  - `::/src/user_include/graphics/window.h` (long dir + nested)
  - a 30+ char component: `::/src/llvm-project-21.1.8.src-EXAMPLE.c` style (mirroring
    the libcxx vendor depth with a dummy file, don't stage the real tree)
  - a mixed-case name and a name with a **collision family** (`longfilename.txt`,
    `longfilename2.txt`, `longfil~1.txt` style edge) — verify unique resolution.
- **In-OS test `LFN_T.BIN`** (`src/user/lfn_test.c`, mirror `fatbig_test.c` wiring):
  opens every fixture by long name read+read-back content; `read_dir` listing shows the
  long names; rename a long name → new long name; mkdir a long-name dir; create a NEW
  long-name file in-OS (write path) and re-open it; unlink it; assert all with
  `print_console` PASS/FAIL lines, `exit(failures)`; add to the `KERNEL_MODE_TEST` boot
  list (`src/kernel/main.c` — mirror FATBIG_T's registration; find it) + disk.img deps +
  mcopy lines.
- **Unit tier additions** (`src/kernel/fat16_test.c`): cover the LFN parse edge cases you
  can reach in-kernel (truncated LFN runs, checksum mismatch rejection, case handling,
  short-name fallback). Follow the file's existing test style exactly.

## Phase 2 — gap fixes (only what Phase 1 proves missing)

Anything Phase 1 exposes (e.g., a name-length boundary, a read_dir case bug, a rename
edge) — fix in `fat16.c` with a test that fails first (repo prefers failing-test-first).
**Do not redesign working paths.** Keep to the file's existing conventions (spinlocks,
caches, sector I/O helpers — see `hobbyos-kernel-constraints` for the load-base rules
that DO apply to kernel code).

## Gates & evidence (run BOTH, on THIS worktree)

1. `./run_unit_tests.sh` — fully green (`Tests run: N, failed 0`; read the log, not the
   exit code; it can take several minutes — that's built-in).
2. `timeout 1500 make test` (ARM) — completion marker `System halt` present AND zero
   `FAIL`/`FAILED` tokens in the raw log; new LFN_T checks grepped by name; FATBIG_T still
   green. Archive the raw logs under `docs/selfhost/lanes/l3-*.log` (commit, trimmed to
   the interesting parts if huge — keep the failure-scan-relevant lines).

QEMU etiquette on this shared host: `fuser -k disk.img` (exact file!) before re-runs if a
previous run was killed; never `make clean`; never broad pkill; `timeout` every run.

## Constraints (hard)

- Commit cadence ≈ every 30-45 min; subjects `selfhost: l3: <what>`; stop at a committed
  checkpoint if the tool budget nears. `tools/cstyle.py format`+`check` ONLY the C files
  you touch (never `make format`).
- Do not edit: `src/libc/**`, `src/user/libc.c`, `Makefile` beyond your test wiring,
  other lanes' files, `selfhost.md`, `.gitignore`.
- If you find a pre-existing test that is red for unrelated reasons, note it, do not fix
  it (unless it blocks your gate — then say so in the report).

## Report (write + validate before finishing)

`docs/selfhost/lanes/l3-report.json` (commit; `python3 -m json.tool`):

```json
{"lane": "l3", "status": "complete|partial|blocked",
 "commits": ["sha"], "artifacts": ["path"], "evidence": ["log paths"],
 "gaps_found": ["..."], "gaps_fixed": ["..."], "gaps_deferred": ["..."],
 "blockers": ["..."], "next_steps": ["..."], "notes": "unit N/N; wave verdicts"}
```
