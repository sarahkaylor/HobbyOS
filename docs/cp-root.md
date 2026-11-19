# cp-root — page tables freed under a CPU that still uses them (H8-root, wave 10)

Base: `cyberpunk` = integ25 = f888a274. Branch `cp-root`. Host for every
number: **strix** (Ryzen AI MAX+ 395, Zen 5, KVM, QEMU 10.2.1), 8 vCPUs, 12 GiB
guests unless a row says otherwise. Evidence: `docs/cp-root-evidence/`.

Goal: no CPU translates through, reloads, or speculatively walks an address
space's page tables after they are freed.

## 1. Summary

- **Measured first.** Each CPU now publishes, in its own 64-byte record, both
  the root its bookkeeping says it runs and the root CR3 really holds (written
  only after the CR3 load retired). Every `vm_arch_teardown` checks both words on
  every other CPU, classes what a CPU found on the root is doing, and counts it
  (`[ROOTFREE]`, `[FREERACE]`).
- **The race is common, not rare, once a probe aims at it.**
  `linux_probe_rootfree` R5 (SIGKILL of a group whose seven threads spin, the
  parent parked in wait4) freed live roots in 4 of 5 boots of the
  instrument-only kernel (2–10 unsafe frees per boot, 21 in all), and every
  one of those boots then died. On integ25 itself the probe completes 0 of 5
  boots.
- **The main way in (H8-01).** The killed threads' CPUs all enter `schedule()`
  at once, find nothing READY and reap the parked slot
  (`proc_reap_parked_locked`) while still on the group's root. The L85 refusal
  in `proc_release_mem_locked` asks for the *lowest* CPU on the root. The reaper
  is often that CPU, so it frees while higher CPUs are still on the root, in
  the IPI that delivered the kill and spinning for `proc_lock` with interrupts
  off.
- **The fix: park, never wait.** A root that another CPU may be on is parked in
  a 64-slot lock-free table. The next teardown, or any CPU's idle loop, frees
  it once every such CPU has published a CR3 load away from it. Nothing waits
  (no IPI, no ack, no lock), so the park cannot deadlock under any lock the
  caller holds.
- **A second crash on the same path (H8-02).** `process_clone`, musl's
  `pthread_create`, published a new thread READY into a group being killed. A
  CPU later switched to it after the teardown and `vm_arch_switch` loaded
  CR3 = 0: a silent guest reset. `process_clone` now publishes only into a live
  group, and `vm_arch_switch` refuses a torn-down AS (`[ROOTSW0]`).
- **After:** rootfree **PASS 5/5** (40–61 parks per boot, 0 unsafe, 0 resets),
  plus 1/1 in the batch. The full integ25 gate batch is at parity (§7), with 0
  assertion rows. The paired fp720 is unchanged: world 26.6 vs 25.5 fps.
- **ARM had the same hole, plus ASID reuse.** ARM parks the same way. It
  invalidates the ASID before it frees the tables, not after. A CPU flushes an
  ASID locally the first time it runs a newer generation of it (ASID = tgid, so
  a reused pid reuses the ASID, and the switch is TTBR0 + isb only).

## 2. The instrument (d25cd54e)

`struct x64_as_pc` (`src/kernel/arch/x64/mmu.c`) is one cache line per CPU. It
replaces three packed `[MAX_CPUS]` arrays on the switch/IPI path:
`v2_active_root[]`, `x64_tlb_pending[]` and `x64_tlb_owed_root[]` (the C1-k7
invariant). `struct cpu_local` is untouched.

| field | written by | when |
|---|---|---|
| `root` | the CPU itself | **before** CR3 is loaded with a v2 root, cleared before the restore's load (bookkeeping, unchanged meaning; the shootdown targets on it) |
| `cr3` | the CPU itself | **after** every CR3 load (`vm_arch_switch`, `vm_arch_restore_kernel`, bring-up). `mov cr3` is serializing, so once this reads ≠ R the CPU can no longer walk R |
| `sw` | the CPU itself | set inside the two switch functions (diagnostics) |
| `tlb_pending`, `tlb_owed_root` | shooter / target | the 0x82 shootdown's words, as before |

"May be on R" = `root == R || cr3 == R`. Each word alone has a window:
`root` went to 0 one instruction before the load, and `cr3` is set one
instruction after it.

