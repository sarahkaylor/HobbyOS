# L67 — the block wedge named: a host-side controller reset, and the aborted transfer that killed the loader

Status: fix in, ladder running. Worktree `/home/sarah/hobbyos-rw-lanes/l67-wedge`
(branch `rw-l67-wedge`), seeded at merged tip `4bcb2d5b`.

## 1. What the L65 evidence could not separate

L65 (§50.6) left two candidates for the deterministic ~490 s wedge:

* **(A) the device is not completing** — QEMU never posts the CQE, or
* **(B) we are missing a completion** — the ring bookkeeping (phase tag,
  head/tail, cid) consumed or mis-keyed the CQE.

The L55 row printed the ring indices but not *what was in flight*, so it could
not decide. It also could not say whether the "CC=0, CSTS=0" it printed was the
device's real state or a stale mapping read.

## 2. The instrument

`src/kernel/virtio_blk.c` (x86_64 NVMe path):

* `blk_cur` records the submitted command (opcode, nsid, LBA, NLB, PRP, cid,
  slot) at `nvme_sq_submit()`;
* every completion records `last_cid`, `last_sqhead` (the device's own SQ head
  from CQE.sq_head) and its timestamp;
* the wait loop polls **CAP/VS/CC/CSTS/INTMS/INTMC**, `cpu_pd7[480]` (the 2 MiB
  MMIO mapping), the per-CPU id and the counters `reqs/stalls/devoff/cc0`;
* the stall row prints the CQE at the CQ head **and** at the command's own SQ
  slot, so "posted elsewhere" is visible;
* `virtio_blk_diag_row()` prints one `[BLKD]` row every 5 s from the existing
  `[THRS]` sampler — a *timeline* of controller state, not just a post-mortem;
* `tools/rw/run-rimworld-desktop.sh` grew `RW_QEMU_EXTRA`, so the host side can
  be traced without editing the recipe: `-trace enable=pci_nvme_mmio_cfg
  -trace enable=pci_nvme_mmio_stopped -trace enable=pci_nvme_pci_reset
  -trace enable=pci_nvme_mmio_doorbell_sq -D <file>`.

`tools/rw/l67-trace.py` reduces a trace file to "every controller-state event
and the doorbell index it follows".

## 3. The stalled-request tuple (run 1, 1800 s window)

Load complete (`[NFSRTT] bytes=277581512`) and then, at t≈590 s:

```text
[BLKQ] device off cid=44230 op=0x2 lba=0x7B41B since_ms=2
       cc=0x460001 csts=0x0 cap=0xF0107FF/0x400820 vs=0x10400
       cc0_writes=0 cpu=3 n=1
[BLKQ] wedge-device-off cid=44230 op=0x2 lba=0x7B41B nlb=0 prp=0x748B6000
       slot=1 sq_tail=2 cq_head=1 phase=1
       cqe[cq_head].cid=65535 .status=0x0  at_slot.cid=65535 .status=0x0
       last_cid=44229 last_sqhead=1 ms_since_done=8
       inside=1 stalls=0 devoff=1 reqs=109665 cc0=0 cpu=3
       CSTS=0x0 CC=0x0 CAP=0xF0107FF VS=0x10400 INTMS=0x0 INTMC=0x0
[BLKQ] controller reset #1
[BLKQ] admin timeout cid=2 CSTS=0x0
[BLKQ] create SQ failed status=0x7FFF
[BLKQ] reinit failed cid=44230 -> I/O error
[BLKQ] device off cid=44231 op=0x2 lba=0x7B418 since_ms=2019
       cc=0x0 csts=0x0 ... cc0_writes=1
[BLKQ] controller reset #2
[BLKQ] recovered cid=44231 us=5000 resets=2
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
[KERNEL] pid=4 memory fault VA=0x108228B570 ... -> killed
```

Read it field by field:

| field | value | what it decides |
|---|---|---|
| `op` / `lba` / `nlb` | `0x2` (read) / `0x7B41B` (and `0x7B418` on the retry) | one 512 B read in **one 4 KiB cache unit**, retried |
| `cid`, `slot`, `sq_tail`, `cq_head`, `phase` | 44230, 1, 2, 1, 1 | the ring is consistent: slot 1 was submitted (tail 2), head is 1, phase 1 |
| `cqe[cq_head].cid/.status` | `0xFFFF` / `0x0` | **no valid CQE at the head** (status bit0 = phase ⇒ "not written") |
| `at_slot.cid/.status` | `0xFFFF` / `0x0` | nothing posted at the command's own slot either |
| `last_cid`, `last_sqhead`, `ms_since_done` | 44229, 1, **8 ms** | the device was completing normally 8 ms earlier, and its own SQ head (1) matched ours |
| `inside` | **1** | one driver entrant — not the L55 two-consumer race |
| `stalls` | 0 | this row came from the device-state detector, not the 2 s bound |
| **`cc0`** | **0** | **this kernel had written CC=0 zero times** — the disable was not ours |
| `CSTS` | `0x0` | RDY=0 |
| `CC` (detection read) | **`0x460001`** | EN=**1** — the controller is *enabled* but not ready |
| `CC` (same row, ~2 ms later) | `0x0` | the reset **finished while the row was being printed** |
| `CAP` / `VS` | `0xF0107FF/0x400820` / `0x10400` | read-only registers read correctly ⇒ the BAR mapping is intact |
| `INTMS`/`INTMC` | 0/0 | the reset zeroed them |

