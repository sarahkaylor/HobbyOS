# L76 — the completion contract behind `Initializing.`, and the queue that never drains

M6/L76, worktree `~/hobbyos-rw-lanes/l76-handshake` (branch `rw-l76-handshake`,
seeded at the merged tip `913d80a0`). L72 named a "three-party handshake"
(WAIT AsyncRead → WAIT gfx → WAKE main→both → WAIT main → WAKE AsyncRead→main →
re-arm) and read it as a protocol livelock: the AsyncRead step reports done and
immediately re-arms, so `LongEventHandler` never observes completion. L73
carried that reading forward. This lane decompiled the game, sampled every
thread's managed stack, and counted what the kernel is actually asked for.

## 0. Summary

| question | answer |
|---|---|
| what does `LongEventHandler` wait to observe? | **`eventThread.IsAlive`** — a managed `Thread`'s exit (`verse/LongEventHandler.cs`, decompiled). No frame counter, no vsync, no async-read counter participates in the contract |
| so is the futex ring a completion protocol? | **no.** It is Unity's *synchronous resource load*: `Resources.Load` submits a read, the `Loading.AsyncRead` worker completes it, the gfx worker takes the upload. Six ops per load, one load per round — the same ring shape a livelock would have, with none of the semantics |
| what is actually stuck? | `LongEventHandler.ExecuteToExecuteWhenFinished` on the **main** thread, running the post-load queue: `ThingDef.PostLoad` → `GraphicData.Init` → `GraphicDatabase.Get` → `ContentFinder<Texture2D>.Get` → `Resources.Load` → `ResourcesAPIInternal.Load` (live sample, §2) |
| is the folder enumeration endless? | **no.** 173 directories enumerated by the game, 173 match `find` on the host exactly, 0 duplicate names inside any listing (§3) |
| is it frame-count/vsync paced? | **no.** In the same window the ring moves 24 rounds/s while the game presents 0.8 frames/s, and `Application.targetFrameRate` is set from a refresh rate we report as 0 (uncapped). A frame-paced contract cannot run 30× faster than frames (§4) |
| what is ours? | two things, and only one is on the critical path: (a) a LOOKUP failure that is *transport*-shaped is reported as ENOENT, so a lost UDP reply becomes "Could not find file" for a file that exists — that is what kills runs (§6); (b) the NFS read path keeps no cache, so every 4 KiB page pays its own round trip — fixed and measured, but it is ~20 % of the plateau's round, not the wall (§5) |
| result | see §6 |

## 1. The contract, from the game's own code

`Verse.Root.Start()` queues two long events when play data is not loaded:

```csharp
LongEventHandler.QueueLongEvent(() => PlayDataLoader.LoadAllPlayData(),
                                null, doAsynchronously: true, null);
LongEventHandler.QueueLongEvent(action, "InitializingInterface",
                                doAsynchronously: false, null);
```

The asynchronous event runs on a **managed thread** the handler creates
(`UpdateCurrentAsynchronousEvent`), and completion is that thread's death:

```csharp
if (eventThread == null) { eventThread = new Thread(() => RunEventFromAnotherThread(...)); eventThread.Start(); }
else {
  if (eventThread.IsAlive) return;          // <-- the completion contract
  ...
  ExecuteToExecuteWhenFinished();
  currentEvent = null;
}
```

`PlayDataLoader.DoPlayLoad()` sets the splash text to `"Initializing"` at its
**tail** (`SetCurrentEventText("Initializing".Translate())`, right before
`KeyPrefs.Init()` and `ShortHashGiver.GiveAllShortHashes()`), queues its
post-load closures, returns, and `loadedInt = true`. So a screen that says
`Initializing.` means the loader got through every def and every keyed string
and is inside the last two steps or the post-load queue.

`LongEventHandler.ExecuteToExecuteWhenFinished()` runs the whole queue
**synchronously on the main thread**, one closure after another, in a single
`Root.Update` frame. That is where the run stops.

## 2. The live sample: what main is doing at the wall

`/TPLATE` (late stack sampler, SIGUSR2 capture + `mono_jit_info_table_find`),
t=380 s, tid=4 (main), from `obj/rw-desktop/l72.log`:

