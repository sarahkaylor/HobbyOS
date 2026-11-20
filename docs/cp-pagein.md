# cp-pagein: a page-in that finds no frame is not an I/O error (H9-12)

Lane H10-pagein, wave 10.  Finding H9-12 in `docs/cp-hunt.md` (round 4),
evidence `docs/cp-hunt-evidence/round4/h9-12-lavapipe-oom-eio.txt`.  This
lane's evidence: `docs/cp-pagein-evidence/`.

## 1. What happened

Lavapipe P9 cold boots at 12 GiB (h4-l01, h4-l02; the same RIP both times):
the game was ~11 GiB resident during the save load (lavapipe keeps the GPU's
memory in guest RAM), it executed its own `int3`, and Wine's SIGTRAP handler
needed a page of Wine's DSO text on the NFS `/CP` stage that had not been
read yet.  The serial (strix `~/cp/lanes/hunt4/obj/hunt-logs/h4-l01.serial.txt`
L11917-L11928):

```
[KERNEL] User process fault! Vector: 3 RIP: 0x1404bcdbc
[EIO] pid=130 (Cyberpunk2077.e) page-in failed VA=0x0000001080294057 ... region=0x0000001080261000+0x0000000000060000 prot=5 kind=7 ... -> SIGBUS (EIO, not a protection fault)
[EIO] pid=130 ... VA=0x0000001080294130 ... -> SIGBUS
[LCOMPAT] fault-in-handler pid=130 sig=7 ... -> default action
[EXITPC] pid=130 last_user_pc=0x108004DE8B code=0x40000007
```

No NFS error, resend or drop row before it, and no `[TXT] UNVERIFIED` row:
the exec gate never ran, so the page source itself returned 0.

## 2. Root cause (integ27 = 6d86b0c1)

The page sources return a bare 0 for two different things.
`src/kernel/file_obj.c`, `nfs_obj_page()`:

```c
  uint64_t nf = frame_alloc();
  if (!nf)
    return 0;                     /* no frame */
  int n = nfs_read_file(m, &fh, ...);
  if (n < 0) {
    frame_free(nf);
    return 0;                     /* the read failed */
  }
```

`file_obj_page()` (FAT16) and `ext2_obj_page()` (`src/kernel/ext2_obj.c`)
have the same shape.  `src/kernel/vm.c`, `vm_file_obj_materialize()`:

```c
  uint64_t src = (o->kind == VM_OBJ_NFS)    ? nfs_obj_page(o, off)
                 : (o->kind == VM_OBJ_EXT2) ? ext2_obj_page(o, off)
                                            : file_obj_page(o, off);
  ...
  if (!src) {
    ... int beyond = (off / FRAME_SIZE) >= (uint64_t)o->nr_frames;
    if (!beyond) {
      vm_object_unref(o);
      return -EIO;                /* every 0 inside the file is "EIO" */
    }
```

The exec gate (`vm_exec_page_verify()`) conflates the same way: `f[i] =
frame_alloc(); if (!f[i]) break;` and with no read done it ends in `return
have ? 0 : -EIO`.  The fault path then:

```c
    int mrc = vm_file_obj_materialize(as, a, cow, VM_PW_FAULT_OBJ);
    if (mrc != 0) {
      if (mrc == -EIO)
        return vm_page_io_error(as, a, why);
```

and `vm_page_io_error()` sets `r->flags |= VM_MAP_IOERR` on an executable
region and returns 2; the trap prints `[EIO]` and delivers SIGBUS; every later
fault in the region answers EIO at once (M6/L69).  So a temporary frame
shortage was reported as an I/O error, and made permanent, on Wine's text.

`frame_alloc()` itself already evicts the blk cache to empty before it returns
0, so the blk-cache hooks (`frame_reclaim_to_free_frames()` /
`frame_reclaim_to_blocks()`) have nothing left to give at that point.  What
was still reachable: the zeroed frames parked for faults (each CPU's stash and
vmzero's pool, up to 64 MiB, counted as free by `frame_free_count()` but not
reachable by `frame_alloc()`), and the page cache of files nobody maps.

## 3. The fix

| where | change |
|---|---|
| `file_obj_page`, `nfs_obj_page`, `ext2_obj_page` | a new `int *err` out-parameter: `-ENOMEM` when `frame_alloc()` failed (nothing was read), `-EIO` for every other 0 (failed or short read, dead object, past capacity), 0 with a frame.  Only `vm.c` calls them. |
| `vm_exec_page_verify` | no frame for the first read (`f[0]`) is `-ENOMEM`; a missing later frame only shortens the ladder (the device re-read with `f[0]` still decides) |
| `vm_file_obj_materialize_once` (the old body) | returns `-ENOMEM` for a source or gate that found no frame (the private-copy and page-table allocations already did) |
| `vm_file_obj_materialize` (new wrapper) | on `-ENOMEM`: round 1 gives the parked frames back (`vm_pagein_unpark`: this CPU's stash, vmzero's pool), round 2 drops the unmapped FAT16/NFS page cache (`file_obj_reclaim_unmapped`: entries whose object only the cache holds, freed outside the table lock), a retry after each.  No lock held (the materialize contract), no sleep, two rounds.  A `[PAGEIN-NOMEM]` row per event (the first 16, then every 256th): `-> recovered` or `-> ENOMEM` |
| `vm_handle_fault` | a page-in still `-ENOMEM` returns `VM_FAULT_NOMEM` (3), `*why = "NOMEM"`, and marks nothing |
| x64 / ARM trap | `frc == VM_FAULT_NOMEM`: a `[PAGEIN-NOMEM] pid=… page-in found no free frame … -> SIGKILL (out of memory, not an I/O error)` row and `process_fault_exit(tf, 9)` |
| `vm_touch` | unchanged: it already answered `-ENOMEM` as -2 (the syscall layer reports EFAULT as before) |

**Why SIGKILL.**  Linux's fault path answers a page-in that cannot get memory
with `VM_FAULT_OOM`; `pagefault_out_of_memory()` runs the OOM killer, whose
answer is SIGKILL to a victim, and the faulting task retries if it was not the
victim.  It never answers SIGBUS and never poisons the mapping.  This kernel
has no OOM killer that picks a victim, so the least invasive honest answer is
the faulting process itself (in H9-12 it is also the process holding ~11 GiB,
the one Linux would pick).  SIGKILL rather than the anon path's "OOM" SIGSEGV
because SIGSEGV is catchable: Wine would turn it into an access violation in
the game, and a handler that needs the very page would fault again.  The
alternative "retry the instruction until memory appears" has no bound without
an OOM killer.

**Unchanged.**  A real read failure (short read, failed RPC, the exec gate's
refusal of zeros or disagreeing reads) is still `-EIO` -> `VM_MAP_IOERR` ->
`[EIO]` -> SIGBUS/BUS_ADRERR, M6/L69 and M6/L63 exactly as before (unit step
5, probe P4).  A page past EOF is still `-ENXIO` -> SIGBUS (C1-k4).

## 4. Anon and COW paths (checked, not changed)

No EIO conflation there: an anonymous fault that finds no frame
(`vm_zframe() == 0`), a COW break (`vm_cow_page_locked`,
`vm_cow_share_break_locked`), a private fill over a memfd
(`vm_obj_private_fill_locked`) and the native IMAGE path
(`v2_image_materialize`) all answer `*why = "OOM"`, return 1, and mark
nothing.  But that answer is `process_fault_exit(tf, 11)`, a SIGSEGV a compat
process's handler can catch (Linux: SIGKILL from the OOM killer).  Same OOM
class, different path: reported, not changed (section 7).

## 5. Reproduction (both seen failing on the base and passing with the fix)

**Unit:** `test_vm_exec_page_nomem` (`src/kernel/vm_test.c`).  Takes every
free frame for real (held in a chain of index frames), faults an exec
MAP_PRIVATE page of `/VMOBJ1.DAT`, touches it from the syscall side, parks 16
frames in vmzero's pool and faults again, gives the memory back and faults
again, then injects a zero delivery.

| | base integ27 (test only) | lane |
|---|---|---|
| exhausted | held=2932671 free_at=4 | held=2932675 free_at=0 |
| fault under the shortage | **2 why=EIO ioerr=1** | 3 why=NOMEM ioerr=0 |
| syscall touch | -1 | -2 |
| parked frames reclaimed, same fault | - | 0, bytes correct (`unparked=16 rounds=1 -> recovered`) |
| memory back | - | 0, bytes correct |
| real read error (inject zeros) | - | 2, ioerr=1 |
| result | FAILED `(frc1 != 2) == 1` (465 run, 1 failed) | 465 run, 0 failed |

**Probe:** `src/user/linux_probe_pagein.c`, `tools/gate/t3_linux_probe.sh
pagein`, console graded by `tools/gate/lp_check_pagein.py`.  Child A takes the
12 GiB guest (11407 MiB: memfd writes, then one-byte preads into fresh
anonymous pages until the kernel refuses), then executes an unread page of a
FAT16 file `PGIN.BIN` (PROT_READ|PROT_EXEC, MAP_PRIVATE); if alive it releases
the memory and executes page 5 of the same mapping.  P3 maps the file again and
runs every code page; child C executes the all-zero page (the gate's EIO).

| row | base | lane |
|---|---|---|
| P1 under the shortage | SIGBUS on page 0, `[EIO] pid=3 … -> SIGBUS` | `[PAGEIN-NOMEM] n=1 … rounds=2 free=24 -> ENOMEM`, `[PAGEIN-NOMEM] pid=3 … -> SIGKILL`, child status 0x40000009 |
| P1 same mapping, memory back | **SIGBUS again** (`[EIO] … VA=…5000`: the region was marked) | (the process was OOM-killed) |
| P3 new mapping, memory back | 7/7 pages run | 7/7 |
| P4 zero page | SIGBUS/BUS_ADRERR, page 1 refused too | same |
| verdict | `[LP] done rc=2`, checker FAIL | `[LP] done rc=0`, checker PASS |

`free=24` in the lane row: frames parked in the other CPUs' stashes (counted
free, unreachable from this CPU).

## 6. End-to-end: the H9 recipe at 12 GiB (strix, lavapipe)

`HUNT_GPU=lavapipe P9_MEM=12288 HUNT_SCRIPT=tools/cp/hunt/r4/cold-lvp-m86.p9
bash tools/cp/hunt/run-hunt.sh script pagein-l0N`, tree cp-pagein (b7a6cb1d +
3e566122), no big-guest lock.  Evidence `cp-pagein-evidence/e2e-lavapipe-12g.txt`.

| | h4-l01 / h4-l02 (H9, integ25) | pagein-l01 | pagein-l02 |
|---|---|---|---|
| main menu / loading start (QEMU start = 0) | menu 179.1 / 285.2 s | 179 / 205 s | 216 / 235 s |
| the game's own `int3` at RIP 0x1404bcdbc | game s 941 / 1008 | yes | yes |
| Wine's handler page-in (VA 0x1080294000, region 0x1080261000+0x60000, NFS) | `[EIO]` x2 -> SIGBUS, region marked | `[PAGEIN-NOMEM] n=1 … unparked=5537 rounds=1 -> recovered` | `[PAGEIN-NOMEM] n=1 … unparked=2750 rounds=1 -> recovered` |
| `[PAGEIN-NOMEM]` rows (all `-> recovered`) | - | 11 | 16 |
| `[EIO]` rows / SIGBUS from a shortage / `-> SIGKILL` OOM kills | 2 / yes / - | **0 / 0 / 0** | **0 / 0 / 0** |
| how the game ended | SIGBUS in the handler, exit 0x40000007, status 7 | the breakpoint goes unhandled: Wine tries `winedbg --auto` (`process_create: no free physical memory blocks!` x2505, `fork: AS clone failed`, `start_debugger Couldn't start debugger`), the game exits 0x80000003, status 768 at 1006.5 s, REDEngineErrorReporter ran | same: x2004, exit 0x80000003, status 768 at 1061.7 s |

So the kernel defect is gone: the page Wine needed was reclaimed and mapped
both times (vmzero's parked frames: 2 750-5 537 per event, round 1 always
enough), Wine's SIGTRAP handler ran, and nothing was marked.  The game then
died of its own unhandled breakpoint with the guest still out of memory -- a
different failure, recorded and not chased: a 12 GiB guest is too small for
a lavapipe save load (H9: 32 GiB goes through).  One more kernel row in
pagein-l01: REDEngineErrorReporter (pid 129) died by an anonymous-fault
`in=OOM` (the anon path of section 4, unchanged).  No 32 GiB control was run.

## 7. Open

- The anon / COW / private-fill / IMAGE "OOM" answer is a catchable SIGSEGV;
  Linux's is SIGKILL.  The same `VM_FAULT_NOMEM` answer would fit them; not
  this lane's path.
- NFS read-ahead windows (`nfs.c`, up to 4096 x 64 KiB = 256 MiB of frames on
  x86; h4-l01's `[NFSRA] … wins=4096`) are not given back under pressure.  A
  round-3 `nfs_ra_reclaim()` (free idle windows' frames) would be the next
  lever; `nfs.c` is not this lane's and needs both NFS guards.
- `file_obj_note_write()` (cp/pfx coherence) and `ext2_obj_writeback_locked()`
  silently skip their work when their scratch `frame_alloc()` fails.  Not an
  EIO conflation, but a shortage that leaves a mapped page stale / defers a
  write-back without a row.
- ext2's cache-only objects are not in round 2 (`ext2_obj_reap_locked` needs
  the ext2 lock and may write back).
