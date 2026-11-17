# cp-console — a line-atomic serial console (C0-console, wave 6)

Every gate grades from the guest's serial console (COM1, `-serial file:`). Before this
lane the console was atomic per *call*, not per *line*. On integ7 that destroyed
`[P5] end d3d11` in a P5 run whose probes had all passed, and P5 was graded FAIL. Kernel
rows from two CPUs also alternated byte by byte (`cpu1:[ P0UxB0]0 0w0h0o0=0f0a7u0l0t…`),
and kernel rows spliced into user rows (`recover_us=[P5] end fscheck`, `[BLKD] cc=[PUB] …`).

This lane changes printing only. No kernel behaviour outside the print path changes.

## 1. Every path that writes the serial console (map)

Call counts are for `src/kernel` at 058fd3d5. The "before" column is how each path reached
the wire. The "now" column is how it reaches it on this branch.

| # | Path | Where | Calls | Before | Now |
|---|---|---|---|---|---|
| 1 | `uart_puts(s)` | `arch/*/uart.c`, 73 files | 1922 | `uart_lock` held for the string only | per-CPU line slot, whole lines (§2) |
| 2 | `uart_putc(c)` | 2 files | 6 | `uart_lock` per character | line slot |
| 3 | `print_int`, `uart_print_hex` | `main.c`, 25 + 10 files | 284 + 33 | `uart_lock` per number | formatted locally, line slot |
| 4 | `kl_emit` (`kline.h`), `db_emit` (`process.c`), `pipe.c`/`process.c` record buffers | 30 files | 244 | one `uart_puts` per record (atomic only against other locked writers) | line slot |
| 5 | `uart_putc_dbg(c)`, the "lock-free NON-panic sink" | `fs.c`, `frame.c` (`frame_raw_*` tripwire), `x64/mmu.c` (FREERACE), `x64/locks.c` (`relsnap`), `x64/trap.c` (`wd_puts/wd_hex/wd_int`: the stall watchdog's `CSTK`/`CDATA` dumps from the timer IRQ) | 7 direct, 56 via `wd_*` | **no lock at all**, one character at a time; this is the integ7 byte soup | line slot |
| 6 | `panic_putc/panic_puts/panic_print_hex/print_int_raw`, the raw fatal sinks | `x64/trap.c` (fault record, `[BADVEC]`, `[TORNFRAME]`), `x64/locks.c` (`scream_*`: `[LOCKFOREVER]`, `[STALLBREAK]`, `[STOLEN]`), `process.c` (halt paths, resume diagnostics), `x64/mmu.c`, `ssp.c` | 231 + 13 + 28 + 38 | **no lock**; GB-097's accounting (`[PANIC-SINK-VIOLATION]`) for returning callers | line slot with a 20 ms lock bound on a fatal path; violation accounting kept |
| 7 | COM2 `panic_puts2/putc2/print_hex2/print_int_raw2` | `x64/uart.c`, `x64/trap.c` (WD2 stall dump), `process.c` (`wd_dump_proc_table`) | 58 | lock-free on COM2 (x64; QEMU drops COM2 without `-serial` #2) | unchanged on x64. On arm they are the same PL011, so they now go through the line slot |
| 8 | debugcon port 0xE9 `dbg_putc` | `x64/trap.c` `dbg_resume` | 2 | port 0xE9, not COM1 (no harness maps it) | unchanged |
| 9 | the `[UART] lock @ …` row | `x64/uart.c uart_init` | 1 | raw `outb`, one CPU, before anything else | unchanged (boot CPU, before the console layer) |
| 10 | user `write(1/2)` to a console fd or an unassigned fd 1/2, and `SYS_WRITE_CONSOLE` | `fs.c console_write_user` (from `file_write` and both arches' `sys_write_console`) | — | the user buffer staged (8 KiB windows), then one `uart_lock` hold per window split at newlines. A trailing partial line went out at once, so `"[X] "` + line + `"\n"` from a writer that does not line-buffer could be split by kernel rows | one console-lock hold per window, and a trailing partial line is **held for that thread** (pid) until its newline or 30 ms (§2.4) |
| 11 | XSESSION's relay of its children's stdout/stderr (`[X] ` prefix) | `src/user/x11sess/xsession.c relay_pump` reads the log pipe, emits `print("[X] "); print(line); print("\n")` | — | `print_console` is HobbyOS libc's line buffer (GB-094: one `SYS_WRITE_CONSOLE` per line), so one relayed line = one user write = path 10 | unchanged; path 10 keeps it whole |
| 12 | glibc/musl Linux programs (Wine, the probes) | `write(1/2)`, path 10 | — | as path 10 | as path 10 |

No other code writes COM1. `grep` shows the port only in `x64/uart.c`.

The `cpuN:` text in the integ7 example is not a prefix any writer prints. It is the
watchdog's `  CSTK cpuN:` row (path 5, from the timer IRQ, lock-free) interleaved
character by character with `[PUB]` rows (path 4).

## 2. Design (`src/kernel/kconsole.c`, `src/include/console.h`)

### 2.1 Line slots

Every kernel writer (paths 1–6) ends in `cons_kwrite(s, n)`. With IRQs masked, the bytes
go into a line slot owned by this CPU and the running context, 3 slots × 1 KiB per CPU.
A slot reaches the wire only:
- when it holds a whole line (a newline arrived), as one console-lock hold;
- when it overflows 1 KiB (`split` counter), as a piece of an open line;
- when it is stale (`stale`). The timer tick pushes it out once it is 50 ms old **and**
  the wire has been quiet for 50 ms, or once it is 2 s old. A busy console never breaks a
  line whose writer is only waiting to be scheduled. A 30 ms age-only rule tore 2 of
  39 000 probe rows under host load;
- when it is evicted because all three slots hold text (`evicted`);
- when its CPU halts (`cpu_halt_record` calls `cons_flush_local`).

Only the owning CPU touches its slots, always with IRQs masked, so the slots need no lock.

### 2.2 Contexts

A context is the running thread (`cpu_locals[cpu].current_proc` on x64,
`current_process()` on arm) plus "an interrupt nested in it". Nesting counts only when
both signals agree: the interrupt-enable state differs from the slot's, **and** the
stack pointer is more than 256 bytes away (a trap frame alone is larger). So:
- a row printed across an irqsave lock (IRQ state flips, same frame) keeps its slot;
- a helper with a big frame (same IRQ state) keeps its slot;
- the stall watchdog's dump from the timer IRQ (both differ) gets its own slot, and lands
  as its own lines;
- another thread on the same CPU (a kernel thread preempted by the 0x81 IPI) is a
  different context.

### 2.3 The console lock

The lock is a private ticket lock (`wire_next`/`wire_serving`). It is not a
`spinlock_t`. The spinlock screams print through this file, and the `spinlock_t` waiter
code prints `[LOCKFOREVER]`, so it must not recurse here. The ticket lock is FIFO. The
first version used test-and-set, and under the probe's load one CPU lost the race for more
than 200 ms, which is what the bound is for (measured: 37 tagged lines).
- **Halted holder:** a waiter passes the turn on at once (`broken`). A halted CPU never
  writes again.
- **No progress for 200 ms** (20 ms while a fatal record prints, `panic_depth > 0`): the
  waiter takes the wire over and prints anyway, tagged `[CONS!] ` (`forced`). It does
  not wait for ever. A live holder holds the wire for one line or one ≤ 8 KiB user window
  (about 25 ms at the measured rate), so this means a wedged or descheduled holder.
- **Re-entry on one CPU** (a fault inside the console code) is written raw, tagged
  `[CONS!] ` (`raw`). It never waits on itself.
- The early-boot clock may read 0 or stand still. A spin cap bounds the wait then.
- The wire remembers which writer left a line open. Any other writer starts with a
  newline, so a row always starts at column 0.

The waiter spins with IRQs masked, as `uart_lock` did. The FIFO bounds that time by the
queue length times one line.

### 2.4 User writes

`console_write_user` (path 10) stages each window (≤ 8 KiB, split at the last newline) and
calls `cons_user_write(pid + 1, buf, n)` for the **calling thread**. `file_write` passes the
group leader, so the thread comes from `current_process()`. Under one hold, the window does
this:
- a held partial line of that thread goes out first;
- then everything up to the window's last newline;
- the rest (≤ 512 bytes) is held for that thread until its newline;
- CPU 0's tick pushes a held partial out once it is 30 ms old and the wire has been
  quiet for 30 ms, or once it is 2 s old. A shell prompt still appears on an idle
  console, and the next writer's newline keeps its own row whole.

So `"[X] "` + `"[P5] end d3d11 status=0"` + `" ms=5\n"` from three `write()` calls stays
one line (host test T6, probe writer w=3).

### 2.5 The wire

`cons_hw_tx` (x64) uses the 16550's 16-byte transmit FIFO. One THRE observation admits 16
bytes. The old path polled LSR before every byte, one extra port exit per byte under KVM.
`uart_last_activity_ms` (the stall watchdog's input) is updated once per piece.

## 3. Verbosity knob: diagnostic rows off by default

Measured per-tag share of the serial bytes (strix, integ7 logs, `tagcount`):

| tag | P5 (p5b, 6.07 MB) | P8 lavapipe (6.62 MB) | P4 (2.96 MB) | P2 (1.98 MB) |
|---|---|---|---|---|
| `[PUB]` | 51.9 % (35 630 rows) | 27.1 % | 51.3 % | 60.4 % |
| `[ALIVE-SET]` slot rows | 17.8 % (9 397) | 25.9 % | 17.2 % | 4.3 % |
| `[FUTEX]` | 13.5 % (12 575) | 25.4 % | 4.1 % | 1.7 % |
| `[MR]` | 7.2 % (5 932) | 6.5 % | 14.8 % | 21.7 % |
| `[RESUME-RZ]` | 0.6 % | 0.2 % | 0.1 % | — |
| `[BLKD]` cc= | 0.3 % | 0.2 % | 0.4 % | — |
| `[ANONF]` | 0.0 % | 1.0 % | — | — |
| all of these | **≈ 91 %** | **≈ 86 %** | **≈ 88 %** | **≈ 88 %** |

Held back by default (`cons_dtags[]` in `kconsole.c`; the filter compares the tag at the
row's start, after leading spaces):

| class | rows | kept |
|---|---|---|
| `pub` | `[PUB] …` | `[PUB!]` (the tripwire alarm), `[PUBS]` (the 4096-row summary) |
| `mr` | `[MR] s=…` | `[MR] FAIL …` |
| `futex` | every `[FUTEX] …` | — |
| `alive` | `[ALIVE-SET] slot=…` (the per-slot dump) | `[ALIVE-SET] no READY work …` (graded by `tools/t3_tier.sh`), `[IDLESTUCK]` |
| `resume` | `[RESUME-RZ]`, `[RESUME-SPSR]`, `[RESUME-DIAG]` | — |
| `blkd` | `[BLKD] cc=…` | `[BLKD] data disk …` and the other boot rows |
| `pecow` | `[PECOW]` (only `tools/cp/pe/wine-ab.sh` reads it; run that with `CP_DIAG=pecow`) | — |
| `anonf` | `[ANONF]` | — |

Kept, because a gate, harness or tool greps for them: everything else. That includes
`[KERNEL]`, `[EXITPC]`, `[LCOMPAT]`, `[IDLESTUCK]`, `[LOCKFOREVER]`, `[STALLBREAK]`,
`[NFSRTT]`/`[NFSRA]`/`[NFSNET]` (`rsize_guard.sh` G5), `[NFSHARD]`, `[NFLK]`, `[EXT2]`,
`[THRS]` (`tools/rw`), `[SIGST]`, `[GDT]` and every fault or crash dump. Fault and crash
dumps are never filtered.

Turning the rows back on:
- **at boot, no rebuild:** `CP_DIAG=all` (or a class list, `CP_DIAG=pub,futex`) in the
  environment of `make … run`, `tools/gate/t3_linux_probe.sh`, `run-wine-p2.sh`,
  `run-p4.sh`, `run-wine-p5.sh` or `run-p8.sh`. It becomes
  `-fw_cfg name=opt/hobbyos/diag,string=…`. The kernel's first `[CONS]` row says what it
  read: `[CONS] line-atomic console: diag=all (fw_cfg opt/hobbyos/diag)`. The Venus
  harnesses build their QEMU line in `tools/cp/host-venus.sh` (C6's). There, add the
  same `-fw_cfg` argument by hand.
- **at build time:** `make CP_DIAG_DEFAULT=all …` bakes the default in.

A `[CONS]` row, at most once a minute and only when a count moved, reports the console's
counters and how many rows each class held back, e.g.
`[CONS] lines=23277 user=5222 forced=0 broken=0 raw=0 stale=0 evicted=0 split=0 diag-off: pub=23 mr=16 futex=1 alive=358 resume=18 blkd=10 pecow=0 anonf=1 (CP_DIAG=all to print)`.

## 4. Graders: rows found anywhere in a line

This is a second line of defence. A grade must not depend on the console being perfect.
`tools/cp/console_rows.py resync(text, tags)` splits every serial line at each occurrence
of a known row tag (`[P5] `, `[VK] `, …). A row behind another writer's bytes, or two
fused rows, still grade. XSESSION's `[X] ` in front of a tag is dropped. Only the
grader's own protocol tags are split on, and a tag must be followed by a space, so no
payload is cut (self-test: `python3 tools/cp/console_rows.py --self-test`).

| grader | change |
|---|---|
| `tools/cp/p5/boot_wine_p5.py` (P5, the Venus P6 rerun) | `resync` with P5/VK/D3D11/D3D12/FS/NFSHARD/NFLK. `<tag> done rc=0` must end its number. A probe with no end row is graded on its own rows (up to the next probe), not on an empty span. |
| `tools/cp/run-wine-p2.sh` (P2, both pfx boots) | `serial.txt` is `console_rows.py P2,WP`. `hello` may have a kernel tag glued after it. |
| `tools/cp/p4/p4_gate.py` | already regex-anywhere. `end <step> status=` captures digits only, so a glued row cannot turn `0` into `0[PUB]`. |
| `tools/cp/p8/boot_p8.py` | the launcher rows via `resync`. The progress count is no longer line-start anchored. |
| `tools/gate/t3_linux_probe.sh` | `[LP] done rc=0([^0-9]|$)` anywhere in a line. `[LP]` rows are shown from the tag. A probe that ships `tools/gate/lp_check_<name>.py` is also graded by it. |

Regrading integ7's P5 log (`p5b.serial.log`) with the new grader recovers d3d11's
RESULT, done, module and adapter rows and the `fscheck` end row. What it still reports
comes from the two rows the console destroyed. `[P5] end d3d11` is gone. `[P5] begin
d3d12` is gone, so none of d3d12's rows can be attributed to it. That is 8 failures
where the old grader reported 14. No grader can bring back a row that was shredded byte
by byte; only the console fix can.

The Venus P6 rerun (`tools/cp/venus/run-wine-venus.sh`) now writes to `obj/cp-p5/venus`
(`P5_OUT`, new in `run-wine-p5.sh`), so it no longer overwrites P5's `obj/cp-p5/wine`.

## 5. The probe: `linux_probe_console`

`src/user/linux_probe_console.c` runs for 60 s:
- 12 threads call a private compat syscall, `0x4843` (`src/kernel/linux_compat_cons.c`,
  printing only). It prints `[CSTR] c=<cpu> k=<kind> s=<seq> p=<payload> e=<len>` rows
  in the five shapes the kernel uses: multi-call `uart_puts`/`print_int`; one character
  at a time through `uart_putc_dbg` (the watchdog's shape); one kline; one character at a
  time through `uart_putc`; multi-call with IRQs masked for the middle pieces.
- 4 threads write `[LPC]` rows to the console: short, long (600–1800 bytes), eight rows
  in one `write()`, and one row in three `write()` calls.

Every payload is a function of its numbers. `tools/gate/lp_check_console.py` (run by
`t3_linux_probe.sh`) regenerates it from the console log and counts the rows below. It
first drops the `=== T3 guest sample … ===` notes that `tools/t3_tier.sh` itself
appends to the log file on a stall marker; that is the host writing into the file, not
the console:
- **interleaved** lines: a tag anywhere but column 0, or two tags on one line;
- **torn** rows: a row that does not parse, or a payload or length that is wrong;
- **missing** rows: against the probe's own totals;
- the CPUs the kernel rows came from (all 8 required).

| strix, KVM, `-smp 8`, 60 s | kernel rows intact | user rows intact | interleaved lines | torn | missing | CPUs |
|---|---|---|---|---|---|---|
| base (058fd3d5 + the probe and the stress syscall only) | — | — | **6605** | 1076 | 15 810 | 8 |
| this branch | 25 524 / 25 524 | 12 428 / 12 428 | **0** | 0 | 0 | 8 |

Host test: `make kconsole_test_host && ./kconsole_test_host` (also part of
`make host_tests`). It runs 15 cases against stub hooks: the multi-call row, an IRQ row on
the same CPU, a row across an irqsave lock, a preempted thread, the filter and the knob, a
user line in three pieces, a stale prompt, several lines in one write, a halted holder,
takeover past the bound, a line longer than a slot, and a stale kernel partial with its
continuation.

## 6. Cost: serial bytes and boot time

See the lane report and `docs/cp-console-evidence/` for the measured P5 runs before and
after (strix, KVM, `-smp 8 -m 12G`).
