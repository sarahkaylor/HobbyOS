# cp-fault — the kernel's share of game hitches (H7-fault, wave 9)

Branch `cp-fault`, from `cyberpunk` = integ21 = 5f8c4b76.  Evidence in
`docs/cp-fault-evidence/`.  Every number is from **strix** (Ryzen AI MAX+ 395,
Zen 5, KVM; the user's desktop with other lanes' guests running), P9 boots at
`-m 32G` on Venus with Wine `wine-cp2`, unless a row says otherwise.

Two leads came out of wave 8:

- **(a)** at every audio dropout the game's audio-clock threads spent 16–25 % of
  their absence "in the kernel, not in a syscall" (H4's `[AWAY]` T samples);
- **(b)** NFS resend pauses of 150–260 ms in 14 of 18 boots (H1-13).

## 1. Summary

- **(a) The dominant fault cost is anonymous zero-fill, and it was paid under the
  one global `vm_lock`.**
  - An fp720 boot takes about 1.9 M faults; 98 % are anonymous zero-fills.
  - Each one cost 10–11 µs, of which about 7.4 µs was zeroing the frame. In a
    P9 guest that store is usually the host's first touch of the page: guest
    RAM is shmem-backed with 4 KiB host pages (`AnonHugePages: 0` for the QEMU
    process).
  - The zero ran while holding `vm_lock`, the irqsave lock every fault of every
    address space takes. Per boot that was 17–19 s of hold and 9.4–10 s of wait
    at `vm_lock` sites. One fault held it for 474 ms (a host stall), and one
    `linux_probe_pfpar` fault waited 94 ms.
  - The frame allocator walks the pool forwards, so a game that frees and
    reallocates keeps touching guest RAM the host has never backed. In the world
    phase, 940 MiB were zero-filled while the free pool shrank by only ~100 MiB.
  - Console rows printed under the lock (`[PUBS]` every 4096 publishes) held it
    for 4–15 ms at a time.
  - File-backed faults are rare (NFS 10 k, ext2 2 k, FAT 4 k per boot, nearly
    all before the menu). The T share is about half faults and half timer
    interrupts (14 µs per interrupt from user mode, roughly 500 Hz per CPU).
- **(b) Two shapes, and the pipeline sat out its whole 150 ms first wait in
  both.**
  - **Lost:** the original reply never came (`late_ms=-1`), and the re-sent READ
    was answered in 8 ms.
  - **Delayed:** nothing arrived for 478 ms during a host stall, and then
    everything arrived at once.
  - In the lost shape the host dropped nothing: Udp `RcvbufErrors` +0, QEMU's
    slirp sockets and nfsd's socket at `d0`. The guest received 1 frame in the
    whole 150 ms wait, while its waiter was running (`gap_max_us=43`).
  - Every answered READ came back within 64 ms, and 99.96 % within 4 ms.
  - In the lost shape, one IP fragment of the 23 in a 32 KiB reply never
    reached reassembly (`ipr=2/31424/52`).
  - The TX path is ruled out: 0 full-ring waits and 0 drops.
  - The suspect is the virtio-net RX loops: they re-queue a buffer before
    reading it. That suspect is now counted.
- **Fixes (each with a check seen failing first):**
  - F0: `vm_arch_reload_cr3` only flushes; it never switches.
  - F1: zeroed frames come from a per-CPU stash filled before `vm_lock`.
  - F2: recently freed frames are handed out first (warm LIFO).
  - F3: no console row is printed under `vm_lock`.
  - F4: NFS first wait cut from 150 to 50 ms, and a late original reply now
    completes the READ.
- **Result (A/B, §5).** Per fp720 boot, base (n=5) vs final lane (n=3):
  - fault time 22.1 s → 13.7 s;
  - anon fault 11.0 → 6.6 µs;
  - `vm_lock` wait at every site 10.3 s → 1.6 s;
  - hold inside faults 18.4 s → 0.9 s;
  - the `[AWAY]` T share of the audio-clock threads 33 % → 22 %;
  - NFS stall pauses 151 ms + re-send → 50 ms + 2–21 ms.

  fps (world 25.7/23.0 → 28.3/23.8, median/p10) and audio dropouts (4.4 →
  4.3) did not move beyond the noise of a loaded host (1.5–5.2 s of host stall
  per boot).

## 2. The instrument

All of it is measurement only, and every row has a budget.

`src/kernel/vm_pfstat.c` (x86_64; empty inlines on ARM), one row set per 10 s
while faults happen (400 sets):

| row | what |
|---|---|
| `[PFK] t= dt= n= us= kmode= rep= <kind>=n/us/max[/io=us/rpc=n]` | faults by the path that resolved them: anon, shobj, nfs, ext2, fat, img, cow, priv, retry, err; `io` = time fetching the page's source; `rpc` = faults that put an NFS READ on the wire; `rep` = same page refaulted within 10 ms |
| `[PFH] t= all=<17> file=<17>` | latency histogram, buckets < 1, 2, 4 … 32768 µs, then the rest |
| `[PFL] t= lw= lh= vml=n/w=sum/max@line/h=sum/max@line zero=n/us/max fresh=n/us shoot=n/us/max/cpus= warm=` | `vm_lock` wait/hold inside faults; every `vm_lock` pair in vm.c with the source line of the longest wait and hold; frame zeroing (`fresh` = the first frame of a 2 MiB chunk); TLB shootdowns; allocations the warm LIFO served |
| `[PFI] t= pf= tmr= ipi= oth=` | every trap from user mode that returned to the same thread, by class |
| `[PFT] t= top=tid:pf_us:irq_us:name` | the four threads with the most fault + interrupt time |
| `[PFX] F tid= at= us= kind= va= io= lw= lh= zero= shoot= rpc= km=` / `[PFX] I tid= at= us= vec=` | every single fault or user-mode interrupt of 2 ms or more (400 rows), to match against `[AWAY]` absences |

NFS (`nfs.c`, `net.c`, `virtio_net.c`):
- `[NFSRTH] rtt=<13> late=<13> stale= late_orig= late_used= sstale= rxq_max= rxq_full= txq_wait= txq_drop= txq_wmax_us=`:
  - the READ round-trip histogram (buckets < 128 µs << b, i.e. b0 < 128 µs … b11 < 256 ms, b12 the rest);
  - late original replies;
  - stale datagrams;
  - the RX backlog depth;
  - the TX full-ring waits and silent drops.
- `[NFSRS] off= want= age_ms= inflight= sys_kib= resend_ms= late_ms= iters= gap_max_us= pend= rxpk= frags+= ipr_to+= stale= stalls= ipr= last= txdrop+= txwait+= at=`:
  one row per pipeline stall, followed to the end of the pipeline.
  - Which came first: the re-sent READ's reply or the original.
  - What the timed-out wait saw: looks at the socket, the longest gap between
    two looks, unprocessed RX ring entries, frames processed.
  - The reassembly state, the last RX frame, and TX drops since the pipeline
    started.

Host side (`tools/cp/fault/hostnet.sh`, read-only, 1 Hz beside each boot):
- Udp counters;
- the drops on the QEMU process's UDP sockets and on nfsd's :2049 socket;
- `/etc/exports` edit times;
- compaction counters.

`tools/cp/fault/fp.sh KIND RUN` runs one `run-hunt.sh` boot with the sampler,
then `pfsum.py` (per-phase sums, long events inside `[AWAY]` absences, NFS
rows, host deltas). `abtable.py` builds the A/B table.

Checks:
- `linux_probe_pfstat` + `lp_check_pfstat.py` (the rows count a known mix:
  4096 anon, 256 COW, 8 file pages). It fails on integ21, which has no rows.
- `linux_probe_pfpar` + `lp_check_pfpar.py` (§4).

## 3. (b) NFS resend pauses (H1-13)

`docs/cp-fault-evidence/nfs-stalls-strix.txt`.

- **The stall shape is fixed.** One READ is in flight and its reply never comes,
  while the later READs on the same socket are answered. The pipeline fills its
  8 slots and times out 150 ms after it last issued. The stall rows of wave 8
  show the same thing: `issued=8 replies=7`, `issued=23 replies=22`, with the
  missing block always at `contig`.
- **What a stall saw.** `[NFSRS]` shows two shapes.
  - **Lost** (ab-base-fp2, and every wave-8 row with `replies = issued - 1`):
    - `age_ms=151 resend_ms=8 late_ms=-1`: the original never arrived, and the
      re-send was answered at once;
    - `iters=2385569 gap_max_us=43`: the waiter spun the whole time, so it was
      not a vCPU stall;
    - `pend=0 rxpk=1`: nothing was waiting in the RX ring, and one frame
      arrived in 150 ms;
    - `ipr_to+=0`, and the boot's `[NFSNET] drop_timeout` stayed at 0–2: no
      fragment train was cut off.
  - **Delayed by a host stall** (ab-base-fp3, `at=181781`):
    - `inflight=2 issued=2 replies=0 rxpk=0`, and then
      `resend_ms=328 late_ms=328 frags+=92`: both originals and both re-sends
      arrived together, 478 ms after the first send.
    - The same boot has `[PFX] F tid=97 at=182106 us=474304 kind=anon … lh=474304
      zero=7`, a fault whose vCPU stopped for 474 ms while holding `vm_lock`.
    - The console counted 5.2 s of host stall (`stolen_ms`) in that boot. Nothing
      in the guest can shorten this shape.
- **The host dropped nothing** (hostnet.sh over every boot):
  - Udp `InErrors`/`RcvbufErrors` +0;
  - every QEMU slirp UDP socket at `d0`, with a recv-queue max of 33–68 KB;
  - the nfsd :2049 socket at `d0`;
  - no `/etc/exports` edit within 3 s of a stall, so not exportfs-flushed
    caches.
- **RTT distribution** (`[NFSRTH]`, 4 boots, ~100 k READs each): 99.96 % of
  answered READs within 4 ms, 2–101 per boot at 8–16 ms, 0–4 at 16–32 ms, 0–6
  at 32–64 ms, none later. A 150 ms first wait is 2.3× the slowest reply ever
  seen.
- **Rate:**
  - base: 5 stalls in 4 of 6 boots (5 fp720 + 1 soak): 4 lost, 1 delayed;
  - final lane: 2 stalls in 1 of 4 boots (3 fp720 + 1 soak), one of each
    shape;
  - 0–2 per boot in H4's kg2 boots (6/11);
  - H1's 14/18 were boots on integ15/17, with more lanes booting at once.
- **Where the lost reply goes.**
  - **Suspect 1, ruled out.** `virtio_net_send` spins ~100 000 `pause`s (~2 ms)
    when all 16 TX slots are in flight and then drops the frame, and
    `net_socket_send`'s UDP path ignores the drop. 1dc4aa02 counts it.
    - The next lost stall (ab-fin-fp2) had `txdrop+=0 txwait+=0`.
    - The boot had `txq_wait=0 txq_drop=0`: the READ went out.
  - **What the lost stall shows instead** (e5dd95d7's fields):
    `ipr=2/31424/52 rxpk=1`. A reassembly slot was still collecting 52 ms after
    the send, holding 31 424 bytes of a ~32.9 KB reply.
    - So the reply arrived, minus one IP fragment.
    - That fragment never reached the guest's reassembly (the boot's
      `drop_timeout=2` is that slot timing out 1 s later).
    - The host socket dropped nothing, so the fragment was lost between QEMU's
      slirp socket and the guest's IP layer.
  - **Suspect 2, open.** Both virtio-net RX loops hand a frame's descriptor back
    to the device before `net_rx_packet` reads the buffer (the loops' own
    comment explains why).
    - If the reading vCPU stalls while the device fills every other posted
      buffer (128; the backlog peaked at 48), the device writes the next frame
      into the buffer being read.
    - The frame is torn, and parsing it as some other fragment loses this one.
    - That needs a host stall of the reading vCPU while slirp keeps delivering,
      which matches the rarity and the host load. df28d9ca counts it
      (`rxq_reuse=`): reads during which the used ring advanced by every
      posted buffer.
    - The fix, once a count says so: copy the frame out before re-queuing the
      descriptor (one 1.5 KB copy per frame).
- **Fix F4 (1164f98f):**
  - `NFS_PIPE_TIMEOUT_MS` 150 → 50; a re-sent READ still waits 500 ms.
  - The first send's reply completes the READ even when it comes after the
    re-send (the slot keeps its first xid), so a wait shorter than the slowest
    reply costs one extra READ, not time.
  - Bench: cpnfsbench `inject` (every 5th reply dropped, rsize_guard G9).
    every5.read **2974 → 1374 ms**, every5.mmap **2501 → 910 ms**; rsize_guard
    PASS before and after (`rsize-inject-ab-strix.txt`).
  - In a game boot (ab-fin-fp2) the two stalls were:
    - one lost: `age_ms=51 resend_ms=2`, a 53 ms pause where the base paid 151
      + 8 ms;
    - one delayed: `inflight=2 replies=0 age_ms=50 late_ms=21 resend_ms=-1`.
      Both originals arrived 71 ms after sending and completed their READs
      (`late_used=2`); the re-sends were never needed.

## 4. (a) Fault cost and the fixes

`docs/cp-fault-evidence/fault-mix-fp720-strix.txt` (fa-fp1, the instrument
alone) and `pfpar-ab-strix.txt`.

**The mix (fa-fp1 per phase; µs).**

| phase | faults | fault ms | anon n / avg µs | `vm_lock` wait / hold in faults (ms) | all-site wait (ms) | zero n / avg µs | shootdowns n / ms / max ms | timer traps from user n / ms |
|---|---|---|---|---|---|---|---|---|
| launch (134 s) | 988 k | 11 200 | 977 k / 10.5 | 1307 / 8851 | 5284 | 1074 k / 7.4 | 1354 / 2726 / 258 | 887 k / 11 977 |
| loading (80 s) | 653 k | 6 329 | 652 k / 9.7 | 332 / 5864 | 2753 | 653 k / 7.4 | 691 / 1675 / 59 | 484 k / 5 997 |
| world (101 s) | 240 k | 2 311 | 240 k / 9.6 | 91 / 2167 | 976 | 241 k / 7.6 | 425 / 465 / 21 | 417 k / 6 029 |

File-backed faults are a sideshow:
- NFS: 10 428 faults at 23 µs, 183 of them put a READ on the wire;
- ext2: 2147 at 391 µs;
- FAT: 4353 at 101 µs.

All of them come before the menu, and file pages are read ahead per 64 KiB
window, so "a page at a time" is not the cost. Repeat faults (`rep`) are ~300
per boot, all at boot, and NFS I/O under `vm_lock` was never seen (the file
paths drop the lock for the read).

**Why a zero costs 7 µs.** The P9 QEMU process has `AnonHugePages: 0`, because
its guest RAM is a memfd for Venus blobs. Every 4 KiB guest frame's first store
is therefore a host page fault, and the frame allocator hands out never-touched
frames. It walks the bitmap forwards from a rotating hint and revisits a freed
frame only after wrapping 32 GiB. Under host memory pressure a 2 MiB first touch
took 2–38 ms in `linux_probe_pfpar`.

**The `[AWAY]` T share.** Over the same boot, the game's two audio-clock threads
(tids 92 and 96) spent 6.6 s and 8.9 s in faults plus interrupts during launch.
User-mode timer interrupts are as large as faults by the world phase: 6.0 s vs
2.5 s, 14 µs each. Of 80 `[AWAY]` absences with T samples, 22 had a ≥ 2 ms fault
inside, e.g. `tid=96 ms=2947 T=160 long_faults_inside=25 (86.1 ms)`. The rest of
T is many short faults and timer interrupts.

**F0 (4112438a): `vm_arch_reload_cr3` flushes, it never switches.**
- What failed: the unit tier crashed in `test_v2_image_first_page` with a kernel
  #PF in `nvme_ready_now`, `cr2=0xFFFF808000000014` (the ioremap window), and the
  guest reset and re-ran the suite.
- Why: `vm_handle_fault`'s retry path loaded the faulting AS's root into CR3.
  `test_vm_demand_fault` faults a test AS from kernel context, so CR3 was left
  on that root without `v2_active_root` knowing. The test then tore the AS down,
  and every later test ran on a freed PML4.
- The forward-walking allocator had hidden this: F1 reused the freed root
  frame, for FAT directory data.
- Production faults always run on the faulting AS. The reload now happens only
  when CR3 already holds the root.

**F1 (724a793a): zeroed frames before `vm_lock`.**
- Each CPU keeps four zeroed frames. They are topped up at the start of
  `vm_handle_fault` and of every `vm_touch` page, with no lock held.
- The locked paths take one from the stash and zero inline only when it is
  empty: anon fault and touch, shared-object page, private-object copy, file
  COW copy, fork-COW break.
- `frame_used_count()` counts stashed frames as free, so the unit tier's frame
  balances hold.
- The check: `linux_probe_pfpar`, four threads first-touching 64 MiB each.
  - Before: one thread zeroes a fresh frame under the lock (`lh=zero=38089`)
    while the other three spin (`lw=38091 … 38192`); max lw 94 ms, 72 of 116
    long-fault rows with lw ≥ 1 ms.
  - After: `lh` is 0–3 µs on every long fault, and the long ones are the
    thread's own host first touch (`zero=` 2–30 ms).
  - `lp_check_pfpar.py` fails on lw ≥ 1 ms: base FAIL (72/116, 3/7), lane PASS
    (0/21).

**F2 (2963d3da): the warm LIFO.**
- `frame_free()` pushes the frame onto a 65 536-entry LIFO (4096 on ARM),
  overwriting the oldest entry when it is full. `frame_alloc()` pops from it
  before scanning.
- A popped frame whose bitmap bit is set again is skipped, so the bitmap stays
  the only owner record.
- Unit test `test_frame_warm_lifo`: a frame freed after 256 more allocations is
  the next one handed out. Before: `f=0x200000000 g=0x200101000`, FAIL. After:
  `g == f`.
- `linux_probe_pfpar` R1 (map / first-touch / unmap 32 MiB, rounds 1–5):
  **75–144 ms → 44–49 ms**, worst fault **4.4–13.4 ms → 0.69–0.79 ms**.
- In the game's world phase, 29 k of 35 k zero-fills per 10 s came from the
  LIFO.

**F3 (d419ec9d): no console row under `vm_lock`.**
- What it was: after F1 the longest fault-path holds were anon faults with
  `zero=7 lh=4043…15235`. `vm_pub_note` prints a `[PUBS]` row every 4096
  publishes, and a console row costs a trapped port write per byte plus the
  wire wait.
- `vm_row()` parks a row made under the lock in a per-CPU record, and
  `vm_lock_rel()` prints it after the release.
- The anon fault and touch paths now call `vm_pub_note` (which also scans the
  page) after the release.
- The check: the stash-only pfpar run shows the shape (`[PUBS]` printed while
  tid 6 held `lh=2261` with `zero=6`, and tid 4 waited `lw=1302`); with F3,
  0 rows with lw ≥ 1 ms.

Candidates measured and set aside:
- fault-around / readahead (file faults are 1 % of the count);
- a lock held across NFS I/O (never seen: io sits outside `lh`);
- repeat faults (~300 per boot, at boot).

TLB shootdowns are real (below) but are not fault cost.

## 5. A/B (one tree pair, booted alternately, same session)

Base = af9da50b (the instrument and tools on integ21); lane = the fixes. fp720
audio boots (`tools/cp/fault/fp.sh fp720`, `FAULT_AUDIO=1`, QEMU `wav` backend
only, the WAV checked and deleted). Soak: `tools/cp/hunt/soak.p9`. `abtable.py`
over `~/cp/lanes/fault-logs`; full table in `ab-table-strix.txt`.

Chain 1, lane = 92f7fbb4 (F1+F2; behaviourally identical to 2963d3da):

| per boot (mean) | base (n=3) | lane F1+F2 (n=2) |
|---|---|---|
| fpsprobe world median / p10 | 25.5 / 23.3 | 25.0 / 21.4 |
| prof-fp-world median / p10 | 30.3 / 27.6 | 29.1 / 25.9 |
| audio dropouts (gamecheck) / `[SND] zeros` rows | 4.7 / 6.0 (3/3 boots with zeros) | 4.5 / 5.5 (2/2) |
| faults / fault time | 1.90 M / **22.0 s** | 1.87 M / **12.3 s** |
| anon fault avg | **10.9 µs** | **5.8 µs** |
| `vm_lock` wait / hold inside faults | **1.94 s / 18.4 s** | **0.43 s / 2.43 s** |
| `vm_lock` wait, every site | 9.9 s | 5.3 s |
| `[PFX]` faults that waited ≥ 1 ms for the lock | 26 | 24 (the `[PUBS]` rows, F3) |
| TLB shootdown time | 5.0 s | 4.9 s |
| NFS pipeline stalls | 2 in 3 boots | 0 in 2 |
| host stall (console `stolen_ms`) | 1.6–5.2 s | 1.5–1.9 s |

Chain 2, lane = 1164f98f (F0–F4; fin-fp2/3 add the 1dc4aa02/df28d9ca counters,
which change no behaviour), base boots 4–5 alternated with it. Totals over chains
1 and 2:

| per fp720 boot (mean) | base af9da50b (n=5) | final lane (n=3) |
|---|---|---|
| fpsprobe world median / p10 | 25.7 / 23.0 | 28.3 / 23.8 (fin-fp2: 34.6, a quieter host; without it 25.2 / 22.6) |
| prof-fp-world median / p10 | 30.5 / 28.0 | 32.2 / 29.0 |
| audio dropouts / `[SND] zeros` rows (k/n boots with zeros) | 4.4 / 6.0 (5/5) | 4.3 / 5.7 (3/3) |
| faults / fault time | 1.89 M / **22.1 s** | 1.90 M / **13.7 s** |
| anon fault avg | **11.0 µs** | **6.6 µs** |
| `vm_lock` wait / hold inside faults | **2.02 s / 18.4 s** | **0.33 s / 0.90 s** |
| `vm_lock` wait, every site | **10.3 s** | **1.6 s** |
| faults that waited ≥ 1 ms for `vm_lock` (`[PFX]`) | 31.4 | 7.7 |
| TLB shootdown time | 5.2 s | 5.2 s |
| NFS pipeline stalls / pause each | 4 in 5 boots / 151–152 ms + re-send 1–8 ms (one delayed: 478 ms) | 2 in 3 boots / 50–51 ms + 2 ms (lost) or + 21 ms (delayed, late original used) |
| `[AWAY]` T share over the audio-clock threads' absences (`audio=1` rows) | 29.7–36.6 % (mean 33.2) | 19.1–24.3 % (mean 21.9; chain-1 lane 20.3–24.4) |
| host stall per boot (`stolen_ms`) | 1.6–5.2 s | 1.5–2.1 s |

The soaks (`soak.p9`, ~28 min in the world, no audio, n=1 each). H6's guest
shared the host and the GPU through both: load 10.5–10.9, 9070 XT 83–85 % busy.

| soak | base (ab-base-soak1) | lane (ab-fin-soak1) |
|---|---|---|
| soak phases median of medians / p10 | 36.6 / 33.7 | 31.8 / 29.2 |
| hitches (classify.py, fps rows > 2.5 s apart) | 1 | 0 |
| faults / fault time / anon avg | 2.78 M / 30.8 s / 10.6 µs | 3.39 M / 18.4 s / 5.1 µs |
| `vm_lock` wait (every site) / hold in faults | 19.1 s / 26.5 s | 6.5 s / 1.25 s |
| faults that waited ≥ 1 ms for the lock | 32 | 6 |
| TLB shootdown time | 7.1 s | 10.4 s |
| NFS stalls | 1 (lost, 151 + 1 ms) | 0 |
| host stall (`stolen_ms`) | 2.2 s | 1.9 s |

- The lane soak's lower fps comes with 22 % more faults and 47 % more
  shootdown time in the same script. That is a different run of the game
  world, and probably a different GPU share with H6's guest.
- One soak each cannot separate those causes. The fp720 pairs (n=5/3), with
  equal shootdown time, are the A/B.
- `rxq_reuse=1` in the lane soak: the RX re-queue window (H7-05 suspect 2)
  opened once in 30 minutes, with no stall then.

- The fault path's cost to the game halved, and the lock serialization is gone:
  the wait at every `vm_lock` site fell 6×. The vm_lock hold inside faults fell
  20×.
- fps and audio dropouts are within the run-to-run noise of a loaded host. The
  dropouts are game-side (H4 §13), and the zero rows follow them.
- The anon fault's remaining 6.6 µs is mostly the host's first touch of guest
  RAM that is new to the guest (§9).

## 6. Findings

| ID | symptom | rate (k/n, boots) | first seen | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| H7-01 | every anonymous zero-fill zeroes its frame under the global irqsave `vm_lock`; in a P9 guest that store is a host page fault, so every faulting CPU waits behind it | 6/6 fp720 (fa-fp1, ab-base-fp1..3, ab-lane-fp1..2), pfpar 4/4 | `fault-mix-fp720-strix.txt` (fa-fp1 launch row) | `[PFX] F tid=4 … kind=anon … lw=6 lh=38091 zero=38089` + three `lw=38091…38192` | kernel (vm.c) | perf (hitch: up to 474 ms of one hold, 94 ms of one wait) | fixed in 724a793a |
| H7-02 | the frame allocator first-touches never-backed guest RAM for memory the guest has just freed (forward-walking hint); the P9 guest's host footprint grows toward its `-m` | 6/6 fp720 | `fault-mix-fp720-strix.txt` (world: 940 MiB zero-filled, pool −100 MiB) | `[PFL] … zero=241k/1.8s … fresh=471` with `[P8MEM]` flat | kernel (frame.c) | perf | fixed in 2963d3da |
| H7-03 | `vm_arch_reload_cr3` switched CR3 to the faulting AS instead of flushing; the unit tier then ran on a freed PML4 | 1/1 unit runs with F1 (hidden before by the allocator) | `unit-cr3-strix.txt` | `[KERNEL] FATAL kernel-mode fault … rip=…nvme_ready_now cr2=0xFFFF808000000014` | kernel (x64 mmu.c; test-only path) | crash (unit tier) | fixed in 4112438a |
| H7-04 | console rows (`[PUBS]` every 4096 publishes) printed while holding `vm_lock`: 4–15 ms holds | 2/2 lane F1+F2 boots, 21–27 lock waits ≥ 1 ms per boot | `fault-ab-rows-strix.txt` | `[PFX] F tid=149 … kind=anon … lh=15235 zero=7` | kernel (vm.c instrument) | perf | fixed in d419ec9d |
| H7-05 | a pipelined NFS READ stalls for its full 150 ms first wait (H1-13): either one IP fragment of the reply never reaches reassembly (lost), or a host stall holds every reply back (delayed) | base: 4/6 boots (ab-base-fp2, -fp3, -fp5, -soak1; 4 lost + 1 delayed); final lane: 1/4 (ab-fin-fp2, one of each); fa-fp1 and the F1+F2 lane 0/3; 6/11 kg2 | `nfs-stalls-strix.txt` | `[NFSRS] … age_ms=151 … resend_ms=8 late_ms=-1 … gap_max_us=43 pend=0 rxpk=1`; lane `… ipr=2/31424/52 … txdrop+=0` | kernel NFS client (the wait); the lost fragment: guest RX re-queue race (suspect, df28d9ca counts it) or QEMU/slirp | perf (150–480 ms pause) | pause cut by 1164f98f (lost: 53 ms; delayed: 71 ms, late original used); loss cause open |
| H7-06 | `virtio_net_send` drops a frame silently after ~2 ms with all 16 TX slots in flight (`net_socket_send` ignores the return) | 0 waits, 0 drops in the 3 lane boots with the counter (ab-fin-fp2, -fp3, -soak1) | code review; `nfs-stalls-strix.txt` | `txq_wait=0 txq_drop=0` | kernel (virtio_net.c, net.c) | latent (would lose an RPC) | not the H7-05 cause; the silent drop is left as is (counted) |
| H7-07 | TLB shootdowns cost 4.9–5.2 s per fp720 boot, up to 740 ms each (synchronous IPIs to vCPUs the host has not scheduled) | 6/6 fp720 | `fault-mix-fp720-strix.txt` | `[PFX] F … kind=cow … shoot=279674`; `[PFL] … shoot=790/2488113/739666` | kernel (x64 mmu.c) + host scheduling | perf | open |
| H7-08 | timer interrupts that land on user threads cost 14 µs each (~500 Hz per CPU); about half of `[AWAY]`'s T samples | 6/6 fp720 | `fault-mix-fp720-strix.txt` | `[PFI] … tmr=416731/6028782us/max3251` (world) | kernel (trap.c 0x84 path, scheduler) | perf | open (not this lane's files) |
| H7-10 | after F1–F3 the longest `vm_lock` holds are `vm_mprotect`, `vm_mmap` and the JIT unmap over large spans (3–5.5 ms), and `vm_touch` waits for them | 1/1 final-lane boot checked (ab-fin-fp1) | `fault-ab-rows-strix.txt` | `[PFL] … vml=…/h=225075/4732@5573` (vm_mprotect), `…/3107@5174` (vm_mmap) | kernel (vm.c) | perf (ms holds, 7–10 faults per boot wait ≥ 1 ms) | open |
| H7-09 | this lane's `[NFSRTH]` row (fab2e460) referenced `virtio_net_rx_backlog_max`, which only the x86_64 virtio-net driver defined; found by review before an ARM gate ran | 1/1 (review) | `src/kernel/virtio_net.c` | the counter defined inside `#ifdef __x86_64__` only | kernel (this lane) | build (ARM) | fixed in 1dc4aa02 (counters defined for both arches; ARM build passes) |