A CPU found on a dying root is put in one class, in this priority order:

- `cons`: inside `cons_hw_tx` (the H6-09 mark);
- `irq`: in an interrupt or exception handler (`sched_tpc[c].trap_depth` above
  the syscall level);
- `sw`: inside a switch;
- `stale`: CR3 holds R while the bookkeeping says otherwise (the old reload
  paths);
- `idle`: claims no process;
- `thr`: claims a thread of the dying group;
- `other`: anything else.

Rows:
- `[FREERACE] freeing root=… while cpu=N still translates through it why=<class> root=<0|1> cr3=<0|1> tgid= site=`
  (`site` is the `vm_as_teardown` caller). It is a kline record now; the raw
  `panic_puts` sink is gone from mmu.c, and the allowlist went from 7 references
  to 0.
- `[ROOTFREE] <why> td= self= onroot= unsafe= deferred= drained= parked= leaked= wait_max_us= cons= irq= sw= stale= idle= thr= other=`
  holds running totals. It is printed at the halt in every kernel. Test
  kernels also print it every 64th teardown and with each park (bounded):
  those rows go out through `uart_puts` from a path that can hold `proc_lock`
  with interrupts off, so 9c4b48b9 compiles them only under
  `KERNEL_MODE_TEST`. `[FREERACE]` and `[ROOTSW0]` stay in every kernel.

## 3. Measurement on the base

### 3.1 Before the probe: what the gates showed

- `[FREERACE]` fired once each in `integ22/p6venus.1`, `integ23/p2.1` and
  `integ23/p6venus.1`. Every one was during Wine's service shutdown (SIGQUIT,
  multi-threaded `winedevice.exe` / `svchost.exe` exiting); see
  `base-freerace-integ22-23.txt`.
- The instrument-only kernel showed 0 races in P2, exitgroup, exitrace, execthr
  and killgrp (one boot each, `~/cp/lanes/root-ev/m1`). Natural workloads hit
  it about once per Wine shutdown at most.

### 3.2 `linux_probe_rootfree` (f7268aae)

Each round forks a child with six threads. One spins in user mode, one loops on
`getppid`, one sleeps (its CPU idles and takes timer interrupts), one
first-touches pages, one writes the console, and one spins again. The child
then dies:

- R1: a worker calls `exit_group(0)`;
- R2: the last worker returns, the leader having gone first;
- R3: the parent SIGKILLs the group;
- R4: two R1 children at once;
- R5: seven threads all spin, and a helper SIGKILLs the group while the parent
  is parked in `wait4`.

R1–R4 run 150 children each (R4: 150 pairs) and R5 runs 150. The check is
`tools/gate/lp_check_rootfree.py`: FAIL on any `unsafe>0` or any `[FREERACE]`
row.

| kernel (strix, KVM, `-no-reboot`) | boots completing | unsafe frees | rows |
|---|---|---|---|
| instrument only (d25cd54e), 5 boots | **0/5** (A.1–A.4 die in R5, A.5 in R3) | 10, 5, 4, 2, 0 | 43 `[FREERACE]` |
| integ25 (f888a274), 5 boots | **0/5** (b25.4 dies in R1, b25.2/3/5 in R3, b25.1 in R5) | (no counter; its own `[FREERACE]` fired in b25.1 ×2, b25.4 ×1) | — |
| park only (16054105, no H8-02 fix), 5 boots | 2/5 (B.1/3/5 die in R3) | 0 | 0 |
| lane (60503d3d+), 5 boots | **5/5 PASS** | 0 (40, 61, 57, 59, 58 parked and drained) | 0 |

The paths found on base, counting CPU sightings in the 21 unsafe frees (one
free can find several CPUs):

| class | sightings | what the CPU was doing |
|---|---|---|
| irq | 42 | in the kill's IPI (reschedule) handler, spinning for `proc_lock` with interrupts off while the reaper held it |
| thr | 1 | in the scheduler tail of a killed thread, claim still set |
| idle, cons, sw, stale, other | 0 | — |

On the lane, the same probe parks 40–61 roots per boot, classed irq 28–67 and
thr 53–111. The longest park was 188 ms (median ~10 ms): a parked root waits
for the next teardown or idle loop.

