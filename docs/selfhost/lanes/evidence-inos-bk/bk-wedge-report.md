# In-OS build exec-wedge forensics — LOSTWAKE dispose of in-OS clang (2026-10-15)

Status: **analysed; mitigated; the full loop subsequently PASSED (attempt 4, clean — no
wedge).** Observed in attempts 1-3 of the full in-OS build (two variants, same class,
always on `src/kernel/process.c` = the longest TU; see "Variants" below). Mitigation
shipped in the driver (retry). Evidence serials in this directory:
`bk-full-attempt1-fail.serial.txt`, `bk-full-attempt2-fail.serial.txt`,
`bk-full-attempt3-hang.serial.txt`.

## Symptom

Both attempts of the full in-OS build (`SH.BIN /BUILD_KERNEL.SH all`) died
while compiling the same TU — `src/kernel/process.c`, the longest kernel
compile (3,498 lines) — with:

```
MAKE.BIN: *** [Makefile.hobbyos:220: /SRC/obj/arm/process.o] Error 97
[BUILD] make rc=2
[SELFHOST] phase=s-ful2-build-fail reason=tool-rc rc=512
```

`Error 97` is not a clang exit code: it is the kernel's **exec-rejection
sentinel** (`process.c:2816-2822`) — `group_teardown(..., 97)` delivered by
the LOSTWAKE reaper to the child's whole group.

## Serial-verified sequence (both attempts)

1. in-OS clang parses process.c to the end (both warnings — `:1169`, `:2997`
   — are emitted; attempt 2 shows the full header-open trace plus
   `process-<hash>.o.tmp` create).
2. `[LOCKDIAG]` dump(s): a spinlock spun ~1–2 s under MTTCG
   (`arch/arm/locks.c`, `LOCKDIAG_SPINS = 200000000`, raw **unlocked**
   `uart_puts_raw` per field — hence the character-level interleave of
   several CPUs' dumps in the capture). Attempt 2's dump names
   `lock=0x043E849950 val=0 caller=0x043A69FB28` (~1 GiB above the kernel's
   `0x40080000` link base; symbol resolution needs the run-time VA map —
   left to the kernel lane).
3. `[LOSTWAKE] disposing pid=5 /TOOLS/CLANG.BIN (stale dead-owner claim,
   poisoned resume)` + `reclaiming pid=5 /TOOLS/CLANG.BIN from RUNNING with
   no claiming CPU`.
4. `[IDLESTUCK]` dump (the `acted` path), incl. 36 `st=9` THREAD_DONE LLD
   slots (`parent=-1`) left from the S1 links (P1.1 "reclaimable once off
   every CPU" state — observational, not proven causal).
5. make sees the child group exit 97 → `Error 97` → recipe failed.

In attempt 1 the dispose is additionally followed by a context fault on the
dying clang (`EC: 0x20`, instruction abort) — same event, the torn-down
context's dying gasp.

## Variants (same class, both on process.c)

- _A (attempts 1-2, exit 97):_ LOSTWAKE disposes clang pre-write — a 0-byte
  `process-<hash>.o.tmp` is left behind; make reports the group's exit 97.
- _B (attempt 3, silent hang):_ clang **completed the object write**
  (`process.o` valid on disk: 148 symbols, 72 text) and then hung in its
  exit path — one vCPU pegged (~108% host CPU, constant), zero serial
  output for ~20 min, no LOCKDIAG, no dispose (IRQs still arriving, so the
  watchdog considered the CPU alive).  Killed by the harness; the object
  was salvaged and reused by attempt 4.

## Mechanism (from the code)

- `process.c:2749-2822` (L9 arm-and-confirm): each idle CPU's reaper pass
  arms a candidate RUNNING slot whose freshest claimer heartbeat is older
  than `LOSTWAKE_DEAD_OWNER_MS = 2000`, and **disposes** it if the heartbeat
  has not advanced by the next sample.
- Heartbeats refresh: on idle (`:2694`), on every claim in `schedule()`
  (`:1270`, WK-3 / draft 0008), and on IRQ/syscall/load entry (`3b14758`).
  So a claimer that is genuinely running and taking timer IRQs cannot go
  stale — a 2 s stale window means the claiming CPU stopped taking IRQs.
- The code itself documents this failure shape (`:2726-2733`, for the
  *dump* path): `uart_puts` holds `uart_lock` IRQ-off per char; "a print
  storm can stall a core IRQ-off >2s, making the freeze look real". The
  dispose confirmation path still halts on the same heartbeat signal.
- Dispose **is** the designed recovery for a genuinely poisoned resume
  (soak parents re-fork on exit 97, comment `:2770-2786`). GNU make cannot
  re-exec a rejected child — hence the build dies.

## Why now, why here (hypotheses, ordered)

1. Stochastic: w12's acceptance loops compiled process.c cleanly in-OS twice
   (`rcut-loop.serial` / `rcut-loop2.serial`: 0 LOSTWAKE, 0 `Error 97`).
   Today 2/2 at the same TU — consistent with a rare per-minute hazard whose
   widest window is the longest compile.
2. Console contention: this feature's builds run with the pre-existing
   unconditional `fat16_open()` path trace on serial — every in-OS clang
   header open prints a line, adding console-lock traffic precisely during
   the long compiles. Not yet isolated.
3. LOCKDIAG prints are unsynchronized raw UART writes from spinning CPUs:
   they garble concurrent output and add UART pressure exactly when a lock
   is already stalled.

## Shipped mitigation (in this feature)

`src/user/selfhost/build_kernel.sh`: each make invocation is retried up to
3× (straight-line, in-OS-sh-safe). make is incremental, so a retry re-does
only the failed command (usually one TU) on a fresh process/address space —
matching the kernel's own refork-on-rejection recovery theory. Progress is
monotone; the build is only terminal if one command wedges on all 3 fresh
attempts.

## Kernel-lane recommendations

1. Exclude console-lock-dominated stalls from dispose confirmation (the
   same reasoning `:2726-2733` already applies to the dump path).
2. Serialize + rate-limit LOCKDIAG raw prints (they aggravate the UART
   contention they are diagnosing).
3. Consider an IPI-ping liveness probe for confirm (like the 20 s
   console-silence watchdog) instead of heartbeat-only.
4. Investigate whether THREAD_DONE slots accumulating under a long-lived
   parent (36 LLD threads after 4 links) has any second-order effect.