## 7. Gates

`~/cp/lanes/fault-gates/gates.sh` is a copy of `~/cp/integ-gates21.sh` pointed at
`~/cp/lanes/fault-dev` (the lane at 1164f98f, plus f258ad62's probe check; the
counter commits 1dc4aa02 and df28d9ca were staged into it during the probe
phase). `tip-rerun.sh` reran on the tip (df28d9ca) in `~/cp/lanes/fault-chk` every
gate those two commits could touch. Raw rows: `gates-strix.txt`.

| gate | lane | integ21 reference |
|---|---|---|
| build (intel test) | PASS | PASS |
| unit | 464/0 (tip: 464/0); +1 `test_frame_warm_lifo` | 463/0 |
| probes | 71/71 (the 69 of integ21 + `pfstat`, `pfpar`, both with their console checks); tip rerun: pfstat, pfpar, console, exitgroup, hello, netlink, loaccept, tcpconn all PASS | 69/69 |
| sigmask-gate | 8/8 | 8/8 |
| ext2_gate / exec_gate | PASS / PASS | PASS / PASS |
| rsize_guard | PASS (gate batch and tip) | PASS |
| rsize_scan | PASS (gate batch and tip) | PASS |
| nfs_proto_test_host | PASS | PASS |
| host_tests | 40/42 (the known gaming.md pair; tip 40/42) | 40/42 |
| castscan | 0 new sites (373 outside kmem.h, all allow-listed) | PASS |
| ARM build | PASS (gate batch and tip) | PASS |
| panic-sink allowlist | PASS | PASS |
| pfx_gate | PASS | PASS |
| ext2w_gate | PASS | PASS |
| Gate P2 ×2 | G1/G2/G3 PASS ×2 | PASS |
| Gate P5 | vkcube, d3d11, d3d12, fullscr PASS (boot 275 s) | PASS ×3 |
| Gate P8 Venus | main menu at s008-t000109 (109 s game time, +134 s host), verdict main-menu | main menu at s009-t000114 (+138 s) |

