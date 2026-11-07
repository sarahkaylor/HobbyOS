# L73 — the kernel-mode #PF at `cr2=0x11CB280000` is the **fault report reading the user's frame pointer**

M6/L73, worktree `~/hobbyos-rw-lanes/l73-kpf` (branch `rw-l73-kpf`, seeded at the
merged tip `5bb5d015`). The ladder's last wall after L72 was a kernel-mode #PF
*in user address space*: `cr2=0x11CB280000`, `[KPF-DIAG] … why=HOLE`, killing
pid 4 at t≈560 s. This lane names it, fixes it, and re-runs the ladder.

## 0. Summary

| question | answer |
|---|---|
| what faults? | the kernel's **user-fault report**: `*(uint64_t *)rbp` and `*(uint64_t *)(rbp+8)` at `trap.c`, "wk: essentials only" |
| why that address? | the report is emitted for a process that faulted on a bad pointer; `rbp` **is** that pointer. Measured: `cr2` = `base+len` of the 16 MiB mmap the log had just recorded (`[MR] s=2404 base=0x11CA280000 len=0x1000000`) |
| what is the "syscall"? | none. `[EXITPC] last_user_pc=0x1080118B06` = `libc+0x118B06` = the instruction after `syscall` inside glibc's **`mmap`** (`mov $0x9,%eax; syscall`) — the *last syscall the thread entered*, not the faulting path. `mmap` marshals no user pointer |
| why no user-fault row? | the row that would have named it is `uart_puts`'d **after** the `rbp` reads. The reads faulted, so the record never reached the console |
| the class | a plain `*(uint64_t *)user_ptr` where the safe accessor (`vm_user_read`) belongs — the same defect shape exists in the path-string entry points (validate 1 byte, then scan for the NUL with raw loads) and in the thread-exit `clear_child_tid` word |
| the fix | `kpf_user_peek2()` (validate + copy per page via `vm_user_read`) for the report; `u_strcpy()` (SEC-3) for every user path string; `vm_user_read`/`vm_kwrite` for `clear_child_tid` and the `spawn_ex` fdmap |
| result | see §6 |

## 1. The tuple, and what it is not

The L72 log's last four rows (`obj/rw-desktop/serial-desktop.log`, L72 run
`1800c`):

```text
[MR] s=2404 pid=4 g=4 as=0x75504570 len=0x1000000 base=0x11CA280000 w=H
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0
  RIP: 0x00000000700AB808  RSP: 0x00000000756501A0
  RAX: 0x000000000000014F  RBX: 0x00000011CB280000  RCX: 0x0000000000000150
  RDX: 0x0000000000000000
  RDI: 0x0000000075650210  RSI: 0x000000000000014A  RBP: 0x0000000075650510
  ERR: 0x0000000000000000  R8:  0x000000000000013F  R9:  0x0000000000000140
  CPU: 2  pid: 4
[KPF-DIAG] c00000011CB280000 pte_before=0 handled=0 pte_after=0 why=HOLE
[KERNEL] Kernel-mode #PF in user address space -> killing pid=4 cr2=0x11CB280000
          rip=0x700AB808 rsp=0x17FFFFA7F0 live=27
[EXITPC] pid=4 last_user_pc=0x1080118B06 code=0x4000000B
```

Three readings that look right and are wrong:

1. **"`rax=0x14F` is the syscall number."** It is not. `0x14F` is 335, which is
   not a Linux x86-64 number (the tree defines up to `L_SYS_MAX_KNOWN 440` but
   335 is unassigned), and `0x13F/0x140/0x14A/0x14F/0x150` are a *set* of small
   integers — the register row is whatever the faulting **kernel** code left in
   the volatile registers, which is why L72 could not name a syscall from it.
