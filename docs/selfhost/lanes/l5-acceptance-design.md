# Lane L5 — Acceptance design + payload staging (first cut)

**You are lane L5 of the HobbyOS self-hosting program.** Your COMPLETE instructions are
this file. Read `~/hobbyos-selfhost/selfhost.md` §0, §3, §5 (AD-8/AD-9/AD-11), §6 (M4),
§8 — this brief is its L5 slice and binds you.

## Identity & tree

- **Worktree:** `/home/sarah/hobbyos-selfhost-lanes/l5` (branch `selfhost/l5-acceptance-design`).
- **NEVER touch the workstation repo** `/home/sarah/Documents/GitHub/HobbyOS` — READ-ONLY
  measurements allowed (e.g. `ls -la` / `du` of built artifacts for sizing). **No pushes.**
- Skills: `hobbyos-build-and-run` (QEMU invocations, deadlock rule), `hobbyos-run-tests`
  (verdict standards).

## Mission

Make the M4 acceptance *runnable*: a design doc + a working first cut of the staging
script + the exact host-side verify-loop spec. The M4 gate must be commands and markers,
not a vibe. **Everything must be measured where measurable** — you have the repo, the
built workstation artifacts (read-only), and host QEMU.

## Deliverable 1 — `tools/selfhost-stage.sh` (real script, tested)

A HOST tool (run from the repo root) that stages the in-OS build payload into `disk.img`:

- Payload (F2 shape, refine with evidence): `/SRC/` ← repo `src/**` (all of it — 18 MB /
  613 files), `Makefile.hobbyos` (once it exists — tolerate absent with a warning),
  `tools/**` (small), `bootloader/BOOTAA64.EFI`, a `limine.conf` template (copy the
  Makefile's arm branch content), and — for the C++ acceptance set only — the libcxx
  include subset (`third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include/**`;
  measure its file count + size; recommend include/exclude).
- Use `mcopy -s` (recursive; creates directories) + `mmd` where needed. **Validate on a
  scratch image first**: `dd`-less flow: make a scratch image with `mkfs.fat -F 16`, run
  the staging against it, then **verify with `mdir`** that the long-name tree arrived
  (this is a real host-side test — do it and record the transcript). Then test against a
  copy of a real `disk.img` if one is available (the workstation repo has one — copy it
  to scratch; do not modify the original).
- Idempotent (re-run replaces), `-f` to force, prints a size report (`mdir` totals |
  files | bytes), exits non-zero on any mcopy/mmd failure. Keep it shell-conservative
  (it's a HOST script — bash fine, but keep it simple).
- Name/path discipline: it writes REAL long names (LFN is proven); still, check and report
  how many payload entries exceed 8.3 (informational for F8).

## Deliverable 2 — `docs/selfhost/acceptance.md`

The complete, executable acceptance design:

1. **Stages S0-S5** with exact in-OS commands + expected outputs:
   - S0 `make -f Makefile.hobbyos check` (single TU compile smoke)
   - S1 `… user` (acceptance set AD-9 → .bin; both run in-OS)
   - S2 `… kernel` (71 TU → hobbyos.elf → objcopy hobbyos.bin)
   - S3 `… disk` (OUT.IMG via MKFSFAT/MCOPY)
   - S4 host: extract OUT.IMG from disk.img, boot it, assert kernel marker
   - S5 host: boot again and run the in-OS-built user program
   Each stage: the `[SELFHOST] phase=…` markers it emits (align vocabulary with L4's
   sketch), timeouts, and the verdict extraction (marker substring + `FAIL`-token scan;
   never exit codes).
2. **Sizing math (measured here, today):** disk free/used (workstation `mdir`: 133,032,359
   used / 939,114,496 free — re-measure); payload du/counts (src/, libcxx include subset,
   bootloader EFI, `hobbyos.elf`/`hobbyos.bin` sizes from the workstation tree — READ-ONLY;
   if the per-arch `obj/arm/*` artifact sizes exist there, tabulate the biggest user bins
   too); OUT.IMG size decision proposal (≤256 MiB; show the arithmetic for at least
   {64, 128, 256} MiB: OUT.IMG + payload + build outputs must fit in 939 MB free **with
   margin for obj/ + temp**).
3. **Host verify-loop spec** for `run_selfhost_build_test.py` (wave-2 implementation; you
   deliver the spec + a skeleton comment block): boot QEMU (ARM args per Makefile:197,
   `-display none`, serial to file), drive the build (how: a script `/SRC/AUTORUN.TXT`?
   or interactively via qmp keystrokes? — decide from how existing python runners drive
   the shell; the desktop/`make test` precedents exist — pick the most robust, document
   why), wait with generous timeout, extract OUT.IMG via `fat16img.py get` (read-only
   extractor exists), then boot OUT.IMG with the same QEMU args but `-drive` = OUT.IMG and
   assert markers. Include the exact `qemu-system-aarch64` arg lines and where the EDK2
   firmware file comes from (Makefile:25-31 area defines it per-OS).
4. **Budget guidance:** QEMU ARM is TCG — checkpoint-based estimates (host-clang timing of
   one kernel TU × 71 × a TCG multiplier you justify; mark the multiplier VERIFY until M2
   measures it), plus the plan to run long acceptance runs on the CI VM (`hobytest`,
   `ssh sarah@192.168.0.20`) — locate the CI runner script from repo docs (`grep -rn
   "hobbyos-ci" docs/ *.md`) and note whether it still exists at the referenced path (the
   workstation's `~/bin` is empty — record what you find; if the script is gone, spec what
   a replacement needs: rsync tree → build → run tier → marker+FAIL gate).
5. **Failure-diagnostics plan:** per-stage serial capture files, the marker vocabulary,
   the first things to check for each stage's typical failure (linking OOM? missing .BIN?
   disk full?), and the retry-cost table (what you can re-run without rebuilding the disk).

## Constraints (hard)

- All scratch under your worktree / `~/.hermes/cache/scratch/`; the only `disk.img` you
  touch lives in YOUR worktree (none exists yet — create via `make disk.img`? NO: too
  heavy; use a scratch mini-image for staging tests; the real one is wave-2/acceptance).
- Do not edit `Makefile`, `src/**`, `selfhost.md`, `.gitignore`, other lanes' paths.
- Command availability on this host: `mcopy/mmd/mdir` ✓, `mkfs.fat` ✓, `qemu-system-aarch64`?
  (check; if missing, note it — wave 2 needs it), `fat16img.py` ✓ (repo root).
- Commit cadence ≈ every 30-45 min; subjects `selfhost: l5: <what>`; stop committed.

## Report (write + validate before finishing)

`docs/selfhost/lanes/l5-report.json` (commit; `python3 -m json.tool`):

```json
{"lane": "l5", "status": "complete|partial|blocked",
 "commits": ["sha"], "artifacts": ["path"], "evidence": ["path"],
 "sizes": {"payload_bytes": 0, "outimg_proposal": "…"},
 "blockers": ["..."], "next_steps": ["harness-lane brief notes"],
 "notes": "staging test transcript path; open questions for M3"}
```