## 8. Commands (strix)

```
# one boot with the host sampler and the summary (tree = the cwd's lane tree)
cd ~/cp/lanes/fault && FAULT_AUDIO=1 bash tools/cp/fault/fp.sh fp720 <run>
bash tools/cp/fault/fp.sh soak <run>
# the A/B table
python3 -I tools/cp/fault/abtable.py ~/cp/lanes/fault-logs ab-base-fp1 ... ab-fin-fp3
# probes
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh pfstat
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh pfpar
# gates: ~/cp/lanes/fault-gates/gates.sh (integ-gates21.sh pointed at the lane tree)
```

## 9. What is left

- **The host's first touch (the rest of the 6.6 µs anon fault).** Each P9
  guest frame's first store is a host 4 KiB shmem fault: the guest RAM is a
  memfd for Venus, with no THP.
  - Two guest-side levers remain:
    - a bounded background pre-zero pool. A kernel task keeps N MiB of frames
      zeroed ahead of demand, on idle vCPUs, so a game thread never pays the
      host fault. This costs the same host CPU, and the host footprint grows
      only by N.
    - a larger warm LIFO.
  - The host-side lever (hugepage- or prealloc-backed guest memory) is a host
    setting: ask the user.
- **H7-05, the lost fragment.** df28d9ca counts the RX re-queue race
  (`rxq_reuse=`, 0 in fin-fp3). If a lost stall comes with `rxq_reuse` moving,
  copy the frame out before re-queuing its descriptor. If not, the loss is in
  QEMU/slirp; the 50 ms wait bounds it.