2. **"`rip=0x700AB808` names the function."** Not from the tree: the L72
   worktree's `obj/intel/hobbyos.boot.elf` (rebuilt at 00:09, after the 23:54
   boot) puts `0x700ab808` in `src/kernel/unix_test.c:226`, i.e. inside a unit
   test — proof that the ELF on disk is **not** the binary that ran. The address
   is therefore recorded as unresolvable rather than guessed. (The instrument
   added here removes that dependence: `[KPF-SYS]` prints the syscall tuple and
   `[KPF-STK]` the kernel return addresses, and `tools/rw/l73-ksym.py` resolves
   them against the ELF of *that* build, which the harness now keeps as
   `obj/l73/run<N>.elf`.)
3. **"the fault is `base+len` of a 16 MiB mapping, so an over-long copy ran off
   the end."** The address *is* exactly `0x11CA280000 + 0x1000000` — but the
   copy that ran off the end is not the kernel's: it is **the game's**.

## 2. What actually happened

`[EXITPC] last_user_pc` is latched at **syscall entry** only
(`sync_lower_handler_c`, `trap.c`), so `0x1080118B06` is the return site of the
last syscall pid 4 entered. Resolved against the pinned glibc:

```text
$ objdump -d /tmp/glibc231/lib/x86_64-linux-gnu/libc-2.31.so --start 0x118afc --stop 0x118b0c
  118aff: b8 09 00 00 00   mov $0x9,%eax        ; __NR_mmap
  118b04: 0f 05            syscall
  118b06: 48 3d 00 f0 ff ff cmp $0xfffffffffffff000,%rax
```

So the thread had just called `mmap(NULL-ish hint 0x11CA280000, 16 MiB)`, the
region was installed (`[MR] s=2404`), the syscall returned, and the thread
resumed in user mode. A few instructions later it dereferenced `0x11CB280000` —
one past the end of the buffer it had just been given — and took a **user-mode**
#PF.

That fault is *legitimate*: the address is a hole (a 16 MiB mapping at
`0x11CA280000` ends there, and the next region is not adjacent). The kernel's
user-fault handler could not resolve it (`vm_handle_fault` → `why=HOLE`) and fell
into the kill path that builds this record:

```c
          /* wk: essentials only */
          fl_s(&fl, " rbp=");
          fl_hex(&fl, tf->regs[6]);
          {
            volatile uint64_t *up = (volatile uint64_t *)tf->regs[6];   /* USER VA */
            fl_s(&fl, " rbp0=");
            fl_hex(&fl, up[0]);        /* <-- kernel-mode #PF at cr2 = rbp   */
            fl_s(&fl, " ret=");
            fl_hex(&fl, up[1]);        /* <-- and again at rbp+8             */
          }
```

`tf->regs[6]` is the **user's** `%rbp`. For a fault report that is precisely the
value most likely to be the bad address: the game had faulted on `%rbp`, so
`cr2 == rbp == 0x11CB280000`. The kernel read it with a plain dereference,
faulted a second time in kernel mode on the same hole, and the KPF path killed
pid 4 — **before** the `uart_puts(fl.b)` that would have printed
`[KERNEL] pid=4 (…) memory fault VA=0x11CB280000 PC=… rbp=0x11CB280000`.
That is why the L72 ladder has a KPF row and no user-fault row for the same
address, and why the death looks like a kernel bug.

The two instruments that would have caught this in one run already existed and
were pointed the other way: `compat_fault_dump` reads its stack words through
`fl_qwords` → `vm_user_read` (safe), and the `[GCT]` Boehm thread-table census
walks a user structure entirely with `vm_user_read`. Only this one block
dereferenced.

## 3. The fix

`trap.c`, report path:

```c
int kpf_user_peek2(struct process *p, uint64_t va, uint64_t out[2]) {
  out[0] = 0; out[1] = 0;
  if (!p || !va) return -1;
  return vm_user_read(p, va, out, 2 * (int)sizeof(uint64_t)) == 0 ? 0 : -1;
}
…
            uint64_t uq[2];
            if (kpf_user_peek2(cur, tf->regs[6], uq) == 0) {
              fl_s(&fl, " rbp0="); fl_hex(&fl, uq[0]);
              fl_s(&fl, " ret=");  fl_hex(&fl, uq[1]);
            } else {
              fl_s(&fl, " rbp0=<unreadable>");
            }
```

