# HobbyOS self-hosting — todo (parked issues)

Created 2026-10-14. Program status: `selfhost.md` (§8.3 = frozen acceptance criteria; §11 = fix log — don't re-chase anything recorded there). Evidence for every item lives under `docs/selfhost/lanes/`.

Ordering = priority. Items 1–3 are the active build-velocity work (user direction 2026-10-14: raise these ahead of the acceptance re-cut).

## P0 — active
1. **FAT dirent writer** — files created in-OS get a mangled duplicate dirent twin (`FAT16.O` == `FAT16-AB.O.T`, same cluster); for exactly `nfs_test.o`/`cpu.o` the clean entry is lost → in-OS lld link fails (`cannot open`). Deterministic, filename-keyed. Evidence: `docs/selfhost/lanes/evidence-w7/t43-s2-fix.dirents.txt`. **[w9-dirent]** DONE (`9beedc4`): writer (stale dirent write-back after rename-while-open; LLVM TempFile pattern) + reader (exact 8.3 beats colliding alias); S2 crown PASS, zero twins; units 320/0 + 324/0. Re-check item 7 against the fix.
2. **FAT timestamps** — in-OS file writes leave 1980 dates, so in-OS make can never be incremental (full 59-TU rebuild every attempt ≈ 90 min). Fix: real RTC times on create/write/mtime-update + correct stat readback. Acceptance: a 2-file Makefile in-OS, second `make` = "up to date", source-touch rebuilds only that TU. **[w10-fstime]** ✅ DONE (`465c9b4`): RTC stamps on all write paths + stat readback; `Nothing to be done` + single-TU rebuild proven; units 322/0 + 326/0; wave green.
3. **Parallel in-OS builds** — MAKE.BIN support/viability of `-j8` (8 cores idle today). Assess port (jobserver/pipes/SIGCHLD), fix what's missing, verify correctness (byte-compare vs serial) + measure speedup on a synthetic project. **[w11-makej]** ✅ DONE (`1ac7c7d`): jobserver + bounded-poll pselect stub; 16/16 byte-identical, fail propagation, no zombies; 19.7 s serial vs 10.0 (-j4) / 9.3 (-j8) synthetic; cap -j8.

## P1 — known robustness bugs (this session)
4. **Build-wedge class** — guest freezes mid in-OS build; serial goes silent, CPU 0%. Three sightings this session: battery-4 VM wave, battery-6 local wave (after GREPTEST), w7 S2 stall at TU 45/59. Each costs a full rebuild. Needs a real diagnosis (suspects: console/UART storm saturation under churn, MTTCG race, loader interaction). Adjacent ambient: `SHELLTEST WATCHDOG: protocol stalled 25 s`.
5. **Slot-pressure robustness** — loader backoff vs stuck-table detection; test-side retry tolerance (CXX_T `condvar_wait_notify`, THRD_T `futex-wake`); re-verify behavior after the FS work (the wave + torture interplay).
6. **In-OS make pattern rules vs FAT case** — FAT readdir returns UPPERCASE 8.3 names; make's pattern-prereq match is case-sensitive → `Makefile.hobbyos` uses 59 explicit rules as a workaround. Replace with proper case handling in the in-OS make/filesystem path when convenient.
7. **`mcopy -o` over LFN duplicates** (w6 note) — likely same writer class as item 1; confirm fixed by w9, else track here. Also re-sample after w9: the VM `[LFN] FAIL ... "listed with long name"` checks (2 sightings, different sub-checks, not reproducing on rerun — possibly the same dirent-writer family).
8. **Host-tier `EDITOR.BIN_host` hang hygiene** — the root-dir editor host-test binary can block forever on an anonymous pipe read (no watchdog); it wedges `make host_tests` and leaves an orphan per occurrence (20 found, ~20 h old). Add a runner timeout/stdin redirection, reap orphans, and root-cause the pipe-read hang.

## P2 — performance / quality
8. **Per-TU launch overhead** — 94 MB clang prefetch + ~22k page mappings per launch (~1.3 M mappings per S2 run). Consider cheaper prefetch/segment mapping.
9. **FAT lookup path for header-heavy compiles** — linear directory scans, no cache; first measure (empty vs header-heavy TU) to split compute vs I/O before optimizing.
10. **crt0 positional-arg cap (CRT0_MAX_ARGS 32)** — forced the `OBJS.RSP` response-file workaround for the 66-word kernel link; raise the cap / fix the flat-args path so long argv works natively.
11. **In-OS mtools on 1 GiB images** — `mdeltree` pathologically slow; staging helpers (`tools/w6_lane_stage.sh`) work around; fix or document properly.
12. **sys_exec flat-argv fallback** — still 2×4096-byte stack locals (fine at 64 KiB kernel stacks; revisit if stacks shrink).
13. **Console print-storm rate limiting** — residual verification of the famine-amplifier mitigations.

## P3 — stretch / process
14. **x64 self-host stretch (M5)** — scope and evaluate.
15. **Acceptance RC re-cut** — after P0 items merge: one full loop S0→S3 (incremental + parallel, hopefully fast now) + extract OUT.IMG + boot it + run the in-OS-built program, on the final lineage (T4.3), then the S3/S4/S5 receipts in one record. **Now much cheaper**: incremental + `-j8` landed — a full S2 should drop from ~90 min to minutes.
16. **Merge-back to the workstation repo** `~/Documents/GitHub/HobbyOS` — user-deferred; do as a dedicated, careful step (its `ws/main` has kept moving).
17. **`[NFS] FAIL read PATTERN.BIN` ambient on the CI VM** — network-timing, pre-selfhost history; annotate or stabilize.