- **H7-07, TLB shootdowns: 5.2 s per boot,** synchronous, up to 740 ms each.
  - The targets are vCPUs the host has not scheduled.
  - Levers: batch the munmap/mprotect shootdowns of one call, skip CPUs that
    are not running the AS (lazy flush at switch-in), and let a CPU spinning
    on `vm_lock` service a pending flush.
- **H7-08, timer interrupts on user threads: 14 µs each,** ~500/s per CPU,
  about as much T time as faults. The path is trap.c's 0x84 handler
  (`sched_hrt_fire`, `linux_compat_timer_tick`, `schedule`). It is not this
  lane's code; the numbers are in `[PFI]`.
- **H7-10:** `vm_mprotect` / `vm_mmap` / the JIT unmap hold `vm_lock` for 3–5.5
  ms over large spans.
- **The instrument's own cost.** A row set is ~1 KB of console every 10 s,
  printed by whichever thread's fault crosses the interval (after its trap is
  timed, so the rows do not see it). Plus up to 400 `[PFX]` rows. The
  integrator can gate the set off by default if it shows up in a hitch.
- **Gate coverage.** The probe phase of the gate batch ran on 1164f98f/f258ad62
  kernels. 1dc4aa02 and df28d9ca (counters only) were added after it, and the
  infra rerun on the tip is in §7.

# Round 2 (base integ22 = aed4a1f2, branch cp-fault)

**Round-2 summary.**
- **H7-05, the lost reply fragment, is fixed.**
  - The virtio-net RX loops handed a descriptor back before reading its
    frame. A deterministic injection reproduces the game's stall signature,
    and once a wrong byte.
  - Copying the frame out first (f83a2dfe) removes it.
  - Game boots: lost-fragment stalls in 2 of 5 base boots → 0 of 6 lane boots
    (5 fp720 + 1 soak). All NFS stalls: 10 → 0 in the fp720 boots (§10, §14).
