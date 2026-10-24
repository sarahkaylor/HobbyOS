# GB-086-V — gate run, revision ad51ac5

Worktree `hermes-9642df51`, branch `verify-gb086v` at **ad51ac5** (= `origin/ticket/086-polltst`, the
PR head the card names; not an ancestor of `origin/main`), swarm-farm-2.
ARM tier on this box: `qemu-system-aarch64` + `AAVMF_CODE.fd` present.

## DoD 1 + 2 — elapsed_ms printed and the check passes, at both CPU counts (PASS)

```
$ T3_SMP=1 bash tools/t3_tier.sh arm
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): PASS
  POLLTST elapsed_ms=2542

$ T3_SMP=8 bash tools/t3_tier.sh arm
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): PASS
  POLLTST elapsed_ms=408
```

16/16 POLLTST checks PASS in the 1-CPU run. The measured distribution is quoted rather than hidden:
2542 ms at 1 CPU, 408 ms at 8 — both inside the existing 85..5000 ms window, so the bound was **not**
widened. (2542 ms for a 100 ms select is itself worth a look, but it passes the DoD as written.)

The tier exits 1 on both runs for reasons outside this row set — 1 CPU: `SHELLTEST WATCHDOG: protocol
stalled 25 s`; 8 CPU: the control-run abort described below. Disclosed, not folded into the verdict.

## DoD 3 — the broken-select control (HALF MET: fails at 8 CPUs, never runs at 1)

8 CPUs — the control does what the DoD asks:

```
[WAVE-MANIFEST] POLLTSTBRK.BIN
[ARRIVED] POLLTSTBRK.BIN rc>0
  POLLTST timeout took >= 85ms and < 5000ms (elapsed seen): FAIL
  POLLTST elapsed_ms=0
```

1 CPU — the control produces **no row at all**, and the guest halts:

```
v2 loader: PID=2 entry=0x1000000000 sp=0x1800000000 resident=1 tables=3
[ARRIVED] POLLTSTBRK.BIN rc>0
[KERNEL] FATAL: CPU 0x0 Synchronous Exception in EL1! ESR: 0x8600000E EC: 0x21
  ISS: 0xE, FAR: 0x0, ELR: 0x0, PROC pid=1
```

`grep -c "POLLTST timeout\|elapsed_ms" <1-CPU control serial>` → **0**.

So `POLLTSTBRK.BIN` aborts to address 0 (instruction abort, EC=0x21, ELR/FAR = 0) before it prints its
check on a 1-CPU wave, and takes the CPU down with it. The 8-CPU run hits the *same* abort
(`ELR: 0x0`, same ESR) — but only after the FAIL line is out.

DoD 3 says the control must FAIL **at both counts**. At 1 CPU it cannot fail a check it never reaches.

## DoD 4 — T4 differential (PASS)

```
$ make ARCH=intel t4_compat_parity
t4 host differential: A=0 B=0 cases=3
t4_parity: PASS (host differential locked; device leg gated)
```

The two named rows, run on both legs directly:

```
$ /tmp/legA.bin   (musl -static, real Linux)
select_timeout_rc rc=1
select_timeout_fdset rc=1
$ /tmp/legB.bin   (clang -DLEG_B_SHIMS, HobbyOS compat surface)
select_timeout_rc rc=1
select_timeout_fdset rc=1
```

Byte-identical. (This is the host differential; the in-QEMU device leg is still gated and reports
SKIP, as the harness says in its own output.)

## Verdict

**RED** — DoD 3 is only half satisfied. The instrument must be able to fail *at both counts*; at 1 CPU
the negative control dies before it reaches the check, so this ticket cannot be called GREEN without
painting over that.

The other three clauses hold and are quoted above, so the RED is scoped to the control's 1-CPU
execution, not to the elapsed_ms emitter or the parity rows.

**Two things the Coordinator should see:**

1. `POLLTSTBRK.BIN` aborts to `ELR=0x0` on ARM (EC=0x21, instruction abort) — the same
   branch-to-null signature the intel loader showed on a rejected ELF (`rip=0x0`). Two architectures,
   same failure shape, which makes it look like one defect in the load/exit path rather than two.
2. The 1-CPU control needs to reach its check; until it does, GB-086's provably-fails clause is
   unproven at that count.
