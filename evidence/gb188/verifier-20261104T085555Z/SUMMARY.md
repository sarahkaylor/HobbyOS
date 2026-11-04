# GB-188-V — independent acceptance run, swarm-farm-1, 2026-11-04

Head graded: `4cc60a4a` (PR #354; runner fixes at `934cea16`, kernel under test `main 7c8f16a6`).
Worktree `/home/sarah/.swarm/wt-gb188v` (branch `verify-gb188v`).

## Commands

```
make disk.img DESKTOP=hobby ARCH=intel          # rc=0
unset QEMU_SMP
DESKTOP=hobby python3 tools/t5/acceptance_run.py --arch intel
```

The runner's QEMU command in the captured serial is `-smp 8` (Makefile default, no
`QEMU_SMP=1` passed): `qemu-system-x86_64 -M q35,vmport=on -smp 8 ...`.

## Results

- `[MENU] count=186`
- `grep -c "FATAL kernel-mode fault" serial.log` = **0**
- `grep -c FATAL serial.log` = **0**
- `grep -c "Vector: 14"` = **0**; `grep -c "System halt"` = **0**
- Loads: `[LAUNCH]` + `[ARRIVED] ... rc>0` for PONG (slot 16), MILLIPED (17),
  XNIBBLER (20), XCRYPT (21), NES (22)
- Table: `loaded=yes`, `live frame=True`, PASS 5/5; `Summary: **PASS**`, rc=0

## Falsifier (quoted from GB-053's PR #314 record, not re-run)

```
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x14
  RIP: 0x00000010000043B0  CS: 0x1B  CPU: 5  pid: -1
[DEADCLAIM] slot=1 pid=1 name=DESKTOP.BIN claimed only by halted cpus
System halt from CPU 1
```

with `cr2 == rip == 0x10000043B0` and `pid=-1`: the intel default-8-CPU leg was
4/4 pre-menu halts on the PR #314 base. The current head reaches the menu at the
same setting.

## Verdict

`VERDICT: GREEN sha=4cc60a4a`.
