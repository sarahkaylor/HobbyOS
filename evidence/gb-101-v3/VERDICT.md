# GB-101-V3 — verdict: **GREEN** at `a5d2cb49` (PR #240), clause 2 only — with a naming fragility disclosed

Host farm-3, intel (ARCH=intel), 8 CPUs, QEMU TCG. Graded head: PR **#240**, resolved
`a5d2cb49` ("GB-101: add CS-coherence to the TORNFRAME gate"), one file,
`src/kernel/arch/x64/trap.c` +27/−14. Worktree `.worktrees/verify-gb101v3`, evidence
`evidence/gb-101-v3/`. No CI consulted; nothing here is a CI result.

Disclosure: the card gates the clause-2 answer to GB-101 (#179), whose **findings** I wrote. I implemented
nothing on #240 and no part of my evidence branch is in it.

## Clause matrix

| # | Clause | Result |
|---|---|---|
| 1 | detector fires by name on an injected torn frame; quote the tier's line | **PASS** — `[TORNFRAME] FAIL - … vector=131072 rip=0x0000000000000097 cs=0x0000000000000008 …` quoted by the tier in its FAIL list |
| 2 | negative case: no fire on a default wave, exit status as `main` actually is | **PASS** — 0 `TORNFRAME` lines across 23008 console lines / 84 program exits; tier EXIT=1 (1 FATAL + 107 FAIL), i.e. exactly `main`'s state |
| 3 | cannot fire on a legal frame (cs 0x1B user, 0x08 kernel) | **PASS** — bound is `cs != 0x08 && cs != 0x1B`; a full user+kernel wave produced zero fires |
| 4 | releases proc_lock, stops only its own CPU | **PASS** — mid-wave injection: 2 CPUs spun, **86 programs exited**, `System halt from CPU 1`; no `released proc_lock` line because the lock was not held |
| 5 | `make host_tests` green; `obj/*/trap.o` clean on both arches | **PASS** — `Checks run: 482 / Checks failed: 0 / TEST EXIT: 0`, `obj/intel/trap.o` 221640 B, `obj/arm/trap.o` 154416 B |
| 6 | two-arch parity, or say it is x64-only and justify by frame layout | **PASS (disclosed x64-only)** — arm's `struct trap_frame` has no `vector`/`cs`/`ss` at all |

## 1 — the detector fires, and the tier quotes it

Throwaway build at the graded head with the carded frame handed to the dispatcher
(`vector=0x20000 elr=0x97 err=3`, `evidence/gb-101-v3/02-inject.patch`), tier output verbatim
(`03-t3-inject.log`):

```
T3_intel: 414 console lines, 1 FAIL lines, 0 user memory fault records across 0 processes, 0 programs exited, halted=0
T3_intel: FAIL — the console has no 'System halt' line (idle/hung wave, or the 600s deadline hit; a hang is a defect, not a flake)
T3_intel: FAIL — 1 FAIL line(s); first 20:
64:[TORNFRAME] FAIL - interrupt/fault handler read a torn frame: vector=131072 rip=0x0000000000000097 cs=0x0000000000000008 (impossible vector=131072 or invalid cs; a real gate entry pushes cs 0x08/0x1B
T3_intel EXIT=1
```

Named, quoted by the tier, and a different failure from the pre-detector behaviour (which filed 310
`[BADIRET]` lines and a 310-reboot loop as "no System halt line" with zero FAIL lines naming it — GB-101
evidence, comment 14746). Caveat kept: the tier's *headline* is still the hang line; the name is in the
FAIL list.

## 2 — negatives at the graded head, unmodified

```
T3_intel: 23008 console lines, 107 FAIL lines, 17 user memory fault records across 14 processes, 84 programs exited, halted=1
T3_intel: FAIL — 1 FATAL kernel-mode fault line(s): the guest halted because the kernel crashed, not because the wave completed
T3_intel EXIT=1
```
```
grep -c TORNFRAME evidence/gb-101-v3/01-t3-clean.serial.log → 0
```

Clause 2 asks for exactly this: no fire on legal traffic, and the tier's exit is whatever `main` produces
— which is still red for clause 1's reasons. Not glossed over here: this run also contained one shredded
fatal line (`FATAL kernel-mode fault, halting this CPU:/ FvOeLcDtToArB=.5T2X8T0`) that the detector did
**not** flag. That is correct if the frame was coherent and only the printout raced, and it is
unprovable from the log — so I am not claiming the detector covers print-shredding. It does not; that is
GB-121's (#265) territory.

## 3 — the bound is real

```c
uint64_t frame_cs = tf->cs;
int bad_vec = ((uint64_t)tf->vector > 0xFFu);
int bad_cs  = (frame_cs != 0x08u && frame_cs != 0x1Bu);
if (bad_vec || bad_cs) { … }
```
The IDT has 256 gates, so `> 0xFF` cannot be a real vector; `cs` is authoritative on x86-64 because the CPU
always pushes it (SS/RSP only on a privilege change — the comment gets that right). The clean run
exercised both legal shapes at volume (84 ELF starts at `cs=0x1B`, thousands of kernel traps at `cs=0x08`)
with zero fires.

## 4 — one CPU stops, the rest carry the wave

Mid-wave injection (1500 dispatcher entries in, `evidence/gb-101-v3/10-inject-1500.patch`) — the run
completed:

```
T3_intel: 26895 console lines, 129 FAIL lines, 18 user memory fault records across 15 processes, 86 programs exited, halted=1
System halt from CPU 1.
grep -c "released proc_lock" → 0
```

Two CPUs took the poisoned frame and spun in the handler; 86 programs still exited and the wave halted.
No `proc_lock` release line because no CPU held the lock at that point — the release path exists
(`spinlock_release_if_held_by_me`) but was correctly not needed.

## The fragility this run found (disclosed, not hidden)

Under mid-wave load the detector's own line is **shredded** by concurrent console writes and never reaches
the log intact. From `11-t3-inject-1500.serial.log`, the detector's output as it actually landed:

```
dler read a torn frame: vector=131072 rip=0x000000v20 0l000oad0e0r0:9 PI7D c=2s3=0 ex0nt00ry00=000x010000000000001B0 00 s(pi=0mpx1os8s0i0bl00e 0v0 erectsidoern=t=11371 0t72a bolr eisnv=6a
```

`grep -c "TORNFRAME"` on that 26895-line serial returns **0**, and the tier reported 129 FAIL lines none of
which is the detector's. So the naming guarantee clause 1 asks for is real on a quiet console (boot-time
injection) and fragile exactly when frames get torn in earnest — the same lock-free-sink defect that made
GB-101's fatals unreadable. Concretely: GB-121 (#265) must cover `[TORNFRAME] FAIL` lines, or this
detector will be undetectable in the conditions it exists for.

