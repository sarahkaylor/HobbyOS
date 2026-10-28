# GB-100 — DEEP findings: "enter_user_space returns" is not a code path; a resume that
cannot enter its target dies as a **fatal kernel fault whose frame is torn**

Host: farm-3 VM, intel (ARCH=intel), QEMU TCG, `T3_MONITOR=1`. Head: `4626084` (`origin/main`,
2026-10-28). Worktree `.worktrees/gb100`. Prior evidence read, not re-derived: `b450584` on
`hermes/gb100` (0 resume-returns, 5 fatal frames @ `527670f`).

## 1 — the route cannot happen by executing code

`enter_user_space` is 341 bytes and contains **zero RET instructions and exactly one IRETQ**, which is
its last instruction. Measured on the artifact this VM just built (`hobbyos.elf`, extracted from
`.text` and disassembled as `i386:x86-64`, script `evidence/gb-100/disasm.py`):

```
.text addr=0x70000000 off=0x1000 size=0x45af5
enter_user_space 0x7003d88b .. next symbol 0x7003d9e0
bytes: 341
RET instructions: 0
IRET instructions: 1
   149:	48 cf               	iretq
```

So `enter_user_space` cannot *fall* back to its caller: the only exit it has is the IRETQ. A `[KERNEL]
resume call RETURNED` line therefore means the **caller's return address was popped by something other
than a `ret` in this function** — i.e. the kernel stack the call sits on was written through, which is
exactly the GB-101 class (one torn frame describing three code moments). That also explains why the
probe `resume_returned_halt()` never fires on current main: it reports the symptom, and the symptom now
manifests as a crash before the call can "return".

## 2 — a resume that cannot enter its target is fatal, not recovered

Falsifier: poison the user-mode resume destination so the frame copy cannot land — one line inserted into
`enter_user_space`'s user path in `src/kernel/arch/x64/trap.c` (throwaway build, patch kept at
`evidence/gb-100/02-poison.patch`, **not for commit**):

```
+    mov r13, 0x40000000          /* 0x3FFFFEC0 is unmapped */
     lea rdi, [r13 - 320]
```

A/B on the same head, same VM, same tier options (`T3_MONITOR=1 T3_TIMEOUT=900 W0_GATE=0`):

| run | console lines | user mem-fault records | programs exited | halted | FATAL kernel frames | tier |
|---|---|---|---|---|---|---|
| baseline (main, unmodified) | 10883 | 6 across 6 procs | 85 | 1 | **0** | EXIT=1 (83 harness FAIL lines — GB-077's terrain) |
| poisoned resume destination | 22362 | 17 across 16 procs | 87 | 1 | **2** | EXIT=1 (137 FAIL lines + "the guest halted because the kernel crashed, not because the wave completed") |

The only difference between the two builds is the poison line. Verbatim, the poisoned run's verdict and
frames:

```
T3_intel: 22362 console lines, 137 FAIL lines, 17 user memory fault records across 16 processes, 87 programs exited, halted=1
T3_intel: FAIL — 2 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
```
```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 19 Error Code: 0x0000001000010848
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=14 rip=0x0000000070037698 cr2=0x0000000375503820 pid=62 err=0 cr3=0x00000000402F6000 pd480=0x00000000810000FB
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 6 Error Code: 0x0000000000000000
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000000000007 cr2=0x000000100004A18B pid=43 err=0 cr3=0x0000000075513000 pd480=0x00000000810000FB
```

## 3 — the crash cannot be attributed, because the frame is torn

Two things in that quote are load-bearing and neither is what a fault report should look like:

- `vector=6 rip=0x0000000000000007` — a #UD at RIP 7, and the console on that line is interleaved with
  another CPU's writes (`[0K0E0RN0E0L0]0 0F0A00T0AL …`, `RIP: 0x0000000000000000 CS: 0x0 SS: 0x0` in the
  raw serial). Both are the GB-101 signature: the frame printed is not the frame that faulted.
- the poison's own address (`0x3FFFFEC0`) never appears in any fault line — the run dies two steps
  downstream of the poison, in a way the serial cannot localise.

That is the honest boundary of this run: the A/B proves the poisoned resume **turns into fatal faults**
(0 → 2, same head, nothing else changed), and the evidence says the *reporting* of those faults is the
reason the original "it returned" inference could not be nailed down.

## What this establishes, and what it does not

Established:
1. Returning out of `enter_user_space` is impossible as a code path (0 RET, 1 IRETQ, measured on the
   built artifact) — so any observed return is stack write-through, not control flow.
2. A resume whose target cannot be entered is **fatal today**: it kills the CPU and the wave, and the
   fault that kills it is reported with a torn frame.
3. On current main, with the probe in place, a full intel wave (85 program exits) produced **zero**
   `resume call RETURNED` lines and **zero** FATAL frames — the probe is not being skipped; the route is
   not being taken in that run.

Not established (and not claimed):
- A minimal reproduction of the *original* "twice in one intel run, on two CPUs" observation. Eight CPUs
  at this head did not produce it in 85 exits, so it is a rare interleaving, not a steady state.
- Any fix. Naming the mechanism is as far as the measurement goes; making a resume that cannot enter its
  target *recoverable* is a design change (GB-060's halt-record is the current answer, and it costs the
  CPU and the guest), and it should not be written until the torn frame (GB-101, #179) can report the
  real site — otherwise the next fix is graded against evidence that cannot show whether it worked.

Recommended order: GB-101 (#179) lands → re-run this exact A/B (the patch is reproducible from
`02-poison.patch`) → only then decide between "refuse the resume and keep the CPU" and "make the copy
provably unable to fault".

## Reproduce

```
cd .worktrees/gb100                      # head 4626084
make disk.img ARCH=intel -j32            # evidence/gb-100/00-build-intel.log
T3_MONITOR=1 T3_TIMEOUT=900 W0_GATE=0 bash tools/t3_tier.sh intel   # 01-t3-intel-monitor.log
git apply evidence/gb-100/02-poison.patch
make disk.img ARCH=intel -j32 && T3_MONITOR=1 T3_TIMEOUT=900 W0_GATE=0 bash tools/t3_tier.sh intel  # 03, 05
python3 evidence/gb-100/disasm.py                                                              # 04
```