# RUN-REQUEST — lane J1 (ARM LLInt smoke: RLCONF fixture probe)

Updated 2026-10-09 ~05:45 UTC.

## State recap (controller-owned builds)
Both builds GREEN and linked:
- intel-jit `bin/WebProcess` 188,629,944 B @ 04:47, rebuilt incrementally
  @ ~05:19 after the fs-r10-gate commit `37a5044c05` (LLInt + baseline JIT;
  DFG off).  No ninja running.
- arm-jit  `bin/WebProcess` 147,580,392 B @ 05:35 (native LLInt, JIT=OFF;
  includes fs-r10-gate commit `37a5044c05`).

## x64 (KVM) results — J1-owned, DONE this chunk
Runner: `~/hobbyos-perf-lanes/j1/run_j1.sh` (builds disk from perf-j1-os,
stages fixtures + markers, drives via QMP, wall-guarded).  Evidence in
`j1/evidence/*/perf.json` + serial.log.

| probe | script gate | load-ok | doc title | verdict |
|---|---|---|---|---|
| RLCONF (run2, pre-fix bin) | ON but fs-r10 skip still blocked parser | 4817 ms | static title | gated (JS found inert) |
| HOME (run2/3) | ON | 391-576 ms | n/a | green |
| example.com (run1, pre-fix bin) | ON (inert) | 10 838 ms | Example Domain | green |
| en.wikipedia.org (run1, pre-fix bin) | ON (inert) | 229 577 ms | <title> | green |
| RLCONF (run3, fixed bin `37a5044c05`) | ON | **never (deadlock)** | none | gated (findings) |
| RLCONF (run4, fixed bin, /NO-SCRIPT marker) | OFF (opt-out) | 25 524 ms | static title | **green** |

### Key finding (this chunk)
J1 commit `37a5044c05` makes the fs-r10 parser script-skip conditional on
`settings().isScriptEnabled()` — with the driver gate open (no /NO-SCRIPT),
parser page scripts now EXECUTE under native LLInt/baseline JIT (serial shows
`[SCRIPT] execute inline=4323` with NO `[SCRIPT] skip`).  The C_LOOP-era
RLCONF 100%-CPU spin is gone.

BUT the RLCONF page then stalls BEFORE completion: all 8 CPUs idle-stuck
(`[IDLESTUCK]`, watchdog dumps show syscall=76 idle loops, no spin), no
`load-ok`, no `[WIN] gate-probe`, no doc-title eval.  This is the fs-g11 /
parser-blocking-script / readyState=Complete stall class the PJ report
predicted would re-open once page scripts run (PJ-jit-feasibility §4 risk).
Not a crash, not a JIT hang — a parser/loader completion deadlock.

`/NO-SCRIPT` opt-out verified: with the marker staged, `script-enable=0`,
both inline scripts skipped again, RLCONF completes (25.5 s load-ok), rc=0.

### Next for x64
- Diagnose the completion deadlock with scripts on (parser-blocking-script
  path / requestCount==0 && !parsing honesty, per PJ §4 item 4).  This is the
  gate for real page-JS (CNN T-JS) rows.  Suspect scope overlaps C1's
  loader-wedge work — coordinate before burning long CNN runs.
- example.com / wikipedia rows above used the PRE-`37a5044c05` binary
  (scripts inert at parser level).  Re-run them with the fixed binary for the
  honest JS-on numbers once the completion deadlock is understood.

## ARM leg (TCG — controller-owned, long)
Exact command (replaces build-time make with cached OS objects; wall-guarded):

```
export PATH="$HOME/.local/bin:/usr/lib/llvm-21/bin:$PATH"
timeout 1500 ~/hobbyos-perf-lanes/j1/run_j1.sh --arch arm \
    --wp /home/sarah/webkit-lanes/perf-jit/WebKitBuild/HobbyOS-arm-jit/bin/WebProcess \
    --url RLCONF.HTM --run 1 --load-to 600 --wall-to 1200 \
    > ~/hobbyos-perf-lanes/j1/evidence/arm-rlconf-run1.log 2>&1
echo "ARM_RLCONF_EXIT=$?" >> ~/hobbyos-perf-lanes/j1/evidence/arm-rlconf-run1.log
```
Acceptance: `[WIN] script-enable=1`, page script `[SCRIPT] execute` (not
skip), and capture whether the same completion deadlock occurs under ARM TCG.
Do NOT block on load-ok — the x64 finding says completion may stall; capture
the stall class instead of treating it as a pass/fail.

Driver notes: `drive_j1.py` now lowercases typed URL letters (QEMU qcodes are
lowercase; WK5 has upper-case fixture fallback) and only counts load-ok
lines after nav0 (HOME boot line no longer satisfies the nav receipt).