```text
[TPWA] tid=4 ... bt=Object.runtime_invoke_void <- ResourcesAPIInternal.Load
  <- ResourcesAPI.Load <- Resources.Load <- Resources.Load <- ContentFinder`1.Get
  <- ... <- Graphic_Single.Init <- GraphicDatabase.GetInner <- GraphicDatabase.Get
  <- GraphicData.Init <- GraphicData.get_Graphic <- ThingDef.<PostLoad>b__398_0
  <- LongEventHandler.ExecuteToExecuteWhenFinished
  <- LongEventHandler.ExecuteToExecuteWhenFinished
```

`Resources.Load` is Unity's *synchronous* load. Each call submits a read to the
`Loading.AsyncRead` worker and blocks; the gfx worker takes the upload. That is
the six-op cycle L72 recorded, one cycle per texture:

```text
WAIT AsyncRead   (parks for work)      WAKE main -> AsyncRead   (submit)
WAIT gfx         (parks)               WAKE main -> gfx         (upload)
WAIT main        (waits for the load)  WAKE AsyncRead -> main   (load done)
                                       WAIT AsyncRead           (re-arms for the next)
```

Every wake lands because every wake is *supposed* to land: this is a request /
completion transport at ~24 requests/s, not a stranded handshake.

## 3. The enumeration, counted against the host

`[GDT]` (one row per directory the game starts reading) from L73 run C: 400
rows, 173 distinct directories. Compared with `find` on the same export:

```text
distinct dirs 173, mismatches 1  (the export root, +1: the harness's own launcher)
```

L76 adds `[GDC]` (per-directory windowed census: `fresh` recounts, `calls`,
`ents`, `total`, `dup`) and a duplicate-name detector inside one listing. Across
the runs of this lane: `dup=0` everywhere, and every `total` matches the host.
The folder walk is correct and finite; it is not the loop.

## 4. The frame-count / vsync hypothesis, measured

Three independent readings, all against frame pacing:

1. **The contract** (§1) has no frame term at all.
2. **Rate ratio in one window**: `[FUTR]` t=520 s, `0x1083B18624:508w507k` —
   508 waits + 507 wakes in a 21 s window = ~24 rounds/s. The same run's
   `[XSW]` rows present at 0.8/s (`n=230` at `t=285 s` of the game clock). 30
   rounds per presented frame.
3. **`Application.targetFrameRate`** is `Mathf.RoundToInt(Screen.currentResolution.refreshRateRatio.value)`
   (`Root.CheckGlobalInit`). Our compat Xlib answers `XRRGetScreenInfo` with
   NULL and `XRRConfigCurrentRate` with 0, i.e. the engine is not given a
   refresh rate to cap to; there is no counter we fail to increment.

The present path is already synchronous — every `[XSW]` row has
`put == putok == swap` — so "make glXSwapBuffers block until the buffer is
flipped" is already true.

## 5. The read path: one round trip per 4 KiB page, and no cache (a cost, not the wall)

`[RDF]` (per-file NFS read census) shows the loader's reads are **4 KiB
`read()`s** (`len=4096`), and `[NFSIO]` gives the aggregate: 49.6 MB in
40 526 RPCs over 390 s = **127 KiB/s**, 104 RPCs/s. The transport's rsize is
1344 bytes (one UDP datagram), so a 4 KiB read is three RPCs and one round trip;
nothing caches the page, so the texture queue pays a round trip per page, tens
of thousands of times.

Fix (`src/kernel/fs.c`) — implemented and measured, but see the honest reading at
the end of this section:

* `l76_nfs_read()` — a **sequential read-ahead window**: when the fd's previous
  read ended exactly at this offset, fetch a 64 KiB window in one pipelined
  `nfs_read_file()` (48 RPCs, 16 deep) and serve the following reads from it.
  A random-access file never trips it.
* a 32-slot **page cache** for whole aligned 4 KiB reads (bundle index blocks
  Unity re-reads per asset).
* `[RAC] bc_hit= ra_hit= ra_fill= direct=` — the counters, printed with the
  `[RDF]` window, so "the cache helped" is a row, not a hope.

Measured in R2's plateau: `[RAC] bc_hit=0 ra_hit=241 ra_fill=71 direct=2845`
— the sequential window fires and serves 7.8 % of reads (3.4 reads per fill);
the page cache never hits, because the bundle reads are not repeated whole
pages.

**The honest reading:** the plateau's round did not get measurably faster
(S3 52.7 rounds/s pre-fix, R2 46 rounds/s post-fix, different windows and host
load). `[RDF]` says why: the plateau moves ~23 KiB of reads per round
(~1 MB/s at 46 rounds/s) while the transport can do ~5 MB/s — the read is
roughly a fifth of the 21 ms round. The rest is Unity's own per-texture work
(decode + upload on this guest), which is not ours to make faster. The cache
is worth keeping — it removes a round trip per page for every sequential
reader — but it is not the wall, and this lane does not claim it is.

## 6. The death that ends the run before the queue can drain

The queue is long, but length alone did not end these runs: the process dies.
Every 1800 s attempt in this lane ended the same way, and it is a *file lookup*:

```text
---> System.IO.FileNotFoundException: Could not find file
     "/NFS/Data/Biotech/Languages/English/Keyed/WITabs.xml"
  at System.IO.FileStream..ctor
  at RimWorld.IO.FilesystemFile.ReadAllText
  at Verse.LoadedLanguage+<>c.<LoadData>b__42_1 (RimWorld.IO.VirtualFile x)
  at System.Linq.Parallel.SelectQueryOperator`2...SpoolingWork
```

`WITabs.xml` exists on the host, was opened earlier in the same run, and the
same failure shape appears with `Races_Animal_BigCats.xml` (S2) and with
`MemoryMappedFiles.MemoryMapImpl.Open`. One run later:

```text
Could not execute post-long-event action. Exception: System.NullReferenceException
  at Verse.GraphicData.Init ()  ... at Verse.ThingDef.<PostLoad>b__398_0
  at Verse.LongEventHandler.ExecuteToExecuteWhenFinished
[SIGDUMP] SIGSEGV code=1 addr=0x0 ... rip=0x10b8d29f52 (JIT chunk)
[RWMAIN] signal() sig=11 handler=(nil)
```

Why an existing file answers ENOENT: `nfs_lookup()` walks the whole path from
the mount root, **one LOOKUP RPC per component, each on a freshly created
socket** (`rpc_call()` calls `net_socket_create()` per attempt). One run
created **28 672 sockets** (`[NSOCK] created=28672 … inuse=2`) and had to skip
an ephemeral port (`[NSPORT] skipped=1 for port=49161`). `lookup_one()` returns
`-1` for a transport/parse failure and `-2` for the server's NOENT, and
`nfs_lookup()` collapsed both to `-1`, which `sys_open` reports as `ENOENT`.
A lost or mis-demultiplexed UDP reply therefore becomes "file does not exist"
— for a file that is there.

### The fix, ours

1. `nfs_lookup()` (`src/kernel/nfs.c`): a component whose LOOKUP failed in a
   *transport* way (`-1`) is retried up to three times; only the server's own
   NOENT (`-2`) is returned as ENOENT. `[NFLK] retry comp=… tries=… rc=…
   total=…` is the row.
2. `l76_nfs_read()` (`src/kernel/fs.c`): the 64 KiB sequential read-ahead window
   and the 32-slot page cache (§5), so the queue's per-texture reads stop
   paying a round trip each.

## 7. Ladder

Every row is the desktop harness (`tools/rw/run-rimworld-desktop.sh`,
`--timeout 480 --run-timeout 1800`, `RW_WAIT_SECS=1900`), softpipe.

| run | kernel | `[XO]` | plateau | present rate | end state |
|---|---|---:|---|---:|---|
| L73 run C (reference) | L73 | 1205 | handshake live | 0.8/s | splash, killed at t≈780 s |
| S1 (scout, `/TPLATE`) | L76-pre | 1209 | — | 0.8/s | managed sample at t=380 s names the queue (§2); dies in `GraphicData.Init`, t≈440 s |
| S2 (scout, `[RDF]`) | L76-pre | 559 | — | — | `FileNotFoundException … Races_Animal_BigCats.xml` |
| S3 (scout) | L76-pre | 1209 | **60 rounds/s** (`[FUTR]` t=504: `0x113000A460:221w220k`), 748 reads of `resources.assets` + 718 of `.resS` per window | 0.8/s | kernel wedged on a block-device reset, t≈560 s |
| **R1** | L76-fix-a (`49a25…` without the lookup retry) | 1197 | — | 0.8/s | `FileNotFoundException … WITabs.xml` → NRE in `GraphicData.Init` → SIGSEGV, t≈430 s |
| **R2** | L76-fix-b (`49a2515…`) | 1206 (last `[XO]` = `Biotech/…/Keyed/GameplayCommands.xml`) | **46 rounds/s** (`[FUTR]` t=552: `0x1100000334:1029w1030k`, `0x1083B18624:1031w1030k`), `[RAC] ra_fill=71 ra_hit=241 direct=2845` | **0.85/s** (260 presents / 305 s) | **#UD executing a zero page of UnityPlayer.so** (§8), t≈560 s |

Both 1800 s attempts reached `Initializing.` and the plateau, and both ended in
the same *class* of failure the ladder has been fighting since L72: a page of
UnityPlayer's text delivered as zeros. R2 is the sharper record — the page was
published **as verified**:

```text
[TXT] PROBE off=0x9AD000 … | raw rc=0 r_nz=3547 r_sum=0x65E51 | dropped=8 refill n=4096 f_nz=0
[TXT] REPLAY off=0x9AD000 -> read 1 non-zero, read 2 all-zero (device replay): publishing read 1
[TXT] PROBEGET off=0x9AD000 -> … published the device's bytes
[PUB] who=fault_obj va=0x10824E2000 off=0x9AD000 prot=RX priv=1 src=file zero=4096 ver=1
[KERNEL] User process fault! Vector: 6 RIP: 0x10824e2ea0
[UDX] rip=0x10824E2EA0 off=0x9AD000 … pghd=0x0000 0x0000 …
```

`raw_nz=3547` says the probe's own cache-bypassed read was the file's real
bytes, and `zero=4096 ver=1` says what was published is 4096 zeros: the probe
answered 1 and the page that reached the faulting instruction is not the page
it read. The gate's publish path between `fat16_page_probe()` returning 1 and
`vm_frame_copy(dst, pb, FRAME_SIZE)` is where those two diverge — the L73 run-B
shape, one layer lower. Naming the exact line is the next lane's first job; it
is stated here rather than papered over: **`Initializing.` was not passed in
this lane.**

## 8. Regression

| gate | result | log |
|---|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **433 run, 0 failed — UNIT TESTS PASSED** | `obj/l76-units.log` |
| `tools/gate/m3_window_probe.py` (`m3_window`) | **PASS** (27 s) | `obj/l73/gates/m3_window.log` |
| `tools/gate/m4_gl_probe.py` (`m4_gl`) | **PASS** (71 s) | `obj/l73/gates/m4_gl.log` |
| `tools/rw/run-rwnfs-probe.sh` (`rwnfs`) | **PASS** (17 s) | `obj/l73/gates/rwnfs.log` |

Run with `bash tools/rw/l73-gates.sh m3_window m4_gl rwnfs`; the unit tier and
the desktop harness were never run concurrently.

## 9. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l76-handshake
# the rows this lane is about
tools/rw/l76-handshake.py obj/rw-desktop/serial-desktop.log   # FUTR / GDC / RDF / RAC
tools/rw/l76-msample.py   obj/rw-desktop/l72.log              # managed stack per thread
# a run with the managed sampler (it stops the watchdog after ~7 min: use it
# for the t=380 s sample, not for a ladder)
RW_TPLATE=1 RW_WAIT_SECS=1900 tools/rw/run-rimworld-desktop.sh --timeout 480 --run-timeout 1800
# the ladder as measured
tools/rw/run-rimworld-desktop.sh --timeout 480 --run-timeout 1800
```

The `[TPLATE]` sampler sends SIGUSR2 to every live thread and the process died
in libmono at t≈440 s in that run (S1); the ladder rows use no sampler.


## 10. What the next lane should do first

1. **The publish path, not the probe.** In R2 the probe's cache-bypassed read
   held 3547 non-zero bytes of `UnityPlayer.so` at `off=0x9AD000`, the probe
   answered 1, and the page that was published is 4096 zeros
   (`[PUB] … zero=4096 ver=1`, `[UDX] pghd=0x0`). Read
   `vm_read_page_from_device()` from the `[TXT] PROBEGET` branch to the
   `[PUB]` row and find where `dst` stops being the frame the probe filled; the
   `[UDX]` row already prints the leaf's physical page and `same=1`, so the
   divergence is in the kernel, not in the device.
2. **Do not let a transport failure become ENOENT anywhere else.** `nfs_lookup`
   is fixed; `stat`/`open`/`readdir` have the same `-1` collapse in places
   (`nfs_getattr`, `nfs_is_dir`, `dir_cache_fill`), and each one can surface as
   "file does not exist".
3. **The plateau is CPU, not I/O.** ~46 rounds/s, ~23 KiB of reads per round,
   ~1 MB/s against a ~5 MB/s transport. Instrument Unity's texture path (or run
   the plateau with `RW_QEMU_GL=1` and compare rounds/s) before optimising the
   read path further.
