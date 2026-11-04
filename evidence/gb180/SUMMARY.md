# GB-180 — connect() to a closed AF_UNIX listener never returns

Head: `75028739` (main, GB-046 merge) + branch `fix-gb180` (`638e3c8c`).
Bundle: this directory. Raw serials, not scratch paths.

## Root cause (measured, not inferred)

`sys_connect_addr()` treated every `-2` from `file_connect_user()` as the
"parked, restart on wake" sentinel.  `usock_connect()` returns `-ENOENT` when
`usock_name_lookup()` finds no listener, and **`-ENOENT` is `-2`**.  So a
`connect()` to a closed/missing path name rewound `elr` over the `syscall`
instruction and re-executed the same connect forever; the process never
returned to userspace.  In the 63-program wave this is pid 63 at
`rip=0x100001173F`, `syscall=99`.

Instrumentation showed the retry loop verbatim: `file_connect_user()` returned
`0xFFFFFFFFFFFFFFFE` (`-2`) with `elr=0x100001173F` on a runnable caller, and
the next sample showed `[IDLESTUCK] slot=2 pid=2 st=3 wake=0
rip=0x100001173d IPC_T.BIN` for the rest of the run.

`usock_name_park()` sets the caller `PROC_STATE_BLOCKED` before returning
`-2`, so the two meanings are distinguishable: a caller still runnable with
`-2` got `ENOENT`.

## Fix

`src/kernel/arch/x64/trap.c`, `sys_connect_addr()`: when `r == -2` and the
caller is neither `PROC_STATE_BLOCKED` (parked here) nor `PROC_STATE_READY`
(parked here, already woken), set `regs[0] = -ENOENT` and return instead of
parking/rewinding.  A genuinely parked connect is unchanged.

## Gate run

Manifest (`::WAVE.CFG`): one line `IPC_T.BIN` (the AF_UNIX acceptance suite,
whose `t_named()` ends with `connect()` after the listener closes).

Command:

```
T3_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/gb180_wave1.cfg ::WAVE.CFG' \
  T3_MIN_LINES=1 T3_TIMEOUT=120 bash tools/t3_tier.sh intel
```

(Fails the tier's 55-program floor by construction — one program — so the
tier verdict is not the gate; the serial is.)

### Unfixed (`unfixed-t3_ipc.log`) — the falsifier

Deadline hit at 45 s; no `System halt`, no `ALL TESTS PASSED`, no
`connect after listener close` line:

```
[IDLESTUCK] cpu=0 rounds=20002 t=31096
[IDLESTUCK] slot=2 pid=2 st=3 wake=0 rip=0x100001173d IPC_T.BIN
[IDLESTUCK] claims: c0=-1/2868 ... c7=2/0 lock_owner=1 park_returns=0
```

### Fixed (`fixed-t3_ipc.log`)

```
  IPC_T named: bind(path): PASS
  IPC_T named: connect after listener close -> ENOENT/ECONNREFUSED: PASS
ALL TESTS PASSED SUCCESSFULLY!
System halt from CPU 3.
```

23/23 `IPC_T named:` checks PASS.  No `[LOCKFOREVER]`; no `Vector:` fault
(`Vector: 42` in the log is the virtio-net PCI interrupt line).

## T1

`make host_tests` → `Checks run: 482`, `Checks failed: 0`,
`ALL APPS SUITE TESTS PASSED`, `TEST EXIT: 0`.

## Clause map

1. Probe returns: fixed serial, `connect after listener close -> ENOENT/ECONNREFUSED: PASS`.
2. Name gone after owner gone: same check; `bind(path)` → listener close → fresh connect is refused (unfixed tree hangs on that same call). No lingering name.
3. Normal path: `ALL TESTS PASSED SUCCESSFULLY!` — the full AF_UNIX suite, including the fork/accept/echo path, still passes.
4. No `[LOCKFOREVER]`, no fault line in `fixed-t3_ipc.log`.
