# L75 — the "host controller reset" was **our own page fill**: the frame pool
# overlapped the 32-bit PCI hole, and the NVMe BAR lives in it

Lane `l75-blkreset`, worktree `~/hobbyos-rw-lanes/l75-blkreset` (branch
`rw-l75-blkreset`, seeded at the merged tip `913d80a0`). Question: why does
QEMU reset the NVMe controller every ~730 s, and what does a clean recovery
need to be?

## 0. Summary

| question | answer |
|---|---|
| does QEMU reset the controller? | **no.** QEMU 10.2's `nvme_ctrl_reset()` has four callers: a guest `CC.EN` 1→0 write, a PCI FLR, an SR-IOV state change, and unrealize. The run's trace has **one** `pci_nvme_pci_reset` (OVMF, at boot) and **four** `pci_nvme_mmio_cfg` writes — `0x0`, `0x460001` from OVMF, then the driver's own boot `0x0`+`0x460001`. Nothing else ever writes `CC`. |
| what resets it, then? | **a 4 KiB page fill into a frame the allocator handed out.** The x64 frame pool's extent 1 started at physical `0x80000000` — the base of the 32-bit PCI MMIO hole on `-M q35 -m 12288M` — and QEMU places the NVMe BAR at `0x81000000`. Frame #327,936 of the pool *is* the controller's register file. |
| why ~730 s? | that is when the allocator's forward walk reaches it. Extent 0 is 1.25 GiB = 327,680 frames; measured rate 212 frames/s at t=320 s puts frame 327,936 at ≈770 s, and every L72/L73 1800 s window ended at 730-740 s. |
| why zeros and torn pages? | the frames in the hole are not RAM. Reads return the device's register values (or all-ones from unassigned space), consistently, so the block cache publishes them as valid pages: `[TXT] ZERO … n0=4096`, `[TXT] IMG SUM … raw sum=0x76EFDDC5` (the FNV of 4096 zeros). |
| the fix | pool extent 1 moved to 4-8 GiB (all RAM, identity-mapped by PD4..PD7); the 2 MiB identity shadow of each kernel MMIO window reserved; a boot probe plus a permanent `hole=` tripwire; and `nvme_reinit()` is now a **full controller re-initialisation** — CAP/VS re-read, `CC` reprogrammed with a read-back check, admin + IO queues recreated, namespace revalidated by Identify — with the cache flush moved to the front so it happens on every reset, successful or not. |
| result | two 1800 s windows: **0 unplanned resets**, no `device off`, no `stall`, no `[TXT] ZERO`, no `[TXT] IMG SUM`, no `[EIO]`, no exec page published as zero, `hole=0` throughout, and `fr_used` walked **past** 327,680 and 369,000 (the old trigger) with nothing happening. The only reset in each window is the lane's deliberate full-re-init self-check at t=240 s, whose four checksums are identical. |

## 1. The evidence that it is not the host

### 1.1 QEMU's memory map has no RAM where the pool put extent 1

```text
$ qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 12288M -S -monitor stdio
(qemu) info mtree -f
  0000000000100000-000000007fffffff (prio 0, ram): pc.ram @0000000000100000
  0000000100000000-000000037fffffff (prio 0, ram): pc.ram @0000000080000000
```

RAM is `[0x100000, 0x80000000)` and `[0x100000000, 0x380000000)`. Everything in
`[0x80000000, 0x100000000)` — 2 GiB — is the 32-bit PCI MMIO hole.

### 1.2 The pool started exactly there, and the BAR is inside it

Old extent (`src/kernel/frame.c`, before this lane):

```text
[FRAME] frames=1376256 total_mib=5376 ext0=0x20000000-0x70000000 ext1=0x80000000-0x180000000 reserved=0
[BLKQ] mmio win=0x81000000-0x81200000 bar_in_pool=1 alias_read=0x0F0107FF cap=0x0F0107FF
```

`alias_read` is `*(uint32_t *)0x81000000` through the identity map and it equals
the controller's `CAP`. The pool covers the page; the page *is* the controller.
(The lane's boot row now prints exactly this pair, so a future extent edit that
reintroduces the overlap says so at boot.)

### 1.3 A page fill disables the controller — the reproducer

`RW_ALIAS_PROBE=1` writes a 4 KiB zero page through the address the allocator
would hand out, then reads the registers back:

```text
[BLKQ] alias-probe pa=0x0000000081000000 cc_before=0x0000000000460001 csts_before=0x0000000000000001
[BLKQ] alias-probe wrote 4 KiB of zeros (32-bit stores) through the pool frame;
       cc_after=0x0 csts_after=0x0 pa_cc_read=0x0 pa_cap_read=0x0F0107FF bar_cc_read=0x0F0107FF
```

`CC` went `0x460001 → 0x0` and `CSTS` `0x1 → 0x0` — `nvme_ctrl_reset()`'s own
signature (`csts=0, intms=0, intmc=0`, then `cc=0`) — and the driver's `cc0=`
counter was still **0**: this kernel never issued that write. It was a plain
memory store into a frame. (`obj/l75/alias-probe.log`.)

Two details that matter for anyone repeating this:

* 8-byte stores at `0x14` do **not** land: `nvme_write_bar()` honours 32-bit
  accesses there and only logs `pci_nvme_ub_mmiowr_*` for the rest. The first
  version of the probe used 8-byte stores and read back unchanged; the
  page-cache `memcpy` uses 4-byte stores, which is why *it* hits the register
  file.
* `alias_read == cap` is a read-only proof and is printed on every boot, so the
  overlap is visible without a destructive probe.

### 1.4 The rows this explains

* L67 §3's tuple: `cc0_writes=0`, `CC=0x460001`, `CSTS=0x0`, `CAP`/`VS`
  readable — "a reset in progress that this kernel did not ask for", with the
  guard that `cc0=0` proves the write was not ours.