**Reload path (`stale`): not seen.** The reload paths named in the brief
(`vm_arch_reload_cr3`, the 0x82 service, the old `vm_handle_fault` retry)
reload only when CR3 already holds that root (4112438a, de8df5e1). No sighting
had `cr3 == R` without `root == R`. The two words now differ only inside
`vm_arch_switch` / `restore_kernel`, and that is exactly the window `cr3`
closes.

## 4. H8-01: the reaper on the root (fixed, 16054105)

- A group dies by SIGKILL (`process_kill_group_sig`) or `exit_group`. Its
  threads' CPUs get the resched IPI and enter `schedule()` in it.
- `schedule()`'s "no ready processes" branch calls `proc_reap_parked_locked()`
  under `proc_lock` (GB-179) *before* `vm_arch_restore_kernel()`. The anchor
  slot was set FREE by `reap_deliver_locked` because the parent was parked in
  wait4. So `proc_release_mem_locked(anchor)` runs on a CPU whose CR3 is the
  group's root.
- The L85 refusal asks `vm_arch_root_cpu(root)`, which answers with the
  **lowest** CPU on the root. When that is the reaper itself it is excluded as
  "the caller tearing down its own AS". The other CPUs on the root (higher
  ids, spinning for `proc_lock` in their IPI handler) are never seen.
- `vm_arch_teardown` restored the caller's CR3 and freed every table. The
  spinning CPUs then took `proc_lock`, ran the scheduler and the next timer
  interrupt on freed and recycled PML4/PDPT frames. The kernel image is mapped
  through the root's own PDPT (`PDPT[1] → kimg_pd`), so even kernel text was
  reached through freed frames. This is H7-18's lapic_send_eoi #PF, and R5's
  dead guests.

**The fix: park the root.**
- `vm_arch_teardown(R)`:
  1. this CPU leaves R if either of its words says it is on it;
  2. it frees any parked roots that are now free (`vm_arch_reap_roots`);
  3. if no other CPU may be on R, it frees R's tables now. That is the common
     case: in the lane's gate batch the 80 probes other than rootfree parked
     **0** roots;
  4. otherwise it claims a free slot of `x64_dying[64]` by CAS and parks R
     there, with a `[ROOTPARK]` row (test kernels only, 9c4b48b9).
- `vm_arch_reap_roots()` runs from every teardown and from the idle loop
  (process.c, after the idle `vm_arch_restore_kernel`). For each parked root
  that no CPU may be on any more, it claims the slot by CAS (2 → 1), clears the
  slot's root and frees the tables.
- `x64_free_tables` re-checks every CPU first. A CPU found on the root at that
  point is the assertion: `[FREERACE]` plus `unsafe=`, which must stay 0.
- A full table leaks the root (`leaked=`) instead of freeing it.

**Why this is enough.**
- A CPU on a dead root is always in transit: in the IPI, the scheduler tail,
  a console write, or a spin with interrupts off. Its next scheduler decision
  is a CR3 load (`vm_arch_switch` or `vm_arch_restore_kernel`), and the idle
  loop restores on every pass.
- That load publishes the new CR3 only after it retired. With CR4.PCIDE and
  CR4.PGE off (`docs/cp-address-space.md` §1.6), a CR3 load drops every TLB
  entry and paging-structure cache entry of the old root. So once all words
  read ≠ R, nothing can walk R's frames again, speculatively or not.
- Leaf frames are freed with the tables, so a CPU that wrongly touches a dead
  user VA after its teardown writes into still-owned frames. With an immediate
  free they belonged to someone else.

**Why it cannot deadlock.**
- No path in it waits for anything: no IPI, no ack word, no spin on another
  CPU, no new lock. The slot claims are single CAS operations, and a failed
  CAS moves on to the next slot.
- So it is safe under `proc_lock` with interrupts off (the caller of
  `proc_release_mem_locked`).
- It also covers the cases the brief names:
  - a CPU inside a console write the host holds for seconds (the H6-09 laggard,
    `cons`);
  - a CPU spinning with interrupts off (`irq`);
  - two CPUs tearing down roots each other is on: each only parks.
- `vm_lock` is never taken: `vm_as_teardown` releases it before
  `vm_arch_teardown`, as before. vmhold's check is unaffected (§7).
- The cost of a CPU that never leaves (wedged, or halted on the root) is that
  root's frames, held for ever.