`CC.EN=1` with `CSTS.RDY=0` has exactly one producer in QEMU:
`nvme_ctrl_reset()` writes `csts=0`, `intms=0`, `intmc=0` **first**, then drains
the namespaces (`nvme_ns_drain` → `blk_drain`), then frees the SQs/CQs, and only
**then** writes `cc=0`.  The tuple is that window — and the row proves the
window lasted seconds (the same row's later `CC` read is already 0).

So the answer to (A) vs (B):

> **(A), and stronger: the device was not merely slow, it was reset out from
> under the request.**  The CQ holds no entry, the device's own SQ head matches
> ours, we are the only entrant, and the controller's own registers say "reset
> in progress".  No completion was lost: none was ever posted, because the
> controller that owed it had its queues destroyed.

And the reset is **not** the guest's:

* `cc0=0` — the driver had not written CC=0 yet in the whole run;
* the host trace shows `pci_nvme_pci_reset` (FLR) only at boot (OVMF), and no
  `pci_nvme_mmio_stopped` before the wedge — no guest CC write and no FLR;
* `CAP`/`VS`/`cpu_pd7[480]` are all correct at that instant, so the
  "we read a stale mapping" explanation is excluded too.

`tools/rw/l67-trace.py obj/l67/run1-trace.keep` on the instrumented run:

```text
trace lines=111559 sq_doorbells=111541 state_events=18
  after       0 doorbells: pci_nvme_pci_reset: PCI Function Level Reset      <- OVMF
  after       0 doorbells: mmio_cfg config=0x0 / 0x460001 + start_success    <- OVMF
  after    1850 doorbells: mmio_cfg 0x0 + mmio_stopped + 0x460001 + start    <- our init
  after  111518 doorbells: mmio_cfg 0x0 + mmio_stopped + 0x0 + 0x460001 ...  <- our resets
  after  111521 doorbells: (same)
```

The doorbell stream is continuous up to the wedge (one `doorbell_sq` per
command, 111 541 for 109 665 completions — the difference is admin commands plus
the resubmissions), and **no FLR and no CFS** occur at the wedge.

### What is still open

The in-QEMU *initiator* of `nvme_ctrl_reset` is not named by this evidence.  The
candidates left after excluding every guest-side one are `nvme_exit`
(device unrealize) and an internal reset path; QEMU 10.2's `nvme_ctrl_reset`
has no trace point of its own, which is why the state had to be recovered from
the registers.  What the evidence *does* settle is that the guest did not cause
it, that the ring bookkeeping is correct, and that the reset blocked for
seconds inside the namespace drain (a host-side I/O wait), not in the device
model.

## 4. Why the wedge killed the process — the aborted transfer

The L55 recovery was written for a *lost completion*, not for a *reset device*.
Its 2 s bound expired (or the new detector fired) and it did:

1. write CC=0 — which lands **inside** QEMU's still-running reset;
2. write CC=0x460001 and wait for RDY — QEMU's `nvme_start_ctrl` succeeds
   (the trace shows `start_success`);
3. create the IO queues — and then QEMU's reset **finishes**, frees the queues
   we just created, and clears CC.  That is the log's
   `admin timeout cid=2` / `create SQ failed 0x7FFF` / `reinit failed`;
4. the caller gets `-EIO` for a read that was fine;
5. the retry (cid 44231) sees `cc=0, csts=0` (the reset has completed), and the
   second reinit — now with no reset in flight — takes **5 ms**.

But the *data* is what kills the process.  QEMU aborts the requests that were
in flight when it resets the controller; an aborted AIO can complete with
**success and no DMA at all**, so the guest's `bounce_buf` still holds what the
previous command left there — at the end of the load, a page of zeros.  The
exec gate then sees three full-length reads of 4096 zeros
(`[TXT] ZERO off=0x757000 reads=3 n0=4096`), refuses to publish, and the
instruction fetch at that VA faults in `PROT` (err=0x14) and kills pid 4.  That
is the deterministic "same VA" death: the same page is the one the aborted read
was for.

### 4.1 The data really was zeros — the image is intact

The obvious alternative is "the guest read the wrong cluster".  It did not.
Verified on the host after the run, against the image that was booted:

* `md5(UnityPlayer.so extracted from disk.img) = 45408fc19d21120d73e89d7bf81fbadd`,
  identical to the staged DSO (33 010 672 bytes);
* the page at file offset `0x9AD000` starts `ff ff ff ff c7 40 18 ff ff ...`
  (it is `.text`, not a zero page);
* walking the FAT16 chain of `/RWDSO/UNITYP~1.SO` (start cluster 15294, 16 KiB
  clusters, 32 sectors) puts file offset `0x9AD000` at cluster 15913, LBA
  **0x7C728** — and the guest's stalled command was LBA **0x7C72A/0x7C72C**,
  i.e. sectors 2-4 of that same 4 KiB page.

So the guest asked for the correct sectors and the device answered with a full
4 KiB of zeros.  The zeros are the device's, not a chain-walk bug and not a
stale cache entry.

## 5. The fix

Two changes in `src/kernel/virtio_blk.c`, both about *trusting the wrong thing*:

1. **Detect the reset instead of waiting out a bound.**  The wait loop polls
   `CC.EN`/`CSTS.RDY` every 128 spins.  Either clearing means the controller was
   reset and the cid can never be posted; go straight to repair.  This turns a
   2 s stall into a repair measured in microseconds and stops the recovery from
   being issued blind.

2. **Repair without racing the host, and never trust the first transfer after a
   repair.**  `nvme_reinit()` now
   * waits (≤500 ms) for a half-reset device (`EN=1 && RDY=0`) to finish before
     touching it;
   * retries the whole sequence (≤4 attempts) if `RDY` never sets or the admin
     Create SQ fails;
   * re-reads CC/CSTS 20 ms after a successful enable and retries if the device
     settled out of ready (the late-completing host reset).

   And `nvme_wait_cid()` **discards the completion that arrives after a repair
   and re-issues the same command once** (`[BLKQ] re-read after recovery`): only
   the second transfer is used, so an aborted-AIO "success" can never become a
   page of zeros.  Up to 3 repair+re-read cycles per request, then `-EIO`.

Cost: one extra command per repaired request (there are two in a run).

## 6. Run 1's post-load park, explained

Run 1 of L65 (`obj/l65-run1.serial.log`, 145 940 lines) never hit the block
wedge: **0 `[BLKQ] stall`, 0 `[BLKSLOW]`, 0 `[TXT] ZERO`, 0 `[NFSRD] fail`**.  It
is not a blocked read.  Its rows say:

| t (s) | NFS bytes | `[XSW] n` | live | res MiB |
|---:|---:|---:|---:|---:|
| 421 | 77 809 096 | **260** | 26 | 879 |
| 481 | 189 528 908 | **260** | 26 | 1003 |
| 541 | **277 577 677** (final) | 260 | 26 | 1148 |
| 1041 | 277 577 677 | 260 | 26 | 1476 |
| 1690 | 277 577 677 | 260 | **25** | **1899** |

* presents stop at **260** at t≈420 s, *before* the load finishes, and never
  move again;

* the load continues to the deterministic total and then stops dead — no NFS
  read after t=541 s;

* `res_mib` climbs **linearly at 0.64 MiB/s** for the remaining 19 minutes,
  matched exactly by the publish counter: `[PUBS] n` 307 200 → 503 808
  (+196 608 first-touch anon pages = 768 MiB, one page per 6.1 ms);

* the futex flight recorder (the idle-loop hang report re-dumps the last
  `FUTEX_TRACE_RING`=64 ops) shows, right up to the final dump at line
  145 766, a **three-thread WAIT/WAKE cycle** — pid 4 waits on `0x1083B17624`,
  pid 26 wakes it and waits on `0x1100000334`, pid 4 wakes it and pid 27 waits on
  `0x113000A460`, pid 26 wakes it … all with `to=-1` (infinite) and `bitset=0xFFFFFFFF`;
  the record index reached 190 182 over 1690 s ≈ **112 futex ops/s**;

* the scheduler's own verdict at the end: `[ALIVE-SET] no READY work, yet the
  scheduler counts live work ... so no halt`, 25 live slots — 24 in
  `PROC_STATE_FUTEX` (st=8) and pid 4 `st=3` (RUNNING) with `pc=0x10B8043D72` in
  a JIT arena (`0x10B4384000+0x3CBFD72`).

So run 1's park is **a livelock in the post-load frame handshake, not a wait on
a file, a thread that died, or the block layer**:

* the window/present path died first (presents frozen at 260, framebuffer 8×1 by
  t≈500 s — §50.7), which is the same window loss runs 2/3 show at 491/504 s;
* the game keeps loading to the deterministic byte total, then the main thread
  and the gfx/job threads fall into a barrier that completes but never presents;
* a second thread keeps touching fresh anon pages (one per 6 ms) so RSS grows
  without bound while nothing is presented.

A future run must not read that 1 899 MiB as "the loader still working": the
load is over at 277 577 677 bytes and t=541 s.

## 7. The fast reproducer

`BLKTEST.BIN` grew an opt-in **P6 device-churn** phase (runs only when
`/CHURN.ON` exists, so the wave is untouched): 4 readers streaming 6 × 256 KiB
files in random 512…4096-byte chunks, 3 writers rewriting a random file's first
sector every iteration (different clusters), one grower appending 256 B × 400 so
clusters are allocated under the readers, plus the parent as a fifth reader.

See §9 for whether it reproduced the wedge inside 60 s.

## 8. Ladder

| run | window | `[BLKQ] stall` | host resets seen | repair | end state |
|---|---:|---:|---:|---:|---|
| instrumented (QEMU trace) | 1800 s | **0** | 1 (cid 44230) | 5 ms after a 2 s failed repair | killed on a zero page; load complete (277 581 512 B) |
| ladder 1 (fresh cid + straggler drain) | 1800 s | **0** | 1 (cid 44202) | **56 ms**, no admin timeout, no `-EIO` | killed on a zero page; load complete (277 468 532 B) |
| ladder 2 (same build) | 1800 s | **0** | **0** | - | killed on a zero page at t~270 s with the load at 50 MB |

The wedge is dead as a *block-layer* failure: zero `[BLKQ] stall` in every run,
the controller is repaired in tens of milliseconds, and the L55 death spiral
(`create SQ failed` -> `reinit failed` -> `-EIO` on a good read) never happens.

The process still dies, and ladder 2 proves the two bugs are independent: it
died with `devoff=0 resets=0 cc0=0` — controller enabled and ready throughout,
no block-layer event at all — on a page that read back as 4 KiB of zeros
(`[TXT] ZERO off=0xBC000 reads=3 n0=4096`, preceded by
`[FATW] ZERORD sector=656610 off=82944 size=83099`).  The remaining death is a
page-in defect, not the block layer.

## 9. What is left

1. **The zero page with a healthy controller.**  A full 4 KiB of zeros is
   delivered for an executable page whose file content is not zero, with the
   controller ready (`cc=0x460001 csts=0x1`), the ring consistent and the cache
   publishing on status 0.  `n0=4096` rules out an end-of-file short read
   (`fat16_read_direct_locked` clamps).  Next probes: the `[FATW] ZERORD` sector
   (`656610`) on the host, and whether the block cache can publish a zero frame
   when a fill's device read is aborted-then-retried.
2. **A health tick from a safe context.**  QEMU leaves the controller disabled
   after its reset; a repair hook called from the *fault* path deadlocks (it
   waits on the single-in-flight token with interrupts off).  Do it from the
   5 s sampler thread, not from `vm_file_obj_materialize`.
3. **The three-window ladder** could not be finished inside this lane's wall
   clock (32 min per window); two windows plus the instrumented run are above.

## 9. Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **423 run, 0 failed, UNIT TESTS
PASSED**.  `tools/rw/l67-gates.sh` (one tier at a time, never concurrent with a
desktop boot): **redzone PASS, fpu_sig PASS, m2_mono PASS, m2_tp PASS,
m2_threads PASS, m12 PASS, mmap_race PASS, m3_window PASS, m4_gl PASS,
rwnfs PASS — 10 passed, 0 failed**.