`vm_user_read` validates the region and the protection per page, materializes a
demand page first (`vm_touch`), then re-checks and copies under `vm_lock`
(SEC-2). A hole, a guard, or a straddle past the end of the region answers
`-1` — the report says `<unreadable>` and the process still dies of its own
SIGSEGV, which is the correct verdict.

## 4. The audit: the same shape elsewhere

The report was one instance of one pattern: **validate a byte, then dereference
the rest**. Each of these received the safe accessor:

| site | before | after |
|---|---|---|
| fault report (`rbp0`, `ret`) | `*(uint64_t *)tf->regs[6]` ×2 | `kpf_user_peek2` → `vm_user_read` |
| `sys_open` / `sys_unlink` / `sys_rename` / `sys_mkdir` / `sys_rmdir` / `sys_chdir` / `sys_mount` / `sys_umount` / `sys_vfs_bind` | `sys_user_range_ok(p, 1)` then the callee scans for the NUL with raw loads (`file_open`'s `filename[0..3]`, `vfs_abs_path`) | `u_strcpy()` into a `VFS_PATH_MAX` kernel buffer (SEC-3: every byte through `vm_user_read`) |
| `sys_spawn` (filename + flat args), `sys_spawn_ex` (path) | `sys_user_range_ok(p, 1)` then `while (p[i] && i < 31)` | `u_strcpy()` |
| `sys_spawn_ex` fdmap | `sys_user_range(...,n*8,0)` then `fdmap[2*k]`/`fdmap[2*k+1]` | `vm_user_read` of the pair |
| thread exit `clear_child_tid` | `vm_touch` then `*(volatile uint32_t *)ctid` (read **and** write) | `vm_user_read` / `vm_kwrite` |
| `fat16_page_probe` replay buffer (found on the ladder, §6 run B) | one `static uint8_t rawbuf[4096]` shared by every CPU; the copy to the caller happens a whole read later, so two concurrent probes hand the gate **the other probe's page** | per-CPU buffer |

Not changed, and why:

* `compat_fault_dump`, `[GCT]`, `u_strcpy`, `fl_qwords`, `vm_user_read`,
  `vm_kwrite` — already safe.
* `sys_getcwd`, `sys_get_args`, `sys_read`, `sys_write`, `sys_poll`,
  `sys_select`, `sys_socketpair`, `sys_sendmsg/recvmsg`, `sys_getsockopt`,
  `sys_setsockopt`, `sys_getrandom`, `sys_read_dir` — they validate the whole
  range with `sys_user_range`/`l_user_ok`, which *materializes* it
  (`vm_touch`), so a later raw access cannot fault; they keep a SEC-2
  validate-then-use window, recorded here rather than silently blessed.
* `strlen`-style native handlers on `ARM` (`src/kernel/arch/arm/trap.c`) have
  the same 1-byte shape; this lane is x64-only and does not touch that tree.
* the native signal-frame plant stores its restorer at `base - 8`, 8 bytes
  below the range it validated (`sig_frame_range_ok(grp, base, user_sp)`).
  Reachable only when the validated frame begins at the region's first byte.
  Recorded here with the one-line fix (`sig_frame_range_ok(grp, base - 8,
  user_sp)`) for the lane that next touches signal delivery; it is not on the
  RimWorld path measured here (the Linux-ABI plant is a different function).

## 5. Instruments

* `[KPF-SYS] cr2= err= why= rip= rsp= cpu= pid= nr= upc=` — the faulting
  address, the error code, the verdict, the kernel RIP, and the **syscall the
  thread last entered** with the user PC that issued it.
* `[KPF-ARG] upc= umod= a0..a2` / `[KPF-AR2] a3..a5 ring=` — the six syscall
  arguments latched at entry (`p->sc_arg`, `p->sc_rip`) and the last eight
  syscall numbers; by fault time the live registers are the faulting code's,
  which is exactly why L72 could not read them.
* `[KPF-RGN] below=…+… end=… kind= prot= flags= above=… gap= kind=` — the region
  the address fell out of (`end == cr2` is the "one past the end" shape) and the
  next region above it.
* `[KPF-STK] <8 kernel return addresses> frames=N` — scanned from the faulting
  RSP to this CPU's stack top over `[0x70000000,0x70100000)`. The older
  watchdog scan stops at `0x70080000` and reports nothing for a fault above it,
  which is where this kernel's `.text` ends.
* `tools/rw/l73-ksym.py` — resolves those addresses against the ELF of the run
  (`obj/l73/run<N>.elf`, saved by the ladder script); the kernel carries no
  symbol table, and the L72 lesson is that an ELF rebuilt later is a different
  binary.
* `tools/rw/l73-present-rate.py` — presents/second from `[XSW] n=` rows against
  the launcher's 10 s clock, per window.
* `tools/rw/l73-frame-diff.py` — two shots → diff pixel count, bounding box,
  and the fraction of the change inside the panel clock, so "the screen
  changed" cannot be satisfied by the clock.

## 6. Ladder

Every run is the desktop harness (`tools/rw/run-rimworld-desktop.sh --timeout
480 --run-timeout 1800`, `RW_WAIT_SECS=1900`) with the run's own kernel saved as
`obj/l73/run<N>.elf`, so its addresses can be resolved later.

| run | kernel | game t | presents | `[XO]` | res MiB | end state |
|---|---|---:|---:|---:|---:|---|
| L72 `1800c` (pre-fix) | L72 | 560 s | 1 | 2404 mmaps | 1135 | **kernel-mode #PF in user space** (`cr2=0x11CB280000`), pid 4 killed |
| L73 run 1 (instrument only) | `run1.elf` | 640 s | 350 | 1167 | 756 | alive at the 750 s deadline, **no KPF**; "Initializing." splash, presents with changing CRCs |
| L73 run A | `runA.elf` | 740 s | 360 | 1207 | 829 | **no kernel #PF**; `[BLKQ] device off` on LBA 0x5D → `controller reset #1` → 13 211 cached units dropped → the **launcher's** image page `pos=0x10000` re-read as zeros (`[TXT] IMG SUM got=0x76EFDDC5 want=0xDC910CE7`) → `[EIO] pid=3 RWLAUNCH.BIN` → SIGBUS, harness ends |
| L73 run B | `runB.elf` | 740 s | 330 | 1207 | 1120 | **no kernel #PF**; same `device off`/reset → the exec gate published a **zero page as verified** (`[TXT] REPLAY … publishing read 1` then `[PUB] … zero=4096 ver=1`) → `[KERNEL] User process fault! Vector: 6 RIP=0x108228b570` (#UD executing zeros in UnityPlayer's text) → pid 4 died; pid 3 then died on the image gate |
| L73 run C | `runC.elf` | 730 s | 350 | 1205 | 1130 | **no kernel #PF**, and the fix is *seen working*: the game faulted at `VA=0x2010` with `rbp=0xD8A000` (a hole) and the report printed `rbp0=<unreadable>` instead of taking a second, kernel-mode #PF. The game then faulted inside its own SIGSEGV handler (`inH=1`, `PC=0`) → default action → pid 4 exits; the block layer then cascaded (controller resets #1-#7) and the launcher died on the image gate (`raw sum=0xF07F9786`, a torn read) |

Reading of the ladder:

* **the kernel-mode #PF is gone.** Four full loads (750 s + 3×1800 s windows,
  1 167-1 207 `[XO]` rows each) produced **zero** `[KPF-SYS]`/`[KPF-DIAG]` rows
  and zero "Kernel-mode #PF in user address space" kills, against one in one
  L72 run. Run C is the strongest row: the game faulted with
  `rbp=0xD8A000` — a user address in a hole, exactly the L72 shape — and the
  report printed `rbp=0xD8A000 rbp0=<unreadable>` and survived to publish the
  whole record. Before the fix, that same value was the second, kernel-mode
  #PF.
* the L72 death at 560 s is **state-dependent**, not deterministic: run 1 (the
  same instrumented kernel, *before* the `rbp` fix) also survived past 560 s.
  That is why the fix is argued from the code and the tuple (§1, §2) and proved
  by the unit test (§7) and by run C's `<unreadable>` row, not by one ladder
  line.
* all three 1800 s windows ended at **730-740 s**, and in every one of them the
  trigger is the **block layer**: the emulated NVMe controller drops off
  (`device off … lba=0x5D`, the same LBA in A and B), resets, flushes ~13 k
  cached units, and the re-read of that page comes back zero or torn. Run A
  loses the *launcher's* image page to the image-sum gate; run B loses the
  *game* to a zero page the exec gate published as verified — a race in
  `fat16_page_probe`'s shared static buffer (fixed in this lane, per-CPU); run
  C loses the game first to its own SIGSEGV (then a fault inside its handler)
  and the block cascade follows.
* the loader gets **much** further than L72: 1 205-1 207 `[XO]` rows, the last
  ones `Core/…/Menus_Main.xml` (runs A/B) and
  `Biotech/…/Keyed/Misc_Gameplay.xml` / `GameplayCommands.xml` (run C) — the
  main menu's own strings — where L72 stopped at the def plateau (43.6 MiB).
* no menu: the loading screen keeps presenting its progress box (`Initializing.`
  plus a filling bar) at ~1.0 presents/s and the three-party handshake of L72
  §1 is still in the log when each run ends. Nothing here claims a frame that
  was not captured.

Present-rate evidence (`tools/rw/l73-present-rate.py`), run A:

```text
PRESENTS: n 1 -> 360  (+359)     PRESENT-ROWS: 39 (PRES_ROW_EVERY=10)
WINDOW: t=90s .. t=540s          RATE: 0.80 presents/s
LAST: n=360 w=1920 h=1080 changed=1
```

and the frame-to-frame content check (`tools/rw/l73-frame-diff.py`, shots 44 vs
73 of run A): 483 pixels changed, bbox `(932,568)-(1894,1069)`, of which 417
(86 %) lie **outside** the panel clock region — the progress bar filling in the
splash box, not the clock. The box that says `Initializing.` is the same one L72
photographed; what changed is that it is now *animating* and the present path is
live (`nz=1950488/1950488`, `changed=1` on every tenth row).

## 7. Regression

* `TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **433 run, 0 failed — UNIT TESTS
  PASSED** (431 before this lane + `test_l73_user_peek_hole` +
  `test_l73_entry_points_reject_holes`).
* gates (`tools/rw/l73-gates.sh`, one log per gate under `obj/l73/gates/`):

  | gate | verdict | what it proves |
  |---|---|---|
  | redzone | **PASS** | all six psABI red-zone slots survive a 100 Hz preemption |
  | mmap_race | **PASS** | two threads picking the same hole get distinct bases |
  | m2_mono | **PASS** | the official Mono 6.12 runtime runs a threaded C# program |
  | m2_tp | **PASS** | ThreadPool parks/wakes; `sem_timedwait` honours an ms deadline |
  | m2_threads | **PASS** | 8 pthreads: mutex+condvar+TLS+join+dlopen TLS |
  | m12 | **PASS** | 150 threads created+joined, per-thread TLS, `pthread_cancel` |
  | m3_window | **PASS** | the X11 shim serves SDL's whole XCreateWindow sequence |
  | m4_gl | **PASS** | libGL loads, renders a GL 3.3 core triangle, presents, tears down |
  | rwnfs | **PASS** | exec-from-NFS (`HDYN.BIN` runs from `/NFS`) |

  Run with `bash tools/rw/l73-gates.sh`; one log per gate in `obj/l73/gates/`.

Net: **433/0 units + 9/9 gates**, with the two new tests included.
