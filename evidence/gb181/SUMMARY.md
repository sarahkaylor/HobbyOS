# GB-181 — ARM parity: connect() -ENOENT aliases the -2 park sentinel

Base: `fix-gb180` @ `28c87af6` (x64 discrimination already in the tree so clause 3 is
gradable). Branch: `fix-gb181`. Bundle: this directory.

## Root cause

Same alias as GB-180: `usock_connect()` returns `-ENOENT` (2) for a missing AF_UNIX
path, and `sys_connect_addr()` treats every `-2` from `file_connect_user()` as the
park/restart sentinel.  On ARM the handler had no state discrimination at all:

```c
int r = file_connect_user(...);
if (r == -2) {
  tf->elr -= 4; /* restart when woken (pipe park model) */
  schedule(tf, 0);
}
```

so a missing name rewinds `elr` by 4 and re-executes the same `connect()` forever.

## Fix

`src/kernel/arch/arm/trap.c`, `sys_connect_addr()`: when `r == -2` and the caller is
neither `PROC_STATE_BLOCKED` nor `PROC_STATE_READY` (both mean the `-2` came from
`usock_name_park()`), set `regs[0] = -ENOENT` and return; a genuinely parked connect
still rewinds and schedules.  Same shape as the x64 fix in GB-180.

## Clause 1 — ARM returns the errno

Probe: one `::WAVE.CFG` line `IPC_T.BIN` (its `t_named()` ends with `connect()` after
the listener closes).

```
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/gb180_wave1.cfg ::WAVE.CFG' \
  T3_MIN_LINES=1 T3_TIMEOUT=600 bash tools/t3_tier.sh arm
```

Unfixed (`unfixed-arm-ipc.log`, 150 s deadline): no halt, no final check, repeating

```
[IDLESTUCK] slot=2 pid=2 st=3 wake=0 rip=0x100000dca0 IPC_T.BIN
```

Fixed (`fixed-arm-ipc.log`):

```
  IPC_T named: connect after listener close -> ENOENT/ECONNREFUSED: PASS
ALL TESTS PASSED SUCCESSFULLY!
System halt from CPU 7.
```

## Clause 2 — ARM default wave reaches its end state

Command (empty `::WAVE.CFG` so the 97-program literal wave runs):

```
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/gb181_empty.cfg ::WAVE.CFG' \
  T3_SMP=1 T3_MONITOR=1 T3_TIMEOUT=2400 bash tools/t3_tier.sh arm
```

`arm-default-wave.log`:

- `[EXITPC]` lines: **554** (floor 55)
- `System halt.`
- `[WATCHDOG]` lines: **0**
- `[LOCKCNT] ... LOCKFOREVER=0x0000000000000000`
- no `Vector:` fault line

The tier wrapper exits 1 only on its follow-on `t4_w0` corpus gate
(`t4_w0: FAIL — W0 corpus incomplete or output mismatch`), a pre-existing ARM corpus
baseline, not the connect stall; the wave itself has 0 `FAIL` lines and a clean halt.

## Clause 3 — x64 unchanged

`x64-probe.log` (same one-program manifest, `ARCH=intel`):

```
  IPC_T named: connect after listener close -> ENOENT/ECONNREFUSED: PASS
ALL TESTS PASSED SUCCESSFULLY!
System halt.
```

## Clause 4 — no new fault

Both serials: no `[LOCKFOREVER]`, no `Vector:` fault.  T1 `make host_tests`:
`Checks run: 482`, `Checks failed: 0`, `TEST EXIT: 0`.