**Why not an IPI + ack.**
- The victims of the measured case spin for `proc_lock` with interrupts off,
  and the reaper holds it. An ack wait in the reaper would deadlock against
  them, unless every such spin polled the request. The spinlock wait does poll
  owed TLB flushes every 64 spins, but a console port exit cannot poll.
- The ack would also force a CR3 change on a CPU from inside an arbitrary
  interrupted context. x64's user resume runs at CPL0 on the user stack
  (`docs/cp-address-space.md` §1.5), so a CPU evicted there would fault on its
  own stack.
- Parking needs neither.

**The L85 refusal itself is unchanged.** `vm_arch_root_cpu` still returns the
lowest CPU. With the park it no longer matters for the frames: the refusal now
only delays slot reuse, as before.

## 5. H8-02: a thread published into a dead group (fixed, 60503d3d)

- `process_clone` (Linux `clone`, musl's `pthread_create`) marked the new
  thread READY with no lock and no liveness check. `process_thread_create`
  has had the GB-145 re-check for years.
- A SIGKILL landing while the PCB was being filled left a READY member of a
  dead group. `process_kill_group_sig` marks only the members it can see,
  and this one was not READY yet.
- After the teardown the idle loop picked it, and `vm_arch_switch` loaded
  CR3 = `as->root_phys` = 0. The guest triple-faulted with no console row.
  rootfree R3 hit it in 3 of 5 lane boots with only the park, and it is one of
  the base's R3 deaths.
- Captured with a guard: `[ROOTSW0] cpu=5 tgid=494 ver=0 caller=start_scheduler+…`,
  then `[RESUME-PREFLIGHT] pid=500 … -> QUARANTINE`. Pid 500 is the thread
  created while group 494 was being killed.
- Fixed twice over:
  - `process_clone` re-checks the group under `proc_lock` and leaves a
    never-run tombstone (the GB-145 shape);
  - `vm_arch_switch` refuses `root_phys == 0`: `[ROOTSW0]`, and the CPU stays on
    its kernel PML4. The resume preflight then quarantines the slot instead of
    the machine resetting.
- `process_clone` is thread-creation code, outside this lane's narrow file
  list. It is a separate commit, so it can be taken or dropped on its own.

## 6. ARM (same commits)

- **The same hole.** `vm_arch_teardown` restored only the calling CPU and
  freed. Another CPU's TTBR0 could still hold the root (the scheduling paths
  are the same generic process.c code). ARM keeps the kernel's identity map
  in TTBR0 too, so that CPU's kernel accesses also walk the freed L1.
- **Fixed the same way.** ARM publishes the TTBR0 root after the write and its
  `isb` (`arm_ttbr_pub`, one line per CPU), parks the root in `arm_dying[32]`,
  and drains it from the same idle hook.
- **TLBI after free.** The broadcast `tlbi aside1is` ran after the L2/L3 frames
  were already back in the pool. ARM's walk caches are ASID-tagged, so another
  CPU's cached intermediate entries could point into recycled frames. Now
  `tlbi aside1is` runs first, and only once no CPU is on the root.
- **ASID reuse.** ASID = tgid (8 bits; ARM keeps `MAX_PROCESSES` at 192, so
  ASIDs never alias), and a v2 → v2 switch is TTBR0 + isb only. A new process
  on a reused pid ran under the dead one's ASID. A CPU that still held TLB or
  walk-cache entries of the old root, from before the teardown's broadcast or
  refilled while it was still on the old root, served them to the new process.
  The root frame itself can also come back. Fix:
  - `arm_asid_gen[a]` counts the roots allocated under ASID a;
  - `arm_asid_seen[c][a]` is the generation CPU c last ran under it;
  - a v2 → v2 switch to a newer generation does a local `tlbi aside1` first.
  - The v1 → v2 switch already flushes everything.
- **Verified:**
  - `make ARCH=arm TOOLS_SKIP=1 hobbyos.elf` PASS;
  - the ARM unit tier, booted with the linker's `.bss` window assert relaxed
    in a scratch copy, ran 437 tests with 1 failure (`frame_test.c:326`), the
    same as integ25 under the same relaxation (`arm-unit.txt`). On both trees
    the `MODE=unit_tests` ARM image is 1.6 MB over that window, so the tier
    does not link unmodified.
  - No ARM boot of the probe: the compat class is x86_64-only.

## 7. Gates