* L73 run C's post-mortem re-init: `reset: waiting out a host reset,
  CC=0x6D697861 CSTS=0x2`. `0x6D697861` is ASCII `axim` — the register file
  was holding file bytes, because a page of file data had been written into the
  frame that *is* the register file.
* L69's poisoned cache frame (`fstate=0`, `valid=0xFF`, `c_nz=0`: a fully
  valid, all-zero frame) and L73 run B's exec gate publishing a zero page as
  verified — a frame in the hole reads back device state, consistently.
* The 730-740 s end of every window: `fr_used` crosses 327,680 (PA
  `0x80000000`) and then 327,936 (PA `0x81000000`, the BAR) exactly there.

## 2. The fix

### 2.1 The pool is RAM and only RAM (`src/kernel/frame.c`, `frame.h`)

* x64 extent 1 is `[0x100000000, 0x200000000)` (4-8 GiB): all RAM, all
  identity-mapped by `PD4..PD7`, and the pool size is unchanged (5.25 GiB).
* `frame_init()` probes each extent at three addresses (base, middle, last
  frame) with a write/read-back and **refuses the whole extent** if a probe
  fails. `[FRAME] ram ext0=1 ext1=1` is the row.
* The 2 MiB identity shadow of each kernel MMIO window (NVMe at
  `VA 0x1FC000000` = PD7[480], virtio-gpu at `0x1FE000000` = PD7[496..497]) is
  reserved: installing a window there *removes* the identity mapping of that
  physical 2 MiB, so a frame in it would be reachable only through the device.
* `frame_hole_allocs()` counts any allocation that lands in
  `[0x80000000, 0x100000000)`; the 5 s row prints it as `hole=` and
  `test_frame_pool_is_ram_only` pins the invariants.

### 2.2 Full controller re-initialisation (`nvme_reinit`)

The old reset path re-enabled the controller and recreated the queues. It now
rebuilds the controller from the registers up, in this order, printing each
step (a real mid-run sequence, from `obj/l75/serial-L1.log`):

```text
[BLKQ] controller reset #1
[BLKC] post-reset flush dropped=13198 cached units
[BLKQ] reinit cap=0x000000000F0107FF/0x0000000000400820 vs=0x0000000000010400 mqes=2048 dstrd=0 mps=0/4 sq_db=0x00000001FC001008 cq_db=0x00000001FC00100C
[BLKQ] reinit cc=0x0000000000460001 csts=0x0000000000000001 aqa=0x0000000000010001 asq=0x0000000074914000 acq=0x0000000074912000
[BLKQ] ns nsid=1 nsze=0x0000000000200000 lbads=9 flbas=0 vid=0x0000000000001B36/0x0000000000001AF4
[BLKQ] reinit ok resets=1 nsze=0x0000000000200000 lbads=9 qd=16
```

| step | what it proves |
|---|---|
| flush every cached unit **first** | no frame filled across a reset can be published, even if the rebuild below fails (L69's rule, now unconditional) |
| wait out a host reset in progress (`CC.EN=1 && CSTS.RDY=0`, bounded) | L67's half-reset defence |
| re-read `CAP`/`VS`, derive `MQES`, `DSTRD`, `MPSMIN/MAX`, re-derive all four doorbell pointers | the geometry is the device's, not a boot-time constant |
| `CC=0`, wait `RDY=0`, program `AQA` + `ASQ`/`ACQ` + `CC` (`EN`, `MPS=CAP.MPSMIN`, `IOSQES=6`, `IOCQES=4`), verify `CC` reads back exactly | the register block is ours; a mismatch is a hard failure |
| zero both rings, recreate CQ 1 / SQ 1 | the reset freed the queue objects and the old doorbells point at nothing |
| Identify CNS=0 (nsze, flbas, LBA format) + CNS=1 (controller id) | the namespace the driver re-issues into is the one the controller reports |
| stay ready for 50 ms | a late host reset cannot hand back a disabled controller |

`CC`'s field layout is derived once (`EN` bit 0, `CSS` 6:4, `MPS` 10:7, `AMS`
13:11, `SHN` 15:14, `IOSQES` 19:16, `IOCQES` 23:20) instead of a literal, and
`CAP.MPSMIN/MAX` are read from the **high** word — reading them from the low
word asks for 8 KiB pages, and QEMU answers `nvme_start_ctrl` with `CSTS.CFS`
(measured, §2.3).

### 2.3 The data path across a reset

`[BLKQ] selfcheck` (once per boot, t=240 s, from the 5 s sampler thread):
512 KiB of a never-written region are checksummed through the block cache and
raw, then the FULL re-init is forced, then both are checksummed again.

```text
[BLKQ] selfcheck lba=0x00000000001FE000 n=1024 cache1=0xFC31BFF590C22325 cache2=0xFC31BFF590C22325 raw1=0xFC31BFF590C22325 raw2=0xFC31BFF590C22325 reinit_rc=0 ms=133 match=1
```

All four sums identical: the cache did not serve a frame that straddled the
reset (it was flushed and re-filled from the device), no DMA was aborted, and
the re-created queues carried real transfers.

### 2.4 The instruments this lane added

* `[BLKQ] mmio cc= csts= intms= intmc= asqt= acqh= was_cc= was_csts= n= cpu= ms=`
  — one row per observable controller state change, sampled at the 100 Hz timer
  tick on CPU 0. A `CC` transition with no driver reset behind it is the
  signature of a foreign writer (it is what the L65-L73 logs lacked).
* `fr_used= fr_high= hole= mmio=` on the 5 s `[BLKD]` row — the frame pool on
  the same timeline as the device.
* `[BLKQ] selfcheck …` (above) and `[FRAME] ram ext0= ext1=` /
  `[BLKQ] mmio win=… bar_in_pool=… alias_read=… cap=… hole_allocs=…` at boot.
* `tools/rw/l75-qmp.py` — the host side of the question: `query-status`,
  `query-block`/`query-blockstats` (a host-side reset or block error appears
  there; a guest `CC` write does not), `info pci`, `info mtree`.
* `tools/rw/l75-summary.py` — one run → the lane's numbers (resets, re-init
  rows, zero/torn refusals, presents, `fr_used`, the selfcheck row).
* `tools/rw/l75-ladder.sh` — the acceptance windows, sequential.
* `RW_ALIAS_PROBE=1` — the one-off reproducer (§1.3), never in a shipped build.

### 2.5 The block-stress probe — implemented, but not shipped

`RW_BLK_STRESS=1` (harness `RW_CHURN_SOAK=1`) compiles a kernel thread that
does 24 random 512…4096-byte reads plus three write-verify cycles (read a
sector, write the same bytes back, verify) every second over the free tail of
the namespace. It ran: `reads=1752 write_verify=219 bad=0 mismatch=0`
(`obj/l75/` runs). It is **not in the shipped build**: at ~70 s into the game's
load the thread stops making progress while parked at one pc, and the console
stops with it. An earlier version driven from the 5 s census wedged the machine
outright at t=65 s (the census runs from the scheduler with its locks held).
Both are recorded here rather than papered over: the ladder windows below were
run **without** the probe, and their concurrent load is the game's own 240 k
device commands plus the selfcheck's 512 KiB of cache+raw reads and its forced
re-initialisation.

## 3. Ladder

`tools/rw/l75-ladder.sh 1 2`: two 1800 s windows, sequential,
`--timeout 600 --run-timeout 1800`, `RW_WAIT_SECS=1900`, QEMU trace on
`pci_nvme_mmio_cfg`, `pci_nvme_mmio_stopped`, `pci_nvme_mmio_start_success`,
`pci_nvme_pci_reset`, `nvme_error`, `pci_nvme_err_*`, `pci_nvme_ub_*`.

| run | window | resets | of which self-check | `device off` / `stall` | `[TXT] ZERO` / `IMG SUM` / `[EIO]` | max `fr_used` | presents | `[XO]` | end state |
|---|---:|---:|---:|---|---:|---:|---:|---:|---|
| L1 | 1800 s | 1 | 1 | 0 / 0 | 0 / 0 / 0 | 369 734 (`fr_high` 494 978) | 1180 | 1207 | game alive: pid 4, 26 threads, `res_mib` 1337, loader reached `Biotech/…/Keyed/Misc_Gameplay.xml`; block layer clean |
| L2 | 1800 s | 1 | 1 | 0 / 0 | 0 / 0 / 0 | 368 407 (`fr_high` 495 004) | 1260 | 1206 | game alive: pid 4, 25 threads, `res_mib` 1332, 1260 presents; ends in the known post-load handshake park (`[ALIVE-SET]`), block layer clean |

Host side, same windows (`obj/l75/trace-L<N>.log`): **6 `pci_nvme_mmio_cfg`
writes each** — OVMF's `0x0`/`0x460001`, the driver's boot `0x0`/`0x460001`,
and the self-check's `0x0`/`0x460001` — plus one `pci_nvme_pci_reset` at boot
(OVMF). No `pci_nvme_mmio_stopped` other than those, and **zero**
`pci_nvme_ub_*` / `nvme_error` / `pci_nvme_err_*` events: nothing but this
driver ever wrote the register block, and QEMU never rejected an access.

Reset interval, before → after:

| build | windows | resets | interval |
|---|---:|---:|---|
| L65 / L67 / L73 (pre-fix) | 6 | 1-7 per window | first reset at 490-740 s, then a cascade; every 1800 s window ended by 730-740 s |
| L75 (this lane) | 2 | 0 unplanned | none: `fr_used` passes 327,680 (the old hole entry) and 369,000 (the old BAR frame) with `CC` untouched |

## 4. Regression

* `TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **434 run, 0 failed — UNIT TESTS
  PASSED** (433 before this lane + `test_frame_pool_is_ram_only`, which asserts
  that no pool extent intersects the PCI hole, that the two MMIO shadows are
  reserved, that the old BAR page is not in the pool, and that `hole=0`).
* Gates (`bash tools/rw/l75-gates.sh`, one log per gate under
  `obj/l75/gates/`): **9/9 PASS** — redzone, mmap_race, m2_mono, m2_tp,
  m2_threads, m12, m3_window, m4_gl, rwnfs.

Net: **434/0 units + 9/9 gates + 2 x 1800 s windows**, with `hole=0`,
`fr_used` past the old trigger, and one reset per window — the lane's own.

## 5. What is left

1. **The block-stress probe's stall** (§2.5). The instrument is written and
   correct for its first ~70 s; it stalls when the game's load starts. The next
   lane should take it from a thread that is *parked* between rounds by the
   scheduler (not a `safe_wfi()` spin) and print a heartbeat row, so "no
   progress" is a row rather than an inference.
2. **The post-load park** (§51.4/L65): the loader completes its data load
   (`[XO]` 1207, the menu's keyed strings) and then stops presenting
   (`[XSW] changed=0`) while pid 4 stays alive with 26 threads. That is the
   next wall, and it is not a block-layer or page-in problem.
3. **`aliasing on 8-byte stores`**: QEMU silently ignores non-32-bit writes to
   the register block (it only logs `pci_nvme_ub_mmiowr_*`). Any future probe of
   this class must use 32-bit stores.
