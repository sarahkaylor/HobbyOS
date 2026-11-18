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
