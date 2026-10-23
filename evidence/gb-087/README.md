# GB-087 — 1-CPU intel kernel GP: root cause + fix (DEEP Verifier, 2026-10-23)

**Result.** Fault explained with instruction/register/stack evidence, fixed on `debug/gb087-manifest-terminate`
(`a53f26e`). After the fix, `T3_SMP=1` on intel reaches `halted=1` with **0** halting kernel-mode faults on
both a POLLTST-only manifest and the default wave.

## The fault, as delivered

```
[ARRIVED] POLLTST.BIN rc>0
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 13 Error Code: 0x0000000000000000
  RIP: 0x0000000000000000  RSP: 0x0000000023FFF008
  CS:  0x0000000000000008  SS:  0x0000000000000010  RFLAGS: 0x0000000000010212
  ... CPU: 0  pid: 1
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0 cr2=0x0 pid=1 err=0
```
`files: 01-prefix-fault-serial.log`

## The chain (QEMU `-d int`, TCG so the log is populated)

`05-qemu-dint-chain.log`, events 205-209:

```
205: v=2a IP=0008:00000000700381b0 SP=0010:00000000755fe970     timer, kernel stack
207: v=2a IP=0008:00000000700381b0 SP=0010:00000000755fe970
208: v=20 IP=0008:00000000700381b0 SP=0010:0000000023ffe828     timer, interrupt stack
209: v=0d e=0000 IP=0008:0000000000000000 SP=0010:0000000023fff008
```

`0x700381b0` resolves to `spinlock_release_irqrestore`. Event 209 is the GP: **rip=0, CPL0, RSP one
qword ABOVE the stack top** — the signature of a `ret` that popped 0 (RSP advances by 8 past the value
it popped).

Instrumenting `restore_context()` showed the offending first resume:

```
[RESUME] pid=1 elr=0x0000000070014B30 cs=0x0000000000000000 rsp=0x0000000023FFF000 kern=1
```

`0x70014B30` = `test_wave_loader` (`src/kernel/main.c:141`); `0x23FFF000` = that kernel task's stack top
(`user_phys_base 0x22000000 + region 0x2000000 - 0x1000`). A fresh kernel task is entered by `iretq` with
`RSP == stack top`, i.e. **an empty stack — no return address, by construction**.

## Root cause

`test_wave_loader()` ends with `kernel_exit()`, which never returns — *except* on the manifest branch:

```c
if (dispatched > 0) return;   /* manifest wins; literals are the default */   /* main.c:184 */
```

With `::WAVE.CFG` present (the shape every M1 `-V` card uses), the loader **returns** instead of
terminating its task. On an empty kernel-task stack the epilogue's `ret` pops 0 → execution at address 0
with CPL=0 → `#GP` vector 13, err 0 → the CPU halts.

That is why the card is intel-only and manifest-shaped: the literal 97-program path reaches
`kernel_exit()` and never returns; the ARM build's task entry tolerates the stray return, x64 does not.

## Fix

`src/kernel/main.c` — the manifest branch terminates the loader task the same way the literal path does:

```c
if (dispatched > 0) {
  extern void kernel_exit(void);
  kernel_exit(); /* never returns */
}
```

## After the fix (same VM, same 1-CPU shape)

POLLTST-only manifest:
```
T3_intel: 181 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes,
          1 programs exited, halted=1
```
and the guest's own verdict in `02-postfix-polltst-serial.log`:
```
  POLLTST select(timeout < 0) blocks and wakes on data (child write): PASS
  POLLTST the blocking wait's byte reads back: PASS
ALL TESTS PASSED SUCCESSFULLY!
System halt.
```
(`T3_intel EXIT=1` in that run is the tier's 55-program floor, not a fault: a one-program manifest is
below the floor. `03-tier-polltst.log`.)

Default wave:
```
T3_intel: 30869 console lines, 81 FAIL lines, 4 user memory fault records across 4 processes,
          76 programs exited, halted=1
```
`07-default-wave-serial.log`: **0** `FATAL kernel-mode fault` lines, 1 `System halt`. The 81 FAIL lines are
user-level test failures (SEDTEST/TACTEST/TSORTTEST…), and 21 `FATAL: Exception in Kernel Mode! Vector: 14`
dumps remain — kernel-mode #PFs that are handled (the process is killed, the machine continues). Those are a
different defect family, not this card's GP, and they were present before the patch (8 of them, with
`halted=0`; the patch is what lets the wave reach the halt at all).

## What clause 4 changes on other cards

* **GB-086 (#137)** — its premise is "the select-timeout assertion passes at 8 CPUs and fails at 1".
  At 1 CPU with this patch, POLLTST's select/blocking checks **PASS** (`ALL TESTS PASSED SUCCESSFULLY!`).
  The 1-CPU failure it was built on was very likely this crash, not the assertion. Re-read before closing.
* **GB-081 (#127)** — ARM CMPTEST; an x64-only fix, so its ARM CPU-count split stands unchanged.

## Not done

* Clause 3's "deliberately-null vector dispatch" guard test: I have the tier's own detector failing the
  run when the crash is present (the pre-fix runs: `FAIL — 1 FATAL kernel-mode fault line(s)`), but I did
  not plant a synthetic null dispatch.
* A separate latent defect found on the way: `save_context()` reads the pre-trap RSP as
  `((uint64_t *)tf)[38]` — the *value at* `tf+304` rather than the address. For a same-CPL trap the CPU
  pushes three words, so `tf+304` IS the pre-trap RSP. I did not keep that change (it was not this fault and
  deserves its own evidence run).
