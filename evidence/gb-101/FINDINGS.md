# GB-101 — DEEP findings: **both DoD clauses are RED on current main**, and the two defects are named

Host farm-3, intel (ARCH=intel), 8 CPUs, QEMU TCG, `T3_MONITOR=1`, head `4626084` (`origin/main`).
Worktree `.worktrees/gb100`, evidence `evidence/gb-101/`.

DoD, taken literally as two measurements:
> no impossible-vector fatal in 3 consecutive intel T3 runs, and an injected torn frame FAILS the tier by
> name rather than halting a CPU silently

| clause | result | why |
|---|---|---|
| 1 — no impossible-vector fatal in 3 consecutive runs | **RED** | run 1 of 3 reproduced the carded frame exactly: `vector=20000 rip=0x0000000000000097` |
| 2 — injected torn frame fails the tier *by name* | **RED** | the kernel names it 310 times; the tier reports `FAIL — the console has no 'System halt' line` and never mentions it |

## 1 — the carded fault is alive on main, and the report is usually unreadable

Three consecutive runs, nothing changed between them (`evidence/gb-101/run{1,2,3}.log` + `.serial.log`):

| run | console lines | programs exited | `FATAL kernel-mode fault, halting this CPU: vector=…` lines | `FATAL: Exception in Kernel Mode!` banners | tier verdict |
|---|---|---|---|---|---|
| 1 | 57922 | 87 | **2** — including `vector=20000 rip=0x0000000000000097` | 13 | FAIL — 2 FATAL + 350 FAIL lines |
| 2 | 11280 | 81 | **0** | 18 | FAIL — 79 FAIL lines (no FATAL reported) |
| 3 | 11763 | 81 | **0** | 10 | FAIL — 83 FAIL lines (no FATAL reported) |

Run 1, verbatim:

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000000FFE6F8 cr2=… pid=… err=…
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=20000 rip=0x0000000000000097 cr2=… pid=… err=…
```

`vector=20000` and `rip=0x97` are, byte for byte, what the card records. So clause 1 cannot be claimed:
it happened on the first of three runs.

Runs 2 and 3 are the more useful result. The guest took 18 and 10 kernel-mode exception banners and
produced **zero** readable `vector/rip/cr2` lines — the report itself is torn, because the fatal printer
writes multi-line text through the **lock-free** raw sink (`uart_puts_raw`, deliberately lock-free so the
fault reporter cannot deadlock the console it is reporting on) while other CPUs are still printing. Real
sample from run 1's serial:

```
[KERNEL] FATAL kernel-m  oPdAeS Sf caulltose(, 3h1)al ti[nngvheirs o CpPenU:ed ]v -ect> oErB=A2D0F00
```

That is `FATAL kernel-mode fault, halting this CPU: vector=…` interleaved with `[PASS] close(31) [never
opened] -> EBADF` from another core. The tier's FATAL counter needs an *intact* single line, so 28 banners
across runs 2-3 produced **no FATAL count at all** — the CPU halts and the tier says nothing about why.

## 2 — an injected torn frame names itself in the kernel and is invisible to the tier

Injection (throwaway build, `evidence/gb-101/inject.patch`, **not for commit**): at the top of
`general_interrupt_inner()` the dispatcher is handed the carded frame once per boot —

```c
tf->vector = 0x20000;   /* not a vector the CPU can push */
tf->elr    = 0x97;      /* below the NULL page */
tf->error_code = 3;
```

Result (`run4-inject.log`, `run4-inject.serial.log`):

```
[GB101-INJECT] torn frame -> dispatcher: vector=131072 rip=0x97 err=3
[BADIRET] about to return to an implausible user RIP=0x0000000000000097 cs=0x0000000000000008 rsp=0x00000000755FFDD8 eflags=0x0000000000000202 cpu=0x0000000000000000
```

- **310** `[BADIRET]` lines — GB-057's frame check does its job and names the impossible RIP/CS every time.
- **310** guest reboots in 600 s (the serial carries 310 `Booting AArch64 OS` banners after the injection):
  the torn frame puts the machine in a reset loop, ~2 per second.
- **0** `FATAL` lines.
- Tier verdict: `T3_intel: 18337 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes, 0 programs exited, halted=0` then
  `T3_intel: FAIL — the console has no 'System halt' line (idle/hung wave, or the 600s deadline hit; a hang is a defect, not a flake)`.

So clause 2 is RED in the strongest form: the kernel names the torn frame 310 times, the guest never
executes a program, and the tier files it as a **hang** without ever mentioning `BADIRET`, the vector, or
the reboot loop. "Halting a CPU silently" is exactly what the tier does here — not because the kernel is
silent, but because the tier has no pattern for the one message that names it.

## The two defects, named

1. **Torn fault report (kernel).** The fatal path writes ~8 lines through the lock-free sink while other
   CPUs keep printing; 28 of 30 banners across three runs lost their `vector/rip/cr2` body. Any diagnosis
   that depends on reading a fatal frame is unreliable on this build — my own GB-100 A/B hit the same
   wall (`vector=6 rip=0x7`, interleaved).
2. **The tier cannot name this failure (tooling).** It has no detector for `[BADIRET]` and no
   reboot-loop detector, and its FATAL counter only matches intact lines. A torn frame that trips GB-057
   and resets the machine reads as "no System halt line".

Concrete, small fixes (for whoever picks this up — I did not implement them, so nothing here is a claim
that they work):
- tier: count `FATAL: Exception in Kernel Mode!` banners as FATAL even when the detail line is torn; add a
  `[BADIRET]` pattern that names RIP/CS; count boot banners and fail by name on a reset loop.
- kernel: render the fatal report into a per-CPU buffer and flush it with one locked write (or a try-lock
  with a single-line compact fallback); validate `tf->vector` against `0..31` / `0x80..0x8F` before
  dispatch and name `IMPOSSIBLE VECTOR` explicitly.

## What is established, what is not

Established, with quotes: clause 1 fails (run 1 of 3), clause 2 fails (tier never names it), the
impossible frame is reproducible on demand by injection, and the reason it is usually invisible is a torn
report plus a tier with no pattern for it.

Not established: which code path *produces* the torn frame in a clean run (its report is torn, so the
producing site cannot be read from the serial — that is the defect, not a gap in this measurement).

## Reproduce

```
cd .worktrees/gb100 && git log --oneline -1          # 4626084
bash evidence/gb-101/three_runs.sh                   # 3 runs, ~6 min each
python3 evidence/gb-101/tally.py                     # intact-FATAL / impossible-vector tally
git apply evidence/gb-101/inject.patch && make disk.img ARCH=intel -j32
T3_MONITOR=1 T3_TIMEOUT=600 W0_GATE=0 bash tools/t3_tier.sh intel   # 310 BADIRET, 0 FATAL, "no System halt"
```