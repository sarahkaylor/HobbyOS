# P4 — OS-side evidence note (FOR THE CONTROLLER/O1, not applied)

2026-10-09 ~07:5x UTC · Lane P4 (engine-loop park forensics)
Kernel tree read: `~/hobbyos-lanes/perf-x64` (O1's domain — NOT edited).

## 1. State-enum correction to the P4 brief's evidence chain
The brief said "st=4 = PROC_STATE_FUTEX (verify enum numbering)". From
`src/include/process.h` the REAL numbering is:

    PROC_STATE_FREE 0 / ALLOCATED 1 / READY 2 / RUNNING 3 / EXITED 4 /
    BLOCKED 5 / WAIT_SPAWN 6 / WAIT_CHILD 7 / FUTEX 8 / THREAD_DONE 9

So in `GIANT.HTM-p3h-after2.log` (and before3) the browser pid=3:
- was `st=3 PROC_STATE_RUNNING` on a claiming CPU (`claims: c3=3`) for the
  ENTIRE silent phase (t≈35 s … t≈661 s; no FUTEX st=8 ever seen), and
- is `st=4` ONLY in the final dumps (t≈957 s) — i.e. PROC_STATE_EXITED,
  because the runner's `[CLOSE] grace expired, killing` reaped it.
=> The fixture park is NOT a wait. The main thread burns a CPU the whole
silent phase: word-for-word, it is inside the synchronous boot
`FrameLoader::load()` → `Document::implicitClose()` style+layout tail
(last marker `[COMP] checkCallImplicitClose did=1`, then silence).
The "all 8 CPUs idle" reading is misleading: 7 CPUs idle + 1 busy main
thread = IDLESTUCK fires on the idle cores; it is not a deadlock indicator.

## 2. Kernel futex/cond timing: NOT implicated for the 16 ms engine wait
- libc `pthread_cond_timedwait` (src/libc/src/pthread.c:523) computes
  ms_left and calls `ho_futex_wait(&seq, seq, ms_left)` with ms_left>0.
- kernel `process_futex` (process.c:3371-3373) sets
  `wake_ms = timer_get_ms() + timeout_ms` for `timeout_ms>0`; only
  `timeout_ms<0` (== -1, e.g. `pthread_cond_wait` no-timeout, thread join)
  leaves `wake_ms = 0` = infinite BY DESIGN.
- `process_check_sleeping()` (process.c:1090; called from schedule:1177 and
  the idle loop:2533) expires FUTEX waiters with `wake_ms>0` at the tick
  (context[0] = -ETIMEDOUT). So the engine loop's 16 ms `pthread_cond_
  timedwait` SHOULD expire and tick — verified structurally. No kernel-side
  timer bug is needed to explain this park.

## 3. Kernel patch DRAFT (FLAGGED — not applied, not in O1's tree)
Purpose: if a LATER leg (or the x64 arch) parks a thread in a genuine
PROC_STATE_FUTEX wait, the IDLESTUCK dump must name the futex — uaddr,
wake_ms and the word at uaddr — so a userland-vs-kernel wait is provable
from the serial alone. Current dump (process.c:2757-2772) prints state but
not the futex details. Draft for `src/kernel/process.c`, inside the
existing IDLESTUCK slot loop, after the slot=... line:

```c
        if (proc_table[k].state == PROC_STATE_FUTEX) {
          uart_puts("[IDLESTUCK]   futex uaddr=");
          print_hex64(proc_table[k].futex_uaddr);
          uart_puts(" wake_ms=");
          print_int((int)proc_table[k].wake_ms);
          uart_puts(" hb=");
          print_hex64(proc_table[k].heartbeat);   /* if field exists; else drop */
          uart_puts("\n");
        }
```
(Check the actual field names before applying — futex_uaddr/wake_ms exist
per process.c:118-124/3371-3373; heartbeat naming lives in the lost-owner
reclaim code. If `print_hex64` isn't a public helper, use the same
print_int/uart_puts pair used elsewhere. `*(uint32_t*)uaddr` would need a
careful vm_touch'd read like process_futex:3348 — optional.)

## 4. P4 asks the controller
- Merge this run's verdicts into STATE.md's P4 section after the fixture
  leg (the [WIN-doc] decoder in `p4/RUN-REQUEST.md`).
- Do NOT apply §3 without O1 sign-off; it is only useful if a FUTEX park
  actually shows up in a later leg (x64 CNN run 802 class, J1 scripts-ON).