The lane batch is `strix:~/cp/lanes/root-gates.sh`, which is
`integ-gates25.sh` pointed at `~/cp/lanes/root`. It ran 09:50–11:23 on
60503d3d+ (the code before 9c4b48b9, which only gates rows). Results are in
`root-gates.txt` (`docs/cp-root-evidence/gates.txt`), logs in
`~/cp/lanes/root-glogs/`. The host had load 7–11 throughout: H9-hunt4's game
boots, with a GPU guest, ran alongside.

| gate | lane | integ25 |
|---|---|---|
| build | PASS | PASS |
| probes | **82/83**: rootfree PASS (108 parked, 0 unsafe); shootwait FAIL (host load, see below) | 81/82 (vmhold FAIL, host load) |
| thr, exitgroup, exitgroup ×3, execthr, exitrace, forkcow, vmhold | PASS (vmhold max lw 5 µs) | PASS (vmhold failed in its batch, 3/3 reruns pass) |
| console | rows intact 30252/30252, 0 torn, 0 interleaved | the same |
| sigmask-gate / ext2_gate / exec_gate / rsize_guard | PASS (8/8) / PASS / PASS / PASS | the same |
| unit | 464 run, 0 failed | 464/0 |
| host_tests / kconsole | 40/42 (the gaming.md pair) / PASS | the same |
| castscan / panic_sink_allowlist / rsize_scan | 0 new / PASS / PASS | the same |
| ARM build | PASS | PASS |
| pfx_gate, ext2w_gate, reg_gate, ext2_test_host, nfs_proto | PASS | PASS |
| Gate P2 | G1–G3 PASS | PASS |
| Gate P4 | G1–G3 PASS | PASS |
| Gate P5 ×3 | 3/3 PASS (vkcube, d3d11, d3d12, fullscr) | 3/3 |
| Gate P6 Venus / P7 fullscreen / k5probes | PASS / PASS / 9 PASS | the same |
| thrstorm ×3 | PASS, T2 rounds 1141, 981, 1061, 0 faults | 1231, 1166, 1184 (quiet host) |
| P8 lavapipe ×2 / Venus | menu at t=206 / 283 / 130 s | 190 / 195 / 145 s |
| P9 cold (`cold-m86-pq.p9`) | script-done, game status 0, `wineserver -k` 225 ms, quiesce extra=0 | the same, 216 ms |
| P9 fp720 | below (paired) | |
| `[FREERACE]` / `[ROOTSW0]` / `unsafe>0`, all 95 batch consoles plus the P8/P9 serials | **0 / 0 / 0** (the sweep's one hit is lp_check_rootfree's own "0 [FREERACE] row(s)" text) | — |
| parks outside rootfree | **0** (80 probes, P2, P4, P5, P6, P8, P9) | — |

**shootwait, the host-load flake.**
- Interleaved A/B with H9's game boots alongside (`shootwait-ab.txt`):
  - integ25 failed 3/3 (9, 45, 17 slow acks; max 27, 80, 130 ms);
  - the lane failed 2/3 (49, 22, 1; max 167, 60, 1.6 ms).
- Both sides fail. It goes beside the vmhold host-load flake (integrator's
  reading).

**thrstorm, interleaved under the same load** (`thrstorm-ab.txt`): integ25
1040 and 1042 rounds, lane 1004 and 1036 (−2 %); all four rc=0 with T3 stop bad=0. The
batch's lower rounds come from the host load; the paired runs match.

**P9 fp720, paired back to back** (both with H9's GPU guest alongside;
`fps.txt`):

| phase (median fps) | lane root-fp1 (load 8.7) | integ25 base-fp1 (load 7.7) | integ25 batch, quiet host |
|---|---|---|---|
| menu | 150.5 | 131.8 | 198.1 |
| loading | 149.5 | 117.4 | 171.3 |
| world | **26.6** | **25.5** | 29.8 |
| profiled stretch | **30.8** | **30.6** | 37.0 |

No fps regression. Both paired boots sit below the morning's quiet-host
numbers by the same amount: the GPU is shared with H9's guest.

The probe with 9c4b48b9 (rows only in test kernels): rootfree PASS 1/1, 40
parked, 0 unsafe. Default and test kernels both build.

## 8. Findings

| ID | symptom | rate (k/n) | first seen | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| H8-01 | an AS's page tables freed while other CPUs were still on the root (the reaper was the lowest CPU on it); the guest dies after | rootfree on the instrument-only kernel 4/5 boots (21 unsafe frees), integ25 3 `[FREERACE]` in 2/5 boots; integ22/23 gates 3 rows in 3 boots | `strix:~/cp/integ22/p6venus.1.serial:1573` | `[FREERACE] freeing root=0x20BE38000 while cpu=2 still translates through it` | kernel (x64/arm mmu.c teardown; process.c reap) | crash | fixed in 16054105 (rootfree 5/5, 0 unsafe) |
| H8-02 | a thread published READY into a group being SIGKILLed; switched to after the teardown, CR3 = 0, silent triple fault | rootfree R3: 3/5 lane boots before the fix; integ25 R3 deaths (b25.2/3/5) | `docs/cp-root-evidence/probe-lane.txt` (cap1) | `[ROOTSW0] cpu=5 tgid=494 ver=0 caller=start_scheduler+…` (with the guard; none without) | kernel (process.c `process_clone`; x64 `vm_arch_switch`) | crash | fixed in 60503d3d + 16054105 (5/5, 0 `[ROOTSW0]`) |
| H8-03 | arm: ASID-tagged TLB/walk-cache entries of a dead root could serve the next process on the same pid; TLBI issued after the L2/L3 frames were freed | analysis only (no arm compat boot) | `src/kernel/arch/arm/mmu.c` (integ25 `vm_arch_teardown`, `vm_arch_switch`) | — | kernel (arm mmu.c) | wrong output (cross-process translation) | fixed in 16054105 |
| H8-04 | `vm_arch_switch` to a torn-down AS loads CR3 = 0 (no guard) | as H8-02 | as H8-02 | — | kernel (x64 mmu.c) | crash | fixed in 16054105 (`[ROOTSW0]`, stays on the kernel PML4) |
| H8-05 | `pthread_create` EAGAIN for ~ms after groups are SIGKILLed (their slots are not reclaimed yet): the probe's child exited 3 | rootfree R3 (first probe version) 2/3 boots | `~/cp/lanes/root-ev/tf2` (`wrong-status … first 0x300`) | `R3 … wrong-status=2 (first 0x300)` | kernel (slot reclaim latency) | cosmetic (Linux returns EAGAIN under pressure too) | open (probe retries) |

## 9. What is left

- `vm_arch_root_cpu` still answers "lowest CPU". The L85 refusal and
  `slot_as_on_live_cpu` would be more exact with "any CPU but me, on either
  word". It no longer affects the frames, which the park covers; it only
  decides when a slot is reused.
- A parked root waits for the next teardown or idle loop: 264 ms at most in
  the probe. A busy machine with no teardowns and no idle CPU holds the frames
  until then. A periodic drain (for example from vmzero's nap) would bound it.
- The ARM unit image does not fit its `.bss` window on base or lane (1.6 MB
  over), so the ARM tier needs a relaxed assert to run.
- H8-05 (EAGAIN while killed slots drain) is open.
- `x64_tlb_last_targets[]` (a uint8 per CPU, shootdown diagnostics) is still a
  packed array.
- Proposed to the integrator, who is applying it at integration:
  `t3_linux_probe.sh` fails any probe whose console has `[FREERACE]`,
  `[ROOTSW0]` or `unsafe>0`.
- **Leftovers done** (separate commits):
  - 25c54a33: the RW_BLK_STRESS task naps with `process_nap_ms`.
    Compile-checked on x64 and arm with RW_BLK_STRESS=1; no gate boots it.
  - f48ad237: `vn_probe_transport`'s scanout row expects 0 or ENODEV with a
    zeroed struct. Compile-checked; not booted on Venus.

## 10. Commands (strix)

```
# one probe, reset capture (QEMU exits at a triple fault)
T3_QEMU_ARGS="-enable-kvm -no-reboot" LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh rootfree
# k runs / A-B of two trees (scripts kept in ~/cp/lanes/root-ev/)
bash ~/cp/lanes/root-ev/tfloop.sh <tree> <out> rootfree 5
bash ~/cp/lanes/root-ev/ab.sh <out> rootfree 5 <treeA> <treeB>
# the gate batch: ~/cp/lanes/root-gates.sh (integ-gates25.sh pointed at the lane tree)
```
