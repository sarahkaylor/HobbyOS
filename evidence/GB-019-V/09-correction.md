# GB-019-V — correction: the probe is dispatched; the harness chain is what fails

Revision **46f19bb**, worktree `hermes-9642df51`, intel T3, swarm-farm-2.

My first verdict on this ticket said "the probe never arrives because a staged program is never
dispatched". That was right about the symptom and **wrong about the cause**. Three runs, each adding
one fact, pin the real chain:

## Run 1 — stage only (the ticket's recipe)

```
- 61008 T4PROBE.BIN
t4-probe-arrived: 0            # t4_device.sh: FAIL, not SKIP
```

Cause: GB-072 is **Done**, and its mechanism is a manifest — `src/kernel/main.c` dispatches
`WAVE.CFG` when that file exists and falls back to the hardcoded list when it does not. Staging an
ELF without staging `WAVE.CFG` cannot dispatch anything. The four `-V` recipes predate that.

## Run 2 — manifest + the Makefile's probe build (ET_EXEC)

```
[WAVE-MANIFEST] T4PROBE.BIN
elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
[ARRIVED] T4PROBE.BIN rc<0
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 13 Error Code: 0x0
  RIP: 0x0000000000000000
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0 cr2=0x0 pid=1 err=0
```

Two defects here, both real:

1. **The Makefile builds the probe wrong for this loader.** `$(T4_PROBE_ELF)` is
   `musl-gcc -static`, which produces `ET_EXEC` below the user window; this loader requires
   static-PIE (`docs/elf-loader.md §1`). `make obj/t4_probe.elf` can therefore never produce a
   loadable probe.
2. **A rejected program crashes the kernel.** `[ARRIVED] rc<0` is the graceful path, but the guest
   then takes a #GP in kernel mode at **RIP 0x0** on pid 1 — calling a null function pointer on the
   failure path. A bad user ELF must not halt a CPU.

## Run 3 — manifest + a static-PIE build (this is the one that got furthest)

```
$ musl-gcc -static-pie -O2 -Wall src/user/t4_probe.c -o /tmp/t4_probe_pie.elf
ELF 64-bit LSB pie executable, static-pie linked            (exit 0)
$ /tmp/t4_probe_pie.elf > legA-pie.out                      (exit 0, 43 lines)
$ diff -q obj/intel/t4_legA.out legA-pie.out  ->  identical
```

So leg A is unaffected by the PIE change (same rows on real Linux). In the guest:

```
[WAVE-MANIFEST] T4PROBE.BIN
[WAVE-TRY] T4PROBE.BIN
v2 loader: PID=2 entry=0x1000001C18 sp=0x1800000000 resident=31 tables=6
[ARRIVED] T4PROBE.BIN rc>0
[KERNEL] User process fault! Vector: 13 RIP: 0x1000003E6C
```

The probe now **loads and arrives**, then takes a **#GP in user mode at RIP 0x1000003E6C** before
printing a single `row=` line. `row_lines=0`, and the guest ends in the same kernel-mode `rip=0x0`
halt.

## What this means for the verdict

**RED stands** — DoD clause 2 cannot be evaluated, there are zero rows of leg B — but the blocker is
not "never dispatched". It is a three-layer harness failure:

| layer | fact |
|---|---|
| dispatch | works, via `WAVE.CFG` (GB-072). The `-V` recipes are stale: they stage the ELF and no manifest. |
| loader/build | `make obj/t4_probe.elf` emits ET_EXEC, which this loader refuses; static-PIE is required. |
| guest execution | even static-PIE faults at `0x1000003E6C` (user #GP) before row 1 — and the failure path then #GPs in the kernel at `rip=0x0`. |

Until the probe runs past its first row, no `-V` ticket in this family (GB-019-V, GB-021-V, and the
rest of the `t4_device.sh` set) can produce a verdict that means anything about the ABI rows.

Evidence in this tree: `06-t3-manifest.log`/`06b-serial.log` (run 2), `08-t3-pie.log`/`08b-serial.log`
(run 3), `07-legA-pie.out`.