## 5, 6 — build and parity

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0            HOST_RC=0
obj/intel/trap.o 221640
obj/arm/trap.o   154416        ARM_RC=0
```

Parity, disclosed as the card allows: the check is **x64-only**, because arm's frame carries no such
fields —

```c
// src/include/arch/trap.h:24-29 (non-x86_64 branch)
struct trap_frame {
  uint64_t regs[30];  /* x0..x29 */
  uint64_t lr;        /* x30 */
  uint64_t elr;       /* PC */
  uint64_t spsr;      /* PSTATE */
};
```

There is no `vector`, no `cs`, and no `ss` to test on arm; the equivalent torn-frame signal there is
`SPSR` mode bits (EL0t vs EL1t/EL1h) plus an ESR-derived vector, which this PR does not implement. Arm
still compiles and links. If the Coordinator wants claw-parity, that is a separate S-card, not a defect in
this one.

## What this GREEN says

At `a5d2cb49`, GB-101 clause 2 is answerable and answered: an injected torn frame is named by the
detector and that name reaches the tier, the detector stays silent on legal frames, it stops one CPU
without wedging the others, and both arches still build with host tests green.

It does **not** say GB-101 is done: clause 1 is RED at `4626084` (comment 14746) and this card does not
touch it. It also does not say the naming survives concurrency — see the fragility above.

## Reproduce

```
cd .worktrees/verify-gb101v3     # a5d2cb49
make disk.img ARCH=intel -j32 && T3_MONITOR=1 T3_TIMEOUT=600 W0_GATE=0 bash tools/t3_tier.sh intel   # 01: 0 TORNFRAME
git apply evidence/gb-101-v3/02-inject.patch && make disk.img ARCH=intel -j32 \
  && T3_MONITOR=1 T3_TIMEOUT=600 W0_GATE=0 bash tools/t3_tier.sh intel                              # 03: names it
git checkout -- src/kernel/arch/x64/trap.c
make host_tests                                                                                     # 482/0
make disk.img ARCH=arm -j32                                                                         # arm trap.o
```