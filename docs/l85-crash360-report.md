# L85 — the ~360 s `memory fault VA=0x2F9D63` in `Loading.AsyncRead`

Branch `rw-l85-crash360` (from `rimworld` @ `3e8e0291`, includes L83). Worktree
`~/hobbyos-rw-lanes/l85-crash360`. Nothing pushed.

## Verdict

**Ours, in the scheduler — not the game, not HBHOST, not the window size.**

The crash is a **double-executed thread**: HobbyOS's LOSTWAKE reaper decided that
a RimWorld thread "executes nowhere", re-`READY`ed its slot, and a second CPU
resumed it *from its saved context while the first CPU was still running it*.
Two CPUs then shared one register file and one stack. The three observed fault
signatures are the ways that ends:

| run | row | signature |
|---|---|---|
| L83 | `VA=0x2F9D63 rw=w in=HOLE`, `Loading.AsyncRead` | a store through a callee-saved register (`r15`) that holds a small integer, not the object pointer it was set to |
| L85 run 1 | `VA=PC=0x112B806700 rw=rx`, `Loading.AsyncRead` | `ret` popped the thread's own name string ("Loading.AsyncRead") off the stack as a return address |
| L85 prefix P2 | `VA=PC=0x112DE1D2E0 rw=rx`, `UnityGfxDeviceW` | the same stack-execution, on a different thread, in an un-instrumented tree |

and in all three runs the two rows immediately before the fault are the reaper's:

```
[LOSTWAKE] disposing pid=30 Loading.AsyncRe (stale dead-owner claim, poisoned resume)
[LOSTWAKE] reclaiming pid=30 Loading.AsyncRe from RUNNING with no claiming CPU
[KERNEL] pid=30 (Loading.AsyncRe) memory fault VA=...
```

The trigger is *host* CPU starvation, which the IceWM/HBHOST path makes much more
likely (HBHOST's futex storm puts a continuous [FUTEX]/[FUTR] print load on the
console; `uart_puts` holds `uart_lock` IRQ-off per character, so a core blocked
there misses its heartbeat for seconds and looks dead). The defect is that our
recovery trusted that inference and acted on it destructively.

## 1. The fault, with the faulting instruction

From `obj/l85-run1.out` (run 1 of this lane, pre-fix,
`RW_QEMU_GL=1 RW_GLMODE=virgl RW_SCREEN=1280x720 RW_WM_HONOUR=1`, IceWM
Start-menu launch, `RW_SC_RING=1`):

```
[KERNEL] pid=30 (Loading.AsyncRe) memory fault VA=0x000000112B806700
         PC=0x000000112B806700 rw=rx in=PROT region=0x112B007000+0x800000
         prot=3 kind=3 rsp=0x112B805CF0 live=26 inH=0 cr3frame=0x2636E000
         asroot=0x2636E000 sys=0 ret=0xAAAB8
         rbx=0x1100000270 rdi=0x1100000334 rsi=0x80 rbp=0x112B805D98
         rbp0=0x2E676E6964616F4C ret=0x616552636E797341 -> killed
```

* `PC == VA == 0x112B806700` and `rw=rx`: the thread *executed* a stack address.
* `rbp0 = 0x2E676E6964616F4C` = bytes `4C 6F 61 64 69 6E 67 2E` = `"Loading."`,
  `ret = 0x616552636E797341` = `"AsyncRea"`. The qwords the thread's `ret`
  consumed are the ASCII of its own thread name — i.e. the stack pointer was
  standing on a name buffer, not on a call frame.
* `err=0x15` (P=1, U=1, I/D=1) while `[SLEAF] cr2=0x112B806700 present=0` and
  `[SRGN] total=0`: the hardware still had a translation, the kernel's walk found
  none, and the whole region table was **empty** by the time the dump ran — the
  group's address space was being torn down concurrently with the fault.

### The L83 variant, decoded

L83's evidence (`~/hobbyos-rw-lanes/l83-icewmlaunch/obj/l83-evidence/`) has the
same pid/thread and the same precursor rows:

```
[KERNEL] pid=30 (Loading.AsyncRe) memory fault VA=0x00000000002F9D63
         PC=0x000000108282D060 rw=w  in=HOLE region=NONE rsp=0x112B805800
         rbx=0x108010E217 rdi=0x1150300000 rsi=0x200000 rbp=1 ..
[SGPR] rax=0 rbx=0x108010E217 rcx=0x1081DFBE58 rdx=0x108282D060
       rsi=0x200000 rdi=0x1150300000 rbp=1 r8=0x112B805720
       r9=0x3FFFFE00000 r10=0x1100C0C750 r11=0x246
       r12=0xFFFFFFFFFFD066BA r13=0x100000001A r14=0x1150500010 r15=0x2F992F
```

`PC=0x108282D060` is `UnityPlayer.so+0xcf5060` (base `0x1081b38000`). On-disk:

```
cf4fc0: push rbp; push r15; push r14; push r12; push rbx
cf4fc8: mov  0x428(%rsi),%rax      ; this->file
cf4fd8: mov  (%rax),%ebp           ; fd
cf4fec: mov  %rsi,%r15             ; r15 = this
cf4ff2: mov  %r12,%rsi             ; rsi = buf
cf4ff8: call read@plt
cf4ffd: cmp  $-1,%rax              ; EOF / error path
cf5027: mov  (%rax),%eax           ; errno
cf503d: jmp  *%rdx                 ; errno -> Unity error code
cf5060: mov  %eax,0x434(%r15)      ; this->error = code   <-- FAULT
```

So the faulting instruction is `41 89 87 34 04 00 00` = `mov %eax,0x434(%r15)`,
and `0x2F992F + 0x434 = 0x2F9D63` = the reported `VA`. `rax = 0` and
`rdx = 0x108282D060 = rip` prove the thread was executing this function's own
tail exactly (the errno-0 jump-table entry targets `cf5060` directly, so `rdx`
*is* the jump target). `rbx = 0x108010E217` is the `read` count argument's
leftover, `rcx = 0x1081DFBE58` is the table base at `cf502f`.

**Provenance of `0x2F9D63`:** it is `r15 + 0x434`. `r15` is the callee-saved copy
of the caller's `rsi` — the object pointer this read helper was invoked on. So
the bogus value was **passed in by the caller** and is exactly the pattern the
brief predicted: a small integer used as a base. It is not a truncation of a live
pointer in a way that matters: [`SALIAS`] tests every region's low-32 window and
`[SRGN] total=` shows whether any mapping could own it (see the run-1 rows), and
`0x2F9D63` is inside no region at all.

The decisive detail is not the value but the **inconsistency**: `cf4ff2` executes
`mov %r12,%rsi`, so at `cf5060` `rsi` must equal `r12` — measured `rsi=0x200000`,
`r12=0xFFFFFFFFFFD066BA` (which is `-(0x2F9946)`, another small integer). And
`r15` must equal the `this` that the function read `0x428(%rsi)` from at entry —
if `r15` had been `0x2F992F` at entry, the *read* at `0x2F9D57` would have faulted
first. So the thread entered with a valid `this` and a valid `buf`, then continued
with two callee-saved registers replaced by small integers. No single-threaded
execution of this function can produce that; two CPUs sharing the register file
can.

## 1b. The thread's last syscalls (`[SCH]`, run 1)

`RW_SC_RING=1` adds a per-thread ring of the last 24 compat syscalls with
arguments and return values, printed by the fault path. For pid 30 at its kill:

```
[SCH] pid=30 tgid=7 nm=Loading.AsyncRe ring=24
[SCH] -23 nr=202 INFLIGHT a0=0x1100000334 a1=0x80 a2=0x0 a3=0x112B805CF8 -> 0x0 rip=0x108011895D
[SCH] -22 nr=8   done     a0=0x12 a1=0x7AECFA0 a2=0x0 a3=0x112B805C28 -> 0x7AECFA0 rip=0x108010E32B
[SCH] -21 nr=0   done     a0=0x12 a1=0x1130494038 a2=0xAAAB8 a3=0x112B805C28 -> 0x0
[SCH] -20 nr=202 done     a0=0x1083B1B624 a1=0x81 ...
[SCH] -19 nr=202 done     a0=0x10E080EBB0 a1=0x81 ...
[SCH] -18 nr=202 INFLIGHT a0=0x1100000334 a1=0x80 ...
...
[SCH]   0 nr=0   done     a0=0x16 a1=0x1150500010 a2=0x29C070 a3=0x112B805990 -> 0x0
```

(nr: 0 = `read`, 8 = `lseek`, 202 = `futex`.)

Two things this settles:

* **No syscall of ours handed the thread a bogus value.** Its last file calls are
  a successful `lseek(fd 18, 0x7AECFA0)` and a `read(fd 18)` that returned 0 at
  EOF — exactly what a Unity async reader does between chunks — and no `read`
  returned a count larger than its request or a positive `-errno`. The two
  entries that never completed are `futex` calls: the store that killed the
  thread was ordinary game code, not a syscall return path.
* **Two entries are `INFLIGHT` in positions older than completed ones.** A
  single executing context cannot leave an older syscall unfinished while newer
  ones have returned (its ring index only advances at entry, and `done` is set
  on the way out). Two CPUs advancing the same thread's ring is the only way to
  produce that shape — corroboration for §2, from a different instrument.

## 2. The mechanism (kernel rows, in order)

1. `[IDLESTUCK] claims: c6=30/68` — CPU 6 claims pid 30, 68 idle rounds ago.
2. The console is saturated: `[FUTEX] r#2105xx` rows (HBHOST's wakes), `[FUTR]`
   rate dumps, `[NFSRTT]` — every row is a `uart_puts` under `uart_lock` with
   IRQs off, so a core spinning for the lock cannot refresh
   `cpu_heartbeat_ms[]`.
3. `lostwake_stale_claim_reclaimable_at(30)` therefore answers "stale claim".
4. The dispose path **cleared every CPU's claim on pid 30**
   (`set_current_process_pid(c, -1)` for each claimer) before calling
   `group_teardown()`, which manufactures the answer the rest of the file tests
   for: `process_still_running(30)` now says *not running*.
5. The next idle pass finds `state == RUNNING` with no claimer and runs
   `[LOSTWAKE] reclaiming ... from RUNNING with no claiming CPU` → `READY`.
6. The idle picker claims it and `enter_user_space()` resumes it from its saved
   trap frame **on a second CPU**, while CPU 6 is still executing it.
7. Its stack frame is destroyed under it: run 1 died at `PC=0x112B806700` (its own
   stack) with its name string as the return address; L83 died writing through a
   register the other CPU had replaced.
8. In parallel, the freed claim made `proc_release_mem_locked()` free the group
   address space; `mmu.c` prints `[FREERACE] freeing root=0x2636E000 while cpu=6
   still translates through it` — the kernel already *detected* this shape and
   only reported it.

The IceWM/HBHOST path supplies step 2. The window size is irrelevant to it (it
changes the GL/present cost, not the console or the scheduler), and the native
`DESKTOP=hobby` path has no HBHOST, hence no futex storm — which is why the old
desktop reached the menu at ~530 s instead of dying at ~360 s.

## 3. Fix (committed on `rw-l85-crash360`)

1. **`vm_arch_root_cpu(root)`** (`src/kernel/arch/{x64,arm}/mmu.c`, declared in
   `src/include/vm.h`): which CPU still has this root loaded. Ground truth for
   "executing", unlike `cpu_current_pids` (which the reaper itself clears) and
   unlike the heartbeat (which a starved core cannot refresh).
2. **`slot_as_on_live_cpu(k)`** (`src/kernel/process.c`): the slot's group AS is
   loaded on a CPU that is not halted. A halted CPU is excluded because
   `cpu_halt_record()` is dead by construction, so the poisoned-resume recovery
   still fires for the case it was written for.
3. **The reaper consults it** in both destructive branches:
   * no-claimer reclaim → `if (teardown_done || slot_as_on_live_cpu(k)) continue;`
   * stale-claim dispose → `!slot_as_on_live_cpu(k) && lostwake_stale_claim_reclaimable(k)`
   * the decision itself (`lostwake_stale_claim_decide_at`) also refuses a
     `teardown_done` slot.
4. **The dispose no longer clears a live CPU's claim.** Only halted CPUs' claims
   are released (provably safe). Leaving a stale claim in place keeps
   `process_still_running()` conservative.
5. **`proc_release_mem_locked()` refuses to free an AS another CPU translates
   through** (the `[FREERACE]` report becomes the refusal it always deserved).
   The calling CPU is excluded so the normal self-teardown path is unchanged.

## 4. Evidence: A/B and post-fix runs

Workload for every row: `RW_QEMU_GL=1 RW_GLMODE=virgl`, launch from IceWM's
Start menu through HBHOST (except the `hobby` cell), `--run-timeout 900`.
"pre-fix" = branch tip `3e8e0291` (L83) or this lane before the fix (run 1,
same tree + `RW_SC_RING=1` only).

| # | tree | desktop | screen | outcome |
|---|------|---------|--------|---------|
| L83 | pre-fix | icewm | 1280x720 `/WMSIZE` | **kill at ~360 s**: `VA=0x2F9D63` in `Loading.AsyncRe` |
| 1 | pre-fix | icewm | 1280x720 `/WMSIZE` | **kill at ~460 s**: `VA=PC=0x112B806700` in `Loading.AsyncRe` |
| 2 | fixed | icewm | 1280x720 `/WMSIZE` | **no AsyncRead kill**; reached the def pass and died at the known def-load wall at ~480 s |
| 3 | fixed | icewm | 1280x720 `/WMSIZE` | **clean to the 900 s deadline**: 320 MB of NFS, 780 presents, `nz=115104/115200` |
| 4 | fixed | icewm | 1920x1080 (no `/WMSIZE`) | **clean to the 700 s deadline**: 200 presents, full-frame `nz=243332/243332` |
| 5 | fixed | hobby (native desktop) | 1280x720 `/WMSIZE` | **clean to the 700 s deadline**: 430 presents, no `[HBHOST]` at all |
| P1 | pre-fix | icewm | 1920x1080 (no `/WMSIZE`) | no AsyncRead kill to 590 s — but `[XSW] n=3840 swap=3840 put=17 putok=17`, i.e. the present path was failing, so this cell did not reach the same workload |
| P2 | pre-fix (independent tree, no `RW_SC_RING`) | icewm | 1280x720 `/WMSIZE` | **kill at ~620 s**, same family, *different thread*: `[LOSTWAKE] disposing pid=31 UnityGfxDeviceW` → `reclaiming pid=31` → `VA=PC=0x112DE1D2E0 rw=rx` (its own stack) |

Pre-fix 1280x720: **3 of 3 runs killed by the same reaper pair** (AsyncRead at
~360 s, AsyncRead at ~460 s, GfxDeviceW at ~620 s), while all four fixed runs
carry **zero** `[LOSTWAKE]`/`[LWK]` rows. P2 matters because its tree is the
unmodified `3e8e0291` build with none of this lane's instrumentation: the effect
is not an artifact of the `[SCH]` ring.

### What the A/B says (and does not)

* **The crash does not follow the THREAD either.** The same pair kills
  `Loading.AsyncRead` at 1280x720 and `UnityGfxDeviceW` at the same resolution in
  another run: whichever slot the reaper mis-disposes is the one that dies. That
  is what a scheduler race looks like and what a game bug does not.
* **It does not follow the WINDOW SIZE.** The pre-fix tree at 1920x1080 did not
  crash, but it also never ran the same asset-load (present path failing,
  `putok=17`), so that cell is not a clean control. The mechanism is
  size-independent by construction: the trigger is console/host starvation of a
  vCPU and the destructive act is a scheduler decision.
* **It does not follow the DESKTOP either** — the fixed tree is clean on both,
  and the `hobby` cell has no HBHOST at all. The desktop only supplies the
  *trigger*: HBHOST's futex storm (`[FUTEX] r#2105xx`, `[FUTR] … rate=183/s`)
  puts a continuous print load on `uart_lock`, which is held IRQ-off per
  character. That is why the IceWM path dies at ~360-620 s while the old native
  desktop reached the menu at ~530 s: same race, less starvation.
* Limitation, stated: one run per cell for 1920/hobby (time budget), three for
  the pre-fix primary cell and four fixed runs at 1280x720. The claim that does
  not depend on counts is the mechanism row sequence in §2, identical in all
  three pre-fix kills.

### Run P2 detail (pre-fix, independent tree) — the third signature

```
[LOSTWAKE] disposing pid=31 UnityGfxDeviceW (stale dead-owner claim, poisoned resume)
[LOSTWAKE] reclaiming pid=31 UnityGfxDeviceW from RUNNING with no claiming CPU
[KERNEL] pid=31 (UnityGfxDeviceW) memory fault VA=0x000000112DE1D2E0
         PC=0x000000112DE1D2E0 rw=rx in=PROT region=0x112D621000+0x973000
         prot=3 kind=3 rsp=0x112DE1D220 live=33 ... sys=1 ret=0x5D
         rbp0=0x112DE1D340 ret=0x112CA3F6C2 -> killed
```

`PC == VA`, `rw=rx`, and the containing region is `prot=3 kind=3` — a stack — on
`UnityGfxDeviceW`, at 1280x720, in a tree that has none of this lane's code. So
the reaper pair kills whichever thread it mis-disposes, and the effect needs no
instrumentation to appear.

### Run 2 detail (fixed)


```
[RW] rwlaunch: child still running after 480s
[KERNEL] pid=8 (/NFS/RimWorldLinux) memory fault VA=0x0000000000000000
         PC=0x00000010B8E9D7F7 rw=r in=HOLE region=NONE rsp=0x17FFFFA940
         live=26 ... sys=228 ret=0
[RW] rwlaunch: waitpid=8 status=11
```

and `Player.log` ends with a *managed* exception, not a native one:

```
Could not execute post-long-event action. Exception: System.NullReferenceException
  at Verse.BuildableDef.ResolveIcon () [0x00074]
  at Verse.ThingDef.ResolveIcon () [0x00000]
  at Verse.BuildableDef.<PostLoad>b__78_0 () [0x00021]
  at Verse.LongEventHandler.ExecuteToExecuteWhenFinished () [0x0007c]
```

That is the **pre-existing def-load wall**, already documented in
`docs/l76-handshake-report.md` §6 (`Verse.GraphicData.Init` NRE followed by
`[SIGDUMP] SIGSEGV addr=0x0 ... rip=0x10b8d29f52 (JIT chunk)`), on the same
`0x10B8…` JIT chunk range. So the fix does not create it: it *uncovers* it by
keeping the game alive past asset loading. `NFSRTT bytes` grew from 52.9 MB at
the pre-fix kill to 56.8 MB here, and the run reached the def
cross-reference phase (`[XO] …/Biotech/…/Keyed/Letters.xml`).

<!-- A/B cells filled in below -->


## 5. Unit tier

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`

| build | result |
|---|---|
| `RW_SC_RING=1` (instrumented, used for the runs) | **436 run, 0 failed** |
| default (`make ARCH=intel MODE=desktop DESKTOP=icewm hobbyos.elf`, no flag) | **436 run, 0 failed** |

435 before this lane; the new one is `test_lostwake_as_live_blocks_reclaim`
(`src/kernel/process_test.c`), which drives the new decision input both ways:
control (`as_on_live_cpu = 0` → reclaimable, the frozen-soak case the recovery
exists for), the measured defect (`= 1` → refused), and a torn-down slot
(`teardown_done` → refused).

The existing fixture needed one premise added: a slot left `teardown_done` by an
earlier suite is no longer a requeue candidate, so the test now clears that flag
(and any foreign claim on the slot) before asserting the frozen-soak case. That
is a fixture statement, not a behaviour change.

No `src/user/` probe fits: the defect is a scheduler decision over
`cpu_current_pids` / `v2_active_root`, which user space can neither observe nor
provoke. The instrument that *did* answer it is kernel-side (`[SCH]`, `[SLEAF]`,
`[SALIAS]`, `[SRGN]`, `[LWK]`), and those rows are the evidence quoted above.

## 6. Artifacts (outside the report, in the worktree)

| file | what |
|---|---|
| `obj/l85-run1.out` | pre-fix run: full harness output, fault ladder |
| `obj/l85-run2.out`, `obj/l85-run2-serial.log`, `obj/l85-run2-Player.log` | first fixed run (def-load wall) |
| `obj/l85-run3*.{out,log}` | fixed, icewm 1280x720, clean to 900 s |
| `obj/l85-run4*.{out,log}` | fixed, icewm 1920x1080, clean to 700 s |
| `obj/l85-run5*.{out,log}` | fixed, native `hobby` desktop 1280x720, clean to 700 s |
| `~/hobbyos-rw-lanes/l85-prefix/obj/l85-prefix-*.{out,log}` | pre-fix tree runs (independent, un-instrumented) |

## 7. What this lane does *not* fix

* **The def-load wall.** `Verse.BuildableDef.ResolveIcon` /
  `Verse.ThingDef.<PostLoad>` NRE followed by a JIT-chunk NULL deref
  (`VA=0x0`) — documented in L76 §6 and reproduced by run 2. That is the next
  frontier, not this one.
* **HBHOST's present path.** In the 1920 cell `[XSW] n=3840 swap=3840 put=17
  putok=17` — L84 owns that. It matters here only because a failing present path
  changes the workload shape (and therefore how comparable a cell is).
* **The console starvation itself.** `uart_lock` is held IRQ-off per character,
  so a print storm still freezes a core's heartbeat for seconds. The reaper no
  longer acts destructively on that, but the right long-term fix is a buffered
  console writer; this lane deliberately did not touch it (it is the trigger, not
  the defect).
* **A deliberately accepted trade-off.** A CPU that is wedged *inside* a group's
  address space but not halted (e.g. spinning in the kernel with that AS still
  loaded) now blocks the dispose instead of being torn down under itself. The
  recovery for the case it was written for — `cpu_halt_record()` then a stale
  claim — still fires, because halted CPUs are excluded from the guard. Trading
  a possible hang for silent stack corruption is the intended direction: a hang
  is diagnosable, the corruption was not.

## 8. Side observations for the integrator

* **The ARM kernel does not compile at this tip, pre-existing and unrelated:**
  `src/kernel/process.c:1888` / `:1913` use `local_tf.cs` (an x64 `trap_frame`
  member added by L36/L37) outside any `#ifdef __x86_64__`, so
  `make ARCH=arm obj/arm/process.o` fails with `no member named 'cs'`. Both
  lines predate this lane (`git log -S local_tf.cs` → `3d56b4ad`, `75209a6c`).
  The x64 tiers are unaffected. My own ARM addition (`vm_arch_root_cpu` in
  `arch/arm/mmu.c`) does compile — `obj/arm/mmu.o` builds clean.
* **Cold `DESKTOP=icewm` builds are broken in a fresh worktree**:
  `third_party/x11/.../os/utils.c` calls `clock_getres`, which
  `src/libc/include/time.h` does not declare, so `bash third_party/x11/build.sh`
  dies at `[107/310]`. This lane unblocked itself by copying a built
  `third_party/x11/build/intel` from the L83 lane (same commit) — the Makefile's
  `x11-staged` rule accepts a prebuilt `bin/`. Fixing it properly means adding
  `clock_getres` to our libc (declaration + a 10 ms implementation); noted, not
  done here.