- **H7-07, TLB shootdowns: 6.0 s → 1.3 s per fp720 boot.** Late targets were
  inside first-touch faults, descheduled, spinning with interrupts off, or
  writing to the console. Fixes, each with a probe seen failing first:
  - KVM PV flush;
  - polls in the interrupts-off spins and in the console writer;
  - vmzero, a kernel task that zeroes frames ahead of demand.

  The gates caught three bugs in vmzero itself, all fixed (§12):
  - it kept its CPU (hrtimer), fixed in 9c032122;
  - it halted in front of woken threads (ntsync), fixed in a3112535;
  - it outlived its users (thrstorm's leftover census), fixed in 8649c0a3.
- **H7-10, long vm_lock holds.**
  - mprotect is chunked, and the chunk gap is now fair (test-and-set lock).
  - munmap / MAP_FIXED unhook whole page tables and drain them after the lock.
  - New page tables come from the pre-zeroed pool.
  - vmhold FAIL → PASS, max wait 0 µs (§11).
- **Faults: 13.9 s → 5.6 s per boot; anon fault 6.6 → 2.3 µs** (§14). fps
  did not move. Audio dropouts 5.4 → 2.8 per boot, within boot-to-boot noise.
- **H7-08, timer interrupts (measured).** The 0x84 path is steady overhead.
  The hitches are the 5 s census in CPU 0's PIT interrupt, 13–371 ms with
  interrupts off, which is console output (§13). Every console byte costs the
  writer 9–55 µs with interrupts off (H7-11), and that sets the verbosity
  answer (§16).
- **integ22's slower P8 time-to-menu is load, not the lane.** Booted
  together, integ21 and cp-fault reach the menu at the same time (§15).
- **New, not this lane's code: the ext2 flusher keeps one of the 8 vCPUs** in
  every P9 game boot (H7-14). With an ext2 data disk, 8 busy threads get
  6.85–6.93 CPUs; without it, 7.76–7.83. A proposed patch gives them back
  (4 of 5 runs; §19).

## 10. H7-05: the lost reply fragment is the RX re-queue race (fixed, f83a2dfe)

**The suspect.** Both virtio-net RX loops (`virtio_net_handle_irq`,
`virtio_net_poll_rx`) hand a frame's descriptor back to the device before they
read the frame. They do it under `net_lock`, so two processors never race on
an avail slot, and then call `net_rx_packet` on the buffer they have just given
away. A reader that stalls there while the device fills every other posted
buffer (128 of them) reads the next frame instead.

**A deterministic repro (7af3bb1e).**
- Test kernels gain an injection, `.cp-nfs-inject.rxstall.<us>`. Every 32nd RX
  frame's reader spins for `<us>` µs between re-queuing the descriptor and
  reading the frame. That is the shape of a vCPU the host deschedules mid-copy.
- cpnfsbench phase `rxpress` runs under `rxstall.2000`. Six processes read six
  fresh 8 MiB files with 1 MiB `read()`s and compare every byte with the host's
  stream. `[NFSNET]` dumps before and after the phase give the stalls.
- rsize_guard **G12** fails on a wrong byte, or on more than 2 pipeline stalls
  in the phase.

| rsize_guard G12 (strix, one slot) | stalls in rxpress | wrong bytes | `rxq_reuse` | `[NFSRS]` signature | rxpress ms |
|---|---|---|---|---|---|
| without copy-out, run 1 (earlier bench order) | 25 (44 → 69) | **1 file** (`rxpress.child a=0 bad=1 first_bad=4362448`) | 9 → 30 | `ipr=6/31424`, `late_ms=-1`, `resend_ms=0–2` | 1047 |
| without copy-out, run 2 (final bench; G12 FAIL) | **31** | 0 | 33 | same | 1281 |
| **f83a2dfe** (copy-out; G12 PASS) | **0** | 0 | 0 | none | 590 |

- Every stall in the repro has the game boots' lost-stall signature: one
  fragment missing from a 32 KiB reply (`ipr=…/31424`), the original never
  arriving, and the re-send answered at once.
- The race can also deliver wrong bytes. The stack checks no IP or UDP checksum,
  so a frame torn after its header was parsed lands in the reply's data.

**The fix (f83a2dfe).**
- Under `net_lock`, before the re-queue, the frame is copied into a per-CPU
  buffer: one 1.5 KiB copy per frame.
- There are three buffers per CPU, so an IRQ pass nested in a poll pass takes
  the next one.
- The `rxq_reuse` counter now only counts frames read in place, which happens
  when a buffer could not be taken; it stays at 0.

Game boots before and after are in §14.

## 11. H7-10: vm_lock holds in mprotect, munmap and MAP_FIXED (96f11afd, 73895932, ad97e3f4, 1f1c88db)

**Probe** `linux_probe_vmhold` + `lp_check_vmhold.py` (96bef222).
- A faulter thread takes fresh faults the whole time.
- The main thread mprotects a resident 256 MiB region ×16, replaces it with
  MAP_FIXED ×4 and munmaps it ×4.
- The check fails when a `[PFX]` fault waited 1 ms or more for `vm_lock`.

| linux_probe_vmhold (strix) | verdict | waits ≥ 1 ms | mprotect pair max | MAP_FIXED per call | munmap per call |
|---|---|---|---|---|---|
| before (96bef222) | **FAIL 2/2** | 17 per run, up to 5.3 ms | 8.5–9.3 ms | 6.6–9.2 ms | 6.3–8.8 ms |
| 96f11afd | **PASS 2/2** | 0 | 7.0–7.8 ms (in 512-page holds) | 2.9–3.7 ms | 2.4–2.6 ms |

- **mprotect** drops `vm_lock` every 512 resident pages.
  - The region records are final before the walk. A fault in a gap therefore
    maps with the new protection.
  - An old leaf keeps its old rights only until the walk or the closing
    shootdown reaches it. Linux has the same window until its flush.
  - After each gap the region is looked up again. A page whose region another
    call changed in the gap is left to that call.
- **munmap / MAP_FIXED / madvise / mremap** (`vm_zap_range_defer`) clear leaves
  one page table at a time (`vm_arch_zap_range`).
  - That is one walk per 2 MiB instead of two four-level walks per page.
  - Only the first 32 leaves get an `invlpg`; past that, one local CR3 reload.
  - These calls still held the lock for the whole span, about 3× shorter.
    The round-2 gate batch then failed vmhold once: a fault waited 2.5 ms.
    That led to the three commits below.

**Three follow-ups (73895932, ad97e3f4, 1f1c88db).**
- **The mprotect gap did not let anyone in.** `vm_lock` is test-and-set. A
  release followed at once by an acquire re-takes it on the same CPU before
  a spinning waiter sees it free. On a check tree a faulter waited 2.0–3.1 ms,
  ten times in a row, across one chunked mprotect.
  - 73895932, `vm_lock_gap()`: when a CPU is spinning for `vm_lock`
    (`lock_wait_addr`), wait (bounded) until someone else has taken it, then
    re-acquire.
- **New page tables were zeroed under the lock.** `x64_next_table` takes a
  zeroed frame under `vm_lock`. In a P9 guest that frame's first touch is a
  host fault, so the check tree showed `lh=2082 zero=2081` at a 2 MiB-aligned
  address, with faults waiting behind it.
  - ad97e3f4: page-table frames come from vmzero's pool first.
- **munmap / MAP_FIXED unhook whole page tables.** A 2 MiB page table wholly
  inside the span is unhooked from its PD entry with one store and parked in
  the TLB batch. Partial tables are zapped in place.
  - After the lock, `vtb_finish` drains the parked tables: the same release
    rule per leaf, then the table frame itself. It settles the counters under
    one short hold, and only then shoots down and frees.
  - No walk reaches an unhooked table. A CPU that still caches one is flushed
    by the shootdown before anything is freed.
  - A fault in the span after the lock is dropped builds a fresh table.
  - This replaces the "dying region" marker the first version needed.

| linux_probe_vmhold (strix) | verdict | waits ≥ 1 ms | MAP_FIXED per call | munmap per call |
|---|---|---|---|---|
| 96f11afd, gate batch (host loaded) | FAIL 1/1 | 1 (2.5 ms) | ≤ 5.5 ms | ≤ 2.6 ms |
| 1f1c88db without 73895932/ad97e3f4 (check tree) | FAIL 2/2 | 5, 7 (≤ 4.1 ms; behind table zeroes) | max 318 ms (a host stall), 13.8 ms | max 3.5, 6.8 ms |
| ad97e3f4 + 1f1c88db, without 73895932 | FAIL 2/2 | 3, 10 (≤ 3.5 ms; across the mprotect gaps) | max 5.2 ms | max 3.0, 3.9 ms |
| **73895932 + ad97e3f4 + 1f1c88db** | **PASS 2/2** | **0** (max lw 0 µs) | max 3.0, 3.5 ms (mostly after the lock) | max 2.6, 4.0 ms |

The per-call time of munmap / MAP_FIXED barely changed, because draining and
the shootdown still happen inside the call. What changed is that they no
longer hold `vm_lock` while doing it.
- On the check tree 1f1c88db also passes: unit 464/0, tlbstale, tlb, pfpar,
  cow, forkcow, forkva, mmap_race, vmcommit, lowva, pemap, shm, exitgroup,
  manythr.
- Full gates on the tip: §18.

**The guard rail** `linux_probe_tlbstale` (96bef222).
- A worker keeps page P's translation cached on its CPU.
- The main thread makes P read-only, unmaps it, or replaces it with MAP_FIXED,
  then asks the worker for one access. The access must fault, or read the new
  zero page. 1000 rounds of each.
- PASS on every lane build. On a test build whose `vm_arch_shootdown` returns at
  once: `stale=1000` in all three cases.

## 12. H7-07: TLB shootdowns (23f2d8c9, d9b29fe0, 1dbad3fe, 9c032122, a3112535, 8649c0a3)

**Instrument (9d5430a9, 23459fe7).** `[PFS]` sorts every shootdown by its caller
(munmap, JIT unmap, mmap, mprotect, zap, MAP_FIXED, COW, fork, …). For each it
gives count / µs / max, targets, how many targets were running the address
space, and acks that took 1 ms or more (`slow`). `slow_infault=` counts late
targets whose thread was inside a page fault, and `slowcpu=` says which CPUs
were late.

**Who issues them, one fp720 boot of the tip (r2-m1):**

| phase (s) | site n / total ms / max ms | targets / running | slow acks |
|---|---|---|---|
| launch (120) | mmap 317 / 659 / 74.4; jit 307 / 458 / 34.8; mprot 671 / 57 / 2.4 | 2774 / 2761 | 160 |
| loading (80) | mmap 792 / 1328 / 46.0; jit 14 / 52 / 42.7 | 2527 / 2521 | 224 |
| world (101) | mmap 374 / 200 / 22.4 | 931 / 931 | 32 |

- Almost every target had the address space loaded. The "only loaded CPUs"
  cut was already in place: x64 shoots only CPUs whose CR3 is the root.
- `mmap` is MAP_FIXED over a mapped range (Wine's reserve-then-commit). JIT is
  munmap of code pages. Neither batches further: one shootdown per call already.
- Before vmzero, most slow targets were inside a page fault. A first touch of
  guest RAM is a host page fault with interrupts off, and the IPI waits it out.
  With vmzero (d9b29fe0) `slow_infault` is 0–4 per phase.

**What the late targets are doing now: writing to the console.**
- A console write runs with interrupts off. Each byte is a port exit that
  QEMU serves in user space: 11–55 µs per byte in r2-m1, more under load
  (§13). A `write()` from Wine's stderr goes out as one window of up to
  8 KiB under one interrupts-off hold.
- A CPU in that write cannot take the 0x82 IPI. The shooter waits until the
  write ends, tens of milliseconds.

**Fixes.**
- 23f2d8c9: KVM PV TLB flush. A target the host has descheduled (steal-time
  record `preempted`) gets its flush marked in the record and is not waited
  on. 1–9 per boot.
- 23f2d8c9: IRQ-off spin loops (the spinlock wait, the NFS receive wait)
  settle an owed flush every 64 spins.
- d9b29fe0 vmzero: frames are zeroed by a kernel task, so a target is rarely
  inside a first touch.
- 1dbad3fe: the console writer settles an owed flush at every 16-byte FIFO
  refill and while it waits for the wire (`arch/x64/uart.c`).

**A bug in vmzero, found by the gate batch (9c032122).**
- With the pool full, vmzero halted in a 2 ms loop and re-entered it at once,
  so it never gave its CPU back.
- A kernel task is never preempted: trap.c's timer paths preempt user mode
  only. A thread whose timer expired on that CPU is the CPU's next pick, so it
  waited for another CPU's 10 ms tick.
- `linux_probe_hrtimer` L1–L3 (1 ms sleeps beside 8 burners) failed: p50
  error 8992 / 8997 / 8998 µs, against integ22's 13 / 14 / 14 µs.
- Now vmzero halts once, until the next interrupt, then yields. When the pool
  is not full it yields after every 32 frames (was 256).
- hrtimer PASS 2/2: L1–L3 p50 error 21–24 µs. Two of 200 L1 samples still
  reached 9–10 ms, against 0 of 200 on integ22.
- The first lane A/B boot (r2-lane-fp1) ran with the bug. The A/B (§14) uses
  only boots from after the fix.
- **A second one (a3112535).** Between turns vmzero is READY, so a CPU kicked
  for a new or woken thread can pick vmzero first. With the pool full,
  vmzero then halted until that CPU's next interrupt, and the thread waited.
  - `linux_probe_ntsync` W8 measures a 100 ms timeout from the waiter
    thread's start. It failed in the gate batch on 1f1c88db (95.5 ms) and
    2 of 3 times on a check tree (97.5 ms).
  - With vmzero switched off it passed 3/3 (100.000–100.006 ms), and integ21
    passed 2/2.
  - Now vmzero halts only when no other thread is READY, and a turn ends
    after 32 frames or 200 µs.
  - ntsync 3/3 PASS (99.88–99.90 ms). hrtimer L1 p50 error is 15 µs.
- **A third (8649c0a3).** vmzero waited for "nothing else alive", so it
  outlived the Wine processes it serves. The harnesses count every process
  that appeared after their first census as left over: thrstorm S3 failed on
  the tip (`[K5] leftover pid=5 name=vmzero`). Now it exits once no
  Linux-ABI process is alive, and thrstorm passes S1–S4.
- All three bugs were in the lane's own new task, and the gates caught every
  one.
  The ext2 flusher (`ext2_vfs.c`, not this lane's) waits the same way, in a
  50 ms halt loop that never yields (§19).

**Probe** `linux_probe_conswait` + `lp_check_conswait.py` (9f95037c).
- Two writer threads write 2 KiB blocks of 80-byte lines to stdout.
- The main thread mprotects one page read-only and back, 500 times.
- The check fails when slow acks exceed 5 per 100 shootdowns, or one shootdown
  took 20 ms or more.

| linux_probe_conswait (strix) | verdict | slow acks / 500 shootdowns | shootdown total | max |
|---|---|---|---|---|
| before (d9b29fe0) | **FAIL 2/2** | 988, 993 | 14.7 s, 15.1 s (29.5 ms each) | 68 ms, 39 ms |
| 1dbad3fe | **PASS 2/2** | 0, 0 | 44 ms, 42 ms | 1.0 ms, 0.2 ms |

`linux_probe_shootwait` (16efeafc) loads the targets with faults and vm_lock
holds instead. It failed under host load before PV flush and vmzero, and
passes now. On a quiet host it passes either way, so it is a guard rail, not
proof (evidence: `shootdown-probes-strix.txt`).

## 13. H7-08: what a timer interrupt costs (measured; no fix)

`[PFR]` (7afc6dc7) times each part of the two timer paths. Vector 0x84 is the
per-CPU high-resolution timer. Vector 32 is the 100 Hz PIT tick, on CPU 0 only.

**r2-m1, world phase (101 s, 8 CPUs):**

| segment | n | total | avg | max |
|---|---|---|---|---|
| 0x84 EOI + `sched_sync_flush_ex` (hrt_flush) | 765 236 | 5.34 s | 7.0 µs | 2.1 ms |
| 0x84 `sched_hrt_fire` (hrt_fire) | 765 236 | 3.70 s | 4.8 µs | 5.5 ms |
| 0x84 kick / schedule (hrt_sched) | 119 428 | 2.08 s | 17 µs | 0.6 ms |
| 32: `cp_wait_sample` (t32_sample) | 10 098 | 0.18 s | 18 µs | 11 ms |
| 32: 5 s census `rw_thr_sample` + `virtio_gpu_health_tick` (t32_census) | 20 | 0.66 s | **33 ms** | **371 ms** |
| 32: everything after `cons_tick` (t32_rest, includes the two above) | 10 098 | 1.55 s | 153 µs | 382 ms |

- 0x84 fires about 950 times a second per CPU and costs 1.4 % of the 8 CPUs
  in all. That is steady overhead, not hitches; the max is a few ms.
- **The hitches are the 5 s census on CPU 0.** One census is 13–99 ms with
  interrupts off (`[PFR]` per 10 s: 26–131 ms for two). One reached 371 ms.
  Almost all of it is console output: about 1.1 KiB of `[THRS]` rows per
  census, plus `[THRW]` rows from `cp_wait_sample` every 5 s.
- Per 10 s interval, census time divided by census bytes is **11–55 µs per
  console byte**. That is the cost of one port exit to QEMU's 16550 on this
  host. conswait's writers measured 8.6 µs per byte on a quieter host.
  - So every console byte costs a vCPU 9–55 µs with interrupts off.
  - The 371 ms census probably waited for the wire behind another CPU's
    write: `cons_user_write` sends a whole `write()` window of up to 8 KiB
    under one hold.
- While CPU 0 is inside the census, the PIT tick, the 0x81 reschedule
  broadcast and every shootdown that targets CPU 0 wait. 1dbad3fe lets it
  settle the shootdowns.

**Proposals (not lane-owned: `trap.c` census call, `process.c`, `kconsole.c`).**
1. Build the census rows in the tick, and emit them from a kernel task or the
   idle loop, not the IRQ. Or emit them every 30 s.
2. `cons_user_write`: release the wire, and the interrupts-off hold, between
   lines of a window.
3. A faster console sink, e.g. virtio-console or a shared-memory ring that
   the host drains, so a byte is not a port exit.

## 14. A/B: round-1 tip vs round-2 lane (one tree pair, booted alternately)

**Setup.**
- base = 45832325, the round-1 tip and integ22's kernel (`~/cp/lanes/fault-base`).
- lane = 9c032122 (`~/cp/lanes/fault`).
- fp720 boots with audio, alternating base and lane, one game boot at a time,
  on strix with H6-hunt3's boots beside them. Then one soak each.
- `abtable.py` columns, all per boot:
  - `stalls`: NFS pipeline stalls, from the last `[NFSNET]` row;
  - `drop`: audio dropouts (wavgaps);
  - `zeros`: `[SND] zeros` rows;
  - fault ms and anon µs: `[PFK]`/`[PFL]`;
  - lw / lh: `vm_lock` wait and hold inside faults;
  - vmlw: `vm_lock` wait at every site;
  - shoot: TLB shootdown time.

| run | world fps med / p10 | prof-fp med / p10 | stalls | drop | zeros | fault ms | anon µs | lw ms | lh ms | vmlw ms | shoot ms | hitch |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base-fp1 | 26.6 / 24.7 | 29.8 / 25.6 | 0 | 4 | 5 | 14176 | 6.7 | 324 | 870 | 1587 | 5464 | 0 |
| base-fp2 | 24.9 / 21.5 | 31.3 / 29.2 | **3** | 5 | 6 | 13463 | 6.5 | 311 | 864 | 1664 | 5790 | 0 |
| base-fp3 | 23.9 / 21.9 | 30.9 / 29.3 | **2** | 5 | 6 | 13375 | 6.5 | 339 | 869 | 1812 | 7334 | 0 |
| base-fp4 | 24.9 / 15.8 | 31.6 / 30.4 | **5** | 8 | 10 | 14898 | 6.8 | 345 | 938 | 1617 | 6533 | 3 |
| base-fp5 | 34.8 / 28.2 | 35.3 / 33.0 | 0 | 5 | 6 | 13830 | 6.6 | 350 | 909 | 1649 | 5062 | 0 |
| lane-fp1 | 25.1 / 23.1 | 29.4 / 26.3 | 0 | 3 | 4 | 5538 | 2.2 | 183 | 491 | 1322 | 1440 | 0 |
| lane-fp2 | 24.1 / 20.7 | 27.3 / 21.5 | 0 | 2 | 3 | 5411 | 2.2 | 178 | 496 | 1268 | 1265 | 0 |
| lane-fp3 | 25.5 / 24.1 | 31.7 / 26.2 | 0 | 3 | 5 | 5770 | 2.2 | 165 | 495 | 1335 | 1264 | 1 |
| lane-fp4 | 30.7 / 25.4 | 39.0 / 37.2 | 0 | 3 | 4 | 5660 | 2.4 | 172 | 497 | 1323 | 1317 | 0 |
| **mean base (n=5)** | 27.0 / 22.4 | 31.8 / 29.5 | **2.0** (3/5 boots; 2 lost + 8 delayed in all) | 5.4 | 6.6 | **13948** | **6.6** | 334 | 890 | 1666 | **6036** | 0.6 |
| **mean lane (n=4)** | 26.4 / 23.3 | 31.9 / 27.8 | **0** (0/4 boots) | 2.8 | 4.0 | **5595** | **2.3** | 175 | 495 | 1312 | **1321** | 0.2 |

(Lane rows are `r2-lane2-fp*`. `r2-lane-fp1`, the boot before 9c032122, is
left out: 5841 ms of faults, 1412 ms of shootdowns, 0 stalls.)

**The final tip.** Two fp720 boots of a3112535, which adds 73895932,
ad97e3f4, 1f1c88db and a3112535 to the lane rows:

| run | world fps med / p10 | prof-fp med / p10 | stalls | drop | zeros | fault ms | anon µs | lw ms | lh ms | vmlw ms | shoot ms | hitch |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| lane3-fp1 | 25.8 / 23.3 | 31.0 / 30.2 | 0 | 3 | 5 | 5408 | 2.1 | 168 | 491 | 1384 | 1267 | 1 |
| lane3-fp2 | 23.5 / 21.4 | 31.2 / 30.0 | 0 | 2 | 4 | 5355 | 2.2 | 158 | 488 | 1420 | 1394 | 0 |

They match the lane2 boots: no crash, no stall, the same fault and
shootdown numbers. `[PFX]` faults that waited 1 ms or more: 9 and 5 (lane2: 8–10).

**Soaks (one each, about 27 min of world):**

| run | soak fps med / p10 | stalls | faults | fault ms | anon µs | lw ms | lh ms | vmlw ms | shoot ms | `[PFX]` lw ≥ 1 ms | hitch |
|---|---|---|---|---|---|---|---|---|---|---|---|
| base-soak1 | 33.2 / 26.9 | 2 (both delayed: late 70, 385 ms) | 3 414 625 | 18 559 | 5.0 | 459 | 1274 | 7178 | 10 523 | 12 | 2 |
| lane-soak1 | 32.6 / 29.3 | 1 (delayed: late 4 ms) | 2 732 701 | 6 688 | 2.0 | 188 | 577 | 6339 | 2 561 | 0 | 0 |

- **H7-05: no lost fragment in any lane boot.**
  - base: 2 of 5 fp720 boots had one each (fp2, fp4), with the signature
    `late_ms=-1 ipr=2/31424/52`.
  - lane: 0 in 5 fp720 boots and the soak. The five include the boot from
    before 9c032122, which already had the copy-out fix.
  - The other stalls are the delayed shape: the original reply arrived late
    (3–1171 ms, a host stall) and was used. base fp: 8 in 3 boots; lane fp: 0.
    Soaks: base 2, lane 1. The 50 ms first wait bounds them; they are not
    the RX race.
- **Fault time is 2.5× lower**: 13.9 s → 5.6 s per boot. An anonymous fault
  costs 6.6 → 2.3 µs, because vmzero has usually zeroed and first-touched the
  frame already.
- **Shootdown time is 4.6× lower**: 6.0 s → 1.3 s.
- `vm_lock` wait inside faults roughly halves: 334 → 175 ms. Hold inside
  faults: 890 → 495 ms.
- Audio dropouts 5.4 → 2.8 and `[SND] zeros` rows 6.6 → 4.0 per boot. That is
  in the right direction, but it is about the size of boot-to-boot noise
  (base 4–8).
- **fps did not move** (world median 27.0 → 26.4, p10 22.4 → 23.3). The
  outliers base-fp5 and lane-fp4 are host-quiet windows, not tree
  differences. Kernel fault and lock time were already a small share of a
  frame, so the cut shows in stalls and stutters, not in the median.

## 15. P8 time-to-menu: integ21 vs cp-fault under equal load

integ22's P8 lavapipe reached the menu at t=252/253 s, against integ21's
221/206 s. Asked: is that the lane or the load?

**Setup.**
- One integ21 tree (5f8c4b76, `~/cp/lanes/fault-i21`) and one cp-fault
  round-1 tree (45832325, the integ22 kernel, `~/cp/lanes/fault-base`).
- They boot P8 lavapipe at the same time, twice, from copies of the same
  seeded prefix and the same Wine stage.
- H6-hunt3's Venus game guest ran beside both pairs, so there were three game
  guests on the host.
- Times are game seconds since launch, from the harness's OCR shots. The
  shots are 15–35 s apart, so each time is only good to about ±20 s.

| pair | integ21 title prompt | integ21 menu | cp-fault title prompt | cp-fault menu |
|---|---|---|---|---|
| 1 | 307 | 370 | 293 | **340** |
| 2 | 216 | 263 | 231 | **277** |
| mean | 262 | 317 | 262 | 309 |

- Under equal load the lane is not slower: 30 s faster in pair 1, 14 s slower
  in pair 2, and both are within the shot spacing.
- No pipeline stall, `[NFSRS]` row or re-send in any of the four boots.
  Neither suspect fires:
  - the warm LIFO;
  - the 50 ms first wait. It can add re-sends only when a reply is lost, and
    none was.
- The load explains it. With three game guests on the host, both kernels took
  263–370 s. integ21's own batch had the host to itself and took 206–221 s.
- The lane's console is about 80 KB larger per boot (the `[PF*]` and
  `[NFSRTH]` rows): 0.7–4.5 s of vCPU time at 9–55 µs per byte (§13). That is
  small next to the load effect, but it is not zero (§16).

Evidence: `docs/cp-fault-evidence/p8-pair-integ21-vs-lane-strix.txt`.

## 16. Instrument verbosity: should the [PF*] rows default to off?

Every console byte costs the writing vCPU 9–55 µs with interrupts off (§13),
so a row's cost is its bytes. r2-m1's console, 1.24 MB in 365 s, by source:

| rows | bytes | share | when |
|---|---|---|---|
| `[NFSRTT]` + `[NFSNET]` + `[NFSRA]` + `[NFSRTH]` | 506 KB | 41 % | a set every 8 MiB of NFS READ, in the reading thread |
| `[X]` (user stdout / stderr, mostly Wine) | 265 KB | 21 % | per write |
| `[PUBS]` | 82 KB | 7 % | |
| `[THRS]` + `[THRW*]` | 114 KB | 9 % | 5 s census, CPU 0 timer IRQ (§13) |
| `[AWAY]` | 54 KB | 4 % | |
| `[PF*]` row sets (`[PFK]`/`[PFH]`/`[PFL]`/`[PFI]`/`[PFT]`/`[PFS]`/`[PFR]`/`[PFE]`) | 37 KB | 3 % | about 1.0 KiB every 10 s |
| `[PFX]` (long single faults, budget 400) | 32 KB | 2.6 % | the first 400 faults over 2 ms |

- **[PF*] row set.** About 1 KiB every 10 s. It is printed by whichever thread
  takes the first fault after the interval, on its way out of the trap. That
  costs that thread 9–57 ms, and it can be the render thread: one
  hitch-sized stall every 10 s.
- **[PFX].** The budget is spent in the first two minutes of a boot, in
  bursts during launch.

**Recommendation.**
- Default **off** for the detail rows: `[PFH]`, `[PFI]`, `[PFT]`, `[PFS]`,
  `[PFR]`, `[PFE]` and `[PFX]`. Put them behind a CP_DIAG class `pf`, checked
  at the source (before the row is built). The harness turns it on for
  fault-measurement runs: fp.sh, pfx_gate, the probes.
- Keep a one-line `[PFK]` + `[PFL]` summary on by default, every 60 s, about
  280 B. That is cheap enough to tell from any gate log whether faults were
  the problem.
- If `[PFX]` stays on, use a 5 ms threshold and a budget of 100.
- **The bigger saving is not mine.** The NFS periodic set is 41 % of the
  console and runs in the reading thread during loading. Every 64 MiB instead
  of every 8 MiB would cut it 8×. My `[NFSRTH]` row is 14 % of that set and
  could print only with a stall. Second biggest: the census in the timer IRQ.

## 17. Findings (round 2)

Round-1 IDs whose status changed, and new ones.

| ID | symptom | rate (k/n, boots) | first seen | signature | layer | severity | status |
|---|---|---|---|---|---|---|---|
| H7-05 | the lost reply fragment: the virtio-net RX loops hand a descriptor back before reading its frame; a reader stalled there reads a later frame | repro: rsize_guard G12 2/2 without the fix (25 and 31 stalls, one wrong byte); game boots: lost stalls 2/5 base → 0/6 lane | `rx-requeue-repro-strix.txt` | `[NFSRS] … ipr=6/31424 late_ms=-1 resend_ms=0–2`, `rxq_reuse` moving with the stalls | kernel (virtio_net.c) | perf (50 ms pause per stall) + **data** (a torn frame can land in a reply: no IP/UDP checksum) | fixed in f83a2dfe (G12 0 stalls, every byte right) |
| H7-07 | TLB shootdowns wait for targets that have interrupts off: inside a first-touch fault, spinning on `vm_lock`, descheduled by the host, or writing to the console | r2-m1: 417 slow acks in 1 boot; conswait 2/2 FAIL; shootwait 1/1 FAIL under load | `shootdown-probes-strix.txt`, `[PFS]` | `[PFS] … mmap=792/1328171/45952/2482/2476/219`; conswait `988 slow target acks … max 67942 us` | kernel (x64 mmu.c, uart.c) + host scheduling | perf (up to 74 ms per shootdown) | cut: PV flush + IRQ-off polls 23f2d8c9, vmzero d9b29fe0, console polls 1dbad3fe; per-boot numbers §14 |
| H7-08 | timer interrupt cost: 0x84 is steady (1.4 % of CPUs); the hitches are the 5 s census in CPU 0's PIT IRQ, 13–371 ms with interrupts off, almost all console output | 1/1 measured boot (r2-m1), every 5 s | `[PFR]` in r2-m1 | `t32_census=20/658089/370923` | kernel (trap.c, process.c census, kconsole.c) | perf (hitch every 5 s on CPU 0) | measured; proposals in §13 (not lane-owned) |
| H7-10 | `vm_mprotect` / MAP_FIXED / munmap hold `vm_lock` for the whole span (6–9 ms on 256 MiB); then the chunk gap that let nobody in (test-and-set lock), and new page tables zeroed under the lock | vmhold 2/2 FAIL (96bef222), 1/1 FAIL in the gate batch (96f11afd), 2/2 FAIL on the check tree | `shootdown-probes-strix.txt` | `lp_check_vmhold: FAIL -- 17 long fault row(s) … max lw 5273 us`; `lh=2082 zero=2081` | kernel (vm.c, x64 mmu.c) | perf | fixed: 96f11afd (chunked mprotect, one walk per 2 MiB), 73895932 (fair gap), ad97e3f4 (tables from the pool), 1f1c88db (unhook whole tables); vmhold PASS 2/2, max lw 0 |
| H7-11 | a console byte costs the writing vCPU 9–55 µs with interrupts off (a 16550 port exit served in QEMU user space). `cons_user_write` sends a whole `write()` window (up to 8 KiB) under one hold | every boot; 1.24 MB per fp720 boot | §13, conswait | 165 844 B in 1.43 s (8.6 µs/B, quiet); census 11–55 µs/B | kernel (kconsole.c, fs.c) + QEMU serial | perf (every row is a stall of its writer) | shootdown side fixed (1dbad3fe); the rest is the console owner's (§13 proposals, §16) |
| H7-12 | the NFS periodic counter set (`[NFSRTT]`/`[NFSNET]`/`[NFSRA]`/`[NFSRTH]`, 1.45 KB every 8 MiB of READ) is 41 % of the console and is printed by the reading thread | 1/1 counted (r2-m1: 348 sets) | §16 | `[NFSRTT] periodic want=32768 …` every 8 MiB | kernel (nfs.c instrument; `[NFSRTH]` is this lane's) | perf (12–80 ms per set during loading) | open: recommendation in §16 |
| H7-14 | **the ext2 flusher keeps one of the guest's 8 CPUs for as long as the data disk is mounted.** It waits in a 50 ms halt loop that never yields, and a kernel task is never preempted. Every P9 game boot mounts the data disk, so the game runs on 7 CPUs | linux_probe_cpus with an ext2 data disk: FAIL 4/4 (integ21 6.85 / 6.93, tip 6.86 of 8); without: PASS 4/4 (7.76–7.83) | `cpus-ext2-flusher-strix.txt` | `[LP] U1 … cpus_used=6.85 of 8 FAIL` | kernel (ext2_vfs.c `flusher_task`; not this lane's) | perf (1/8 of the guest's CPU) | open: patch proposed (§19); with it 4/5 PASS on a check tree |
| H7-13 | integ22's P8 lavapipe time-to-menu (252/253 s vs integ21's 206–221 s) | integ21 vs cp-fault booted together ×2: menu 370/263 vs 340/277 | `p8-pair-integ21-vs-lane-strix.txt` | no stall, no re-send in 4/4 | host load (3 game guests) | none (not a regression) | closed: load, not the lane |

## 18. Gates (round 2)

Three runs, because the gates kept finding things:
- `gates2.sh` (integ-gates21.sh pointed at `~/cp/lanes/fault-dev`) on
  1f1c88db, up to the infra gates;
- `tip-rerun2.sh` on a3112535 (+ the cpus probe and the castscan entry) in
  `~/cp/lanes/fault-chk`;
- `final-gates.sh` on 8649c0a3, for everything its vmzero exit change can
  touch.

Two earlier `gates2.sh` starts were stopped when they found the vmzero CPU
hog (hrtimer, on 1dbad3fe) and the mprotect-gap unfairness (vmhold, on
9c032122). Raw rows: `gates-r2-strix.txt`.

| gate | lane (tree) | integ22 / round-1 reference |
|---|---|---|
| build (intel test) | PASS (all three) | PASS |
| unit | 464/0 (1f1c88db, a3112535, 8649c0a3) | 464/0 |
| probes | **76/76** on a3112535: the 71 of round 1 + tlbstale, vmhold, shootwait, conswait, cpus, each with its console check. 1f1c88db: 75/76, ntsync W8 failed → a3112535. On 8649c0a3 again: hrtimer, ntsync, cpus, vmhold, pfpar, exitgroup, shootwait, conswait, tlbstale PASS | 71/71 |
| sigmask-gate | 8/8 (1f1c88db) | 8/8 |
| ext2_gate | PASS (1f1c88db) | PASS |
| exec_gate | PASS (1f1c88db); PASS (8649c0a3) | PASS |
| rsize_guard (incl. G12) | PASS (1f1c88db); PASS (8649c0a3) | PASS (G12 new) |
| rsize_scan | PASS | PASS |
| nfs_proto_test_host | PASS | PASS |
| host_tests | 40/42 (the known gaming.md pair), kconsole PASS | 40/42 |
| castscan | 1 new site on 1f1c88db (vm_lock_gap's lock-address compare), allow-listed in efb9e300; 0 new on a3112535 | 0 new |
| ARM build | PASS (1f1c88db, a3112535; also the intermediate trees 73895932 and ad97e3f4, x86 and ARM) | PASS |
| panic-sink allowlist | PASS | PASS |
| pfx_gate | PASS (1f1c88db) | PASS |
| ext2w_gate | PASS (1f1c88db); PASS (8649c0a3) | PASS |
| Gate P2 ×2 | G1/G2/G3 PASS ×2 on 8649c0a3 (G3: ps-extra count=0) | PASS |
| Gate P5 | vkcube, d3d11, d3d12, fullscr PASS on 8649c0a3 (boot 274 s) | PASS |
| thrstorm | a3112535: T1–T3 and S1/S2/S4 PASS, **S3 FAIL** (`leftover pid=5 name=vmzero`) → 8649c0a3: S1–S4 PASS | PASS |
| Gate P8 Venus | main menu at s010-t000130 (130 s game time, +155 s host), verdict main-menu, on a3112535 with H6-hunt3's Venus guest beside it | integ22 129 s, integ21 114 s |
| P8 lavapipe | §15: 340 / 277 s beside integ21's 370 / 263 s, booted together (3 game guests) | integ22 252/253 s (2 guests) |

**Commands (strix).**

```
# probes added in round 2 (console checks run by t3_linux_probe.sh)
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh tlbstale
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh vmhold
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh shootwait
LP_TIMEOUT=300 bash tools/cp/qemu-slot.sh bash tools/gate/t3_linux_probe.sh conswait
# H7-05 repro / guard (G12: rxpress under .cp-nfs-inject.rxstall.2000)
bash tools/cp/nfs/rsize_guard.sh
# A/B and the fault/timer/shootdown summary of one boot
cd ~/cp/lanes/fault && FAULT_AUDIO=1 bash tools/cp/fault/fp.sh fp720 <run>
python3 -I tools/cp/fault/abtable.py ~/cp/lanes/fault-logs r2-base-fp1 ... r2-lane2-fp4
python3 -I tools/cp/fault/pfsum.py <serial.txt> --phases <phases.txt>   # [PFS]/[PFR] included
```

## 19. What is left (round 2)

- **H7-10 leftovers.** Partial 2 MiB tables at the ends of a span are still
  zapped in place, and the drain plus shootdown still run inside the call.
  `vm_lock` is test-and-set: `vm_lock_gap` works around it at the one chunked
  site, and a ticket lock would fix it everywhere (locks.c, not this lane's).
- **The console (H7-11, H7-12, H7-08).** It is the largest interrupts-off cost
  left, and none of it is this lane's code (proposals in §13 and §16):
  - census emission out of the PIT IRQ;
  - `cons_user_write` releasing the wire between lines;
  - the NFS periodic set every 64 MiB;
  - a faster sink.
- **Slow shootdown acks with no cause counted.** After 1dbad3fe, a slow ack is
  most likely a vCPU the host descheduled with interrupts on, between steal-time
  updates. PV flush covers it only once the host has set `preempted`.
- **Host first touch.** vmzero moves it to a kernel task. The host cost is the
  same; only hugepage- or prealloc-backed guest RAM (a host setting: ask the
  user) removes it.
- **H7-14, the ext2 flusher's CPU** (not this lane's file). `flusher_task`
  ends each pass with `while (timer_get_ms() - t0 < 50) safe_wfi();` and
  never yields, so one vCPU runs only the flusher once `/EXT` is mounted.
  - linux_probe_cpus (eded7bbd) shows it: with the data disk, 8 busy threads
    get 6.85–6.93 CPUs; without it they get 7.76–7.83.
  - The fix has the same shape as vmzero's (a3112535): halt only while no
    thread is READY, and yield after every halt.
  - Patch: `docs/cp-fault-evidence/ext2-flusher-yield.patch` (not applied on
    this branch). On a check tree with the data disk: 7.69 / 7.71 / 7.81 /
    7.80 of 8, PASS 4 of 5; one run read 6.91. Without the patch: 0 of 4.
  - This is likely the largest CPU-side lever left for the game. The owner
    should apply it and run ext2w_gate.
- **H7-06** (silent TX drop after 2 ms with a full ring) is unchanged and still
  counted (0 in every boot).
- **The CPU-time clocks.** `CLOCK_PROCESS_CPUTIME_ID` read 65 s over 2 s of 8
  busy threads (4× wall time per thread), and `CLOCK_THREAD_CPUTIME_ID` read 0 in
  a thread that had been running. Not investigated: `linux_compat` clocks.
