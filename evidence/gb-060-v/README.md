# GB-060-V — gate evidence (VERDICT: RED, T3 gate 2 of 2)

Ticket: GB-060-V (#170) — Verify x64 context-switch durability (proc_lock must not strand)
Verifies: GB-060 (#62), shipped on `main` @ `9db1388` (the sha this verdict names; no PR — kernel on main)
Verifier: farm Verifier, DEEP (DeepSeek v4.1 Flash, dgx1), host `swarm-farm-2`, own worktree
Run date: 2026-10-25 UTC (~07:30–08:03Z). Independence: I authored neither GB-060 nor `locks.c`.

## Gate 1 — T1 both shapes: PASS

```
$ make locksteal_test_host && ./locksteal_test_host        # standalone (card's alternative form)
locksteal_test_host: 27 checks, 0 failed
RUN_EXIT=0
```
Full list in `01_locksteal_standalone.log`. Both named shapes and the hard guard are present and pass:
`PASS: LIVE owner (cpu1): never stolen -- this is the safety property`,
`PASS: halted owner (cpu1): freed`,
`PASS: LIVE holder with a fresh heartbeat: never broken, however long the waiter spins`,
`PASS: at the grace period: broken (>=, the same rule the lost-owner reaper uses)`.

In-suite (`make host_tests`, the card's first form):
```
$ make host_tests
...
locksteal_test_host: GB-060 stale-owner steal + wedged-owner stall break
  ... 27 PASS lines ...
locksteal_test_host: 27 checks, 0 failed
...
ALL APPS SUITE TESTS PASSED
HOST_TESTS_EXIT=0
```
(`03_host_tests.log`, 5 237 lines, exit 0 — the rtti/libc++ build break seen on this VM earlier is gone
now that `third_party/llvm-21.1.8/dist-staging` is staged, so the whole suite built.)
Nit, not a failure: the binary prints `27 checks, 0 failed`, not the card's literal
`27 checks, 0 failed, running in-suite` — the suffix does not exist in `src/host/locksteal_test.c`.

## Gate 1's can-fail proof (honesty law): PASS

`mutant_check.py` copies `src/kernel/arch/x64/locks.c`, rewrites one line so
`lock_stallbreak_ok()` breaks a LIVE holder regardless of heartbeat, and runs the real test against it
(the tree is untouched, the mutant is built in `/tmp`):

```
FAIL: LIVE holder with a fresh heartbeat: never broken, however long the waiter spins
FAIL: one ms inside the grace period: still protected
locksteal_test_host: 27 checks, 2 failed
[mutant] MUTANT_EXIT=1  (expected non-zero: the checks must fail)
```
`02_mutant_control.log`. The test binds to the implementation, not to itself.

## Gate 2 — T3 intel full wave: **FAIL**

```
$ T3_LOG=obj/t3_intel_gb060v.log bash tools/t3_tier.sh intel
T3_intel: 23335 console lines, 165 FAIL lines, 11 user memory fault records across 8 processes,
          85 programs exited, halted=1
T3_intel EXIT=1
```
The 165 FAIL lines are `main`'s known intel family (GB-093 SEDTEST, GB-095 TSORTTEST, FPU_T,
DIRECT-ext-cat) and are **not** this card's criterion. The card's criterion is the lock-strand counts,
and one of them is not zero:

```
$ grep -ac '[STALLBREAK]' obj/t3_intel_gb060v.log   ->  1
$ grep -ac 'HOLDER'       ...                       ->  0
$ grep -ac 'LOCKFOREVER'  ...                       ->  0
$ grep -ac 'STOLEN'       ...                       ->  0
$ grep -ac 'IDLESTUCK'    ...                       ->  464
```
The event, verbatim (`obj/t3_intel_gb060v.log:6769`, 29 % into the wave):

```
[STALLBREAK] cpu=6 lock=0x0000000070200248 owner_cpu=2 stale_ms=2054 pc=0x0000000070000603
```

Card text, quoted: *"Expect `halted=1` and the wave reports zero lock-strand events:
`STALLBREAK = 0 HOLDER = 0 LOCKFOREVER = 0 STOLEN = 0`. A nonzero STALLBREAK/STOLED count, or a wave
that hangs at a lock (LOCKFOREVER > 0), is a FAIL."* → **nonzero STALLBREAK = FAIL.**

What the line means, exactly: CPU 2 held lock `0x70200248` and stopped taking timer interrupts for
**2 054 ms** (`LOSTWAKE_DEAD_OWNER_MS` is 2 000 ms in `src/include/process.h:572`), so CPU 6 broke it
one tick after it became eligible. Pre-GB-060 that is the freeze; post-GB-060 it is a recovered break —
the mechanism works, but the trigger fired, and the trigger is the class the card exists to keep at zero.

Host starvation is excluded as the explanation: the run is KVM (`qemu-system-x86_64 … -enable-kvm`,
`/dev/kvm` present), host has 32 CPUs at load ~2.8 with 8 vCPUs, so this is not the MTTCG case that
`src/kernel/process.c:3100-3112` documents ("a genuinely-running claimer's vCPU can be host-starved …
under MTTCG … single-sampling DISPOSEd a live loader"). Under KVM the guest really did not run CPU 2
for two seconds.

## Reproducibility (incomplete, stated as such)

A second identical run was started to see whether the break is deterministic:
`06_t3_intel_run2.log` / `obj/t3_intel_gb060v_run2.log`. At 08:03Z it had reached **12 083 lines with
0 `[STALLBREAK]`** but was running ~5× slower than run 1 (140 lines/min vs ~3 300), with **22**
`[IDLESTUCK]` dumps against run 1's 8 — the stall family is present in that run too, without (yet)
tripping the 2 s grace. It had not halted when this verdict was written. So: **1 break in 1 completed
wave**; the second wave is inconclusive, not a clean control.

## Notes for the owner

- The worker's evidence quoted a summary block (`STALLBREAK = 0 HOLDER = 0 …`) that the kernel does not
  print; there is no such line in a ticket log. Counts have to come from grepping the actual marker
  lines, which is what this verdict does. Recommend the wave print a real end-of-run counter block so
  the number is machine-checkable instead of hand-derived.
- `stale_ms=2054` is a 2-second IRQ-off stretch on a vCPU that KVM was running — worth asking whether
  the 2 000 ms grace is simply tight relative to this tree's longest legitimate IRQs-off section
  (disk/TORTURE/FPU phase), because if it is, the stallbreak is now firing on *slow* rather than
  *wedged* holders and the counter will never sit at 0 on a clean wave.
