# L69 — the zero page is a **poisoned block-cache frame**, and the gate that
# refused it now asks the device instead of the cache

L67 killed the block wedge and left one question: with the controller enabled
and ready (`devoff=0 resets=0 cc0=0`), a full 4 KiB of zeros was delivered for
an executable page whose file holds code, the exec gate refused it three times
(`[TXT] ZERO off=0x757000 reads=3 n0=4096`), and the process died at the same
VA. This lane enumerated every refusing site, proved which of the four delivery
hypotheses holds, and fixed it at the three layers it exists in: the driver,
the block cache, and the exec gate.

## 1. The refusers

Two gates refuse an executable page. Both fired in the instrumented window, on
two different files and two different loaders, and both have the same cause.

| # | row | file | file offset | VA | mapping |
|---|---|---|---:|---|---|
| 1 | `[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published` | `/RWDSO/UnityPlayer.so` (33 010 672 B, md5 `45408fc1…fbadd`) | `0x757000` | `0x108228B570` | base `0x1082267000`, len `0x17A4000`, `prot=5` (R|X), `kind=7` (`VMK_LIB`), `MAP_PRIVATE`, `obj_off=0x733000`, obj `VM_OBJ_FILE` (FAT16, chain 15782) |
| 2 | `[TXT] IMG SUM pos=0x10000 got=0x76EFDDC5 want=0xDC910CE7 -> not published` | `/RWLAUNCH.BIN` (246 520 B) | `0x10000` | `0x1000010EA0` | base `0x1000000000`, len `0x3D000`, `prot=7` (RWX), `kind=1` (`VMK_IMG`), `MAP_PRIVATE`, `ooff=0` |

`n0=4096` on (1) rules out an end-of-file short read (`fat16_read_direct_locked`
clamps to the file size). Neither is a hole: both regions are present and
permissive, and `[TXT] UNVERIFIED` never fired.

## 2. Which delivery hypothesis holds — the numbers

### (a) A genuinely sparse region of the file — **refuted**

The page the guest refused is code in the file, and the *device* returns exactly
that page:

```text
file  /srv/nfs/.../UnityPlayer.so   0x757000: nz=3754 sum=0x6A3B1
      head 50 f3 0f 11 44 24 04 e8  (movss %xmm0,0x4(%rsp); call …)
guest [TXT] PROBE … lba=0x7B6D8 … | raw rc=0 r_nz=3754 r_sum=0x6A3B1
```

`nz` and `sum` agree byte-for-byte: the device answered with the file's page.
For refuser 2 the same holds for the witness: `want=0xDC910CE7` is the FNV-1a
of `rwlaunch.bin[0x10000:0x11000]` computed on the host, so the load-time sum
table is over the right page.

### (b) A short read from NFS — **refuted**

Both refusers are FAT16 files (`/RWDSO` and the image root), not NFS, and both
reported full-length reads (`n0=4096`, and the image fill's `n == want`).
No `[NFSRD] fail`, `[NFSPIPE] stall` or `[ZPFILL]` row appears in the window.

### (c) A poisoned page-cache frame — **CONFIRMED**

One row names the state and three more name its provenance:

```text
[BLKQ] device off cid=44153 op=0x2 lba=0x7B6DE since_ms=22
       cc=0x0 csts=0x0 … cc0_writes=0 n=1
[BLKQ] wedge-device-off cid=44153 … CSTS=0x0 CC=0x0 … reqs=109588
[BLKQ] controller reset #1
[BLKQ] recovered cid=44154 us=53000 resets=1
[BLKQ] re-read after recovery cid=44155 lba=0x7B6DE
[BLKQ] untrusted completion re-read lba=0x7B6DE n=1 status=0x0
[BLKC] distrust unit=63195 (reset during fill) -> frame dropped, re-reading
[TXT] PROBE off=0x757000 … lba=0x7B6D8 len=4096
       | cache found=1 fstate=0 valid=0xFF c_nz=0 c_sum=0x0
       | raw rc=0 r_nz=3754 r_sum=0x6A3B1
       | dropped=8 refill n=4096 f_nz=0 f_sum=0x0
[TXT] ZERO off=0x757000 reads=3 n0=4096 -> not published
```

* LBA `0x7B6DE` is sector 6 of the page at `0x7B6D8` — the **same 4 KiB unit
  (63195)** the gate was validating, i.e. the page that dies is the page the
  aborted transfer was for.
* `c_nz=0` with `fstate=0` (CLEAN) and `valid=0xFF`: the cache held a **fully
  valid, all-zero frame** for that unit. Every gate read is served from it, so
  "two independent reads that agree" only proves the cache agrees with itself.
* `r_nz=3754 r_sum=0x6A3B1` is the file's page, read with the cache bypassed.
* Refuser 2 shows the same shape through the image path's witness: `got` is
  the FNV-1a of 4096 zero bytes (`0x76EFDDC5`), i.e. the fill delivered zeros
  where the load-time sum says the file holds `0f b6 f3 83 e6 03 48 89 …`.

The mechanism is L67's, one layer lower than L67 looked: QEMU resets the
controller, aborts the in-flight request, and the aborted AIO completes with
**status 0 and no DMA**. `virtio_blk_read_raw()` copies `bounce_buf` out on
`status == 0`, so the frame keeps the *previous* command's sector — and a frame
just taken from the allocator is virgin memory, i.e. zeros. The block cache
then publishes that frame as valid (its own rule is "status 0 means the data
landed"), and from that moment the page is zero for every reader until the
entry is evicted or invalidated.

The same shape explains the `[FATW] ZERORD` rows L67 saw on the *write* path (a
sector that must hold `Player.log` bytes reads all zeros): a write whose
transfer was aborted is counted as landed, and the hole is what the next
read-modify-write sees.

### (d) An image sum computed over a different page — **refuted**

`v2_image_materialize` uses `pos = a - USER_IMG_BASE`, `pi = pos / FRAME_SIZE`,
and `vm_image_sums_build` fills `tab[i]` from `pos = i * FRAME_SIZE`; the
prefetch path uses `ppos = (page + i) * FRAME_SIZE`. The guest's
`want=0xDC910CE7` equals the host-computed FNV of `rwlaunch.bin` at `0x10000`,
so the sum it compared against was the sum of the page it validated. The
mismatch was the *bytes*, not the index.

## 3. The fix

### 3.1 Driver: a transfer that overlapped a device event is never used

`blk_dev_epoch` (virtio_blk.c) is bumped by every reset the kernel performs
(`nvme_reinit`) and every one it detects (`CC.EN`/`CSTS.RDY` cleared under an
outstanding command). `virtio_blk_read_raw()` snapshots it per sector: if it
moved across the submit, the completion is not evidence, so the sector is
re-issued once under a fresh cid (`[BLKQ] untrusted completion re-read`); a
second move is `-EIO`, never stale bytes.

### 3.2 Block cache: a fill that straddled a reset is dropped, not published

Both fill paths (`fill_run_contig`, `fill_run_fallback`) snapshot the epoch
before Phase B and compare it in Phase C. If it moved, the entry is unlinked
and its frame freed (`[BLKC] distrust … frame dropped, re-reading`), and the
copy pass re-reads from the device. This is the invariant the task asked for:
**a failed/aborted DMA must never publish a page, and a page that was published
before must be invalidated when the transfer is aborted.**

### 3.3 Exec gates: the device is the truth, the cache is not

* `fat16_read_direct_pos_raw()` reads a page straight from the device at the
  LBAs the cluster chain gives, bypassing `blk_cache`.
* `vm_exec_page_verify` (the DSO gate): on an all-zero answer it reads the page
  with the cache bypassed. If the device has the page, it publishes **those**
  bytes and drops the poisoned unit
  (`[TXT] ZERO off=… -> the DEVICE holds this page: cache frame was POISONED`).
  A page the device really holds as zeros is still refused.
* `v2_image_materialize` (the image gate): on a sum mismatch it re-reads the
  page with the cache bypassed and accepts it only if the **load-time sum**
  matches (`[TXT] IMG SUM pos=… -> the DEVICE matches the witness`). The
  witness stays authoritative; the cache stops being able to defeat it.

### 3.4 A refused page is an EIO on the mapping, not a protection fault

`vm_handle_fault` now returns `2` ("I/O error") when the page-in failed with
`-EIO`, and marks an **executable** region `VM_MAP_IOERR`. The trap layer turns
that into **SIGBUS** (`[EIO] pid=… page-in failed VA=… -> SIGBUS`), Linux's
answer for a file-backed page that cannot be read, instead of SIGSEGV. Mono
installs a SIGSEGV handler for null checks, so SIGSEGV there meant "run the
handler, return, fault again" — measured in L67 as an endless loop that
eventually corrupted `rt_sigreturn`'s own frame and died with status 11. A
marked region answers the same EIO to every later fault and every
`vm_touch` without touching the device, so a failed mapping cannot turn into a
device storm.

## 4. Tests

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **431 run, 0 failed** (was 430; the
lanes' `429/0` target plus this lane's tests).

* `test_blk_cache_aborted_dma_not_published` (blk_cache_test.c) — the seam
  `virtio_blk_test_abort()` reproduces the aborted-AIO shape faithfully: the
  command succeeds, the bounce buffer keeps the *previous* sector, and the
  device generation moves. Asserts (1) a single abort is re-issued and the
  caller gets the **device's** bytes, never the stale bounce buffer; (2) a
  persistent abort returns `-1` and leaves the caller's buffer untouched;
  (3) a fill that straddled the event is **not published** (`probe.found == 0`)
  and the bytes come from a fresh device read; (4) a healthy fill caches and
  matches the device.
* `test_vm_poisoned_cache_page` (vm_test.c, run by
  `vm_test_post_cache_suite` because it needs the cache live) — installs
  exactly the frame the field bug published (`blk_cache_test_poison`: 4 KiB of
  zeros, `valid=0xFF`, CLEAN) over the unit a mapped exec page reads, proves
  the cache serves zeros, then asserts the gate publishes the **file's** bytes
  byte-for-byte, that the poisoned unit is gone, and that
  `fat16_read_direct_pos_raw` agrees with the device. Frame accounting is
  exact (`free0 == now`).
* `test_vm_exec_page_eio` (extended) — a refused exec page now returns `2`
  with `why="EIO"` and sets `VM_MAP_IOERR`; the next fault answers `2` with
  **zero** additional device sectors; clearing the marker lets the same fault
  resolve into the file's bytes.

## 5. Ladder

Three 1800 s windows. Success criteria: no final `[TXT] ZERO` / `IMG SUM`
refusal, no process death from a page fault, NFS at the full 277.5 MB, and the
ladder reaching whatever comes after the load.

| run | window | resets | final `[TXT] ZERO` / `IMG SUM` | page-fault death | NFS bytes | presents (last CRC) | end state |
|---|---:|---:|---:|---|---:|---|---|
| f1 | **1800 s** | 0 | **0 / 0** | none | **277 529 012** (264.67 MiB) | 260 (`0xa4d86f0c…`) | load complete; post-load frame-handshake park (§51.4): alive, 25 threads, `res_mib` climbing one anon page per iteration |
| f2 | 510 s | 1 | 0 / 1 | none | 221 036 013 (210.80 MiB) | 240 (`0x2e2650f4…`) | SIGABRT from the runtime's own `g_assert` path (`ipc=0x108004300B`, the `x86-codegen.h:403` site; the message was lost to the NUL hole an aborted write left in Player.log — `[FATW] ZERORD sector=657286`) |
| f3 | 470 s | 6 | 1 / 0 | none | 263 653 604 (251.44 MiB) | 190 (`0x8f48115d…`) | `[TXT] UNVERIFIED off=0x757000 reads=3 n0=0` after reset #1: a **0-byte** read (metadata/chain), EIO → SIGBUS → Mono's SIGBUS handler faulted at VA=0 |
| f4 | see below | | | | | | |

f1 is the window the task asks for and it is clean: 1800 s, the full deterministic
load, zero refusals of any kind, zero deaths, 3 584 exec pages verified
(`[TXT] n=3584 bad=0`), 260 presents with changing CRCs, and a screenshot (see
below) that shows the game's own window and loading artwork. It ends parked in
the post-load frame handshake — the §51.4 livelock, which is a different bug
(it is *alive*, `[ALIVE-SET]` fires 11 969 times, and `res_mib` grows one
anonymous page per iteration).

f2 and f3 are the reset-rich windows. Both are **page-in clean for the DSO
gate** (no `[TXT] ZERO`), and both end on something else: f2 on the runtime's
own assert, f3 on a 0-byte read whose *chain walk* failed after six resets.
f3's shape is the residual class §7 names: a reset can also corrupt a
**metadata** read, and a wrong FAT entry turns a chain walk into another
cluster (measured separately as `chain=-1` at a file offset the host verifies
is inside a complete 2 015-cluster chain). `read_fat()` now verifies every FAT
sector with two agreeing reads; f4 is the window that carries that change.

### Screenshot (f1, t≈480 s)

`obj/rw-desktop/shots/50_running.png`: the HobbyOS desktop with the game's
window ("RLAUNCH" title bar, red close button) filling the screen; the window
content is RimWorld's own loading artwork (a Mars-like planet with ice caps
against a starfield) with a small centred box reading **"Initializing."**; the
taskbar is visible with Apps / RLAUNCH / clock. The last frames before the park
(`58_running.png`) are noise, which is what the window shows once the game stops
presenting — no menu is reached, so no menu is claimed.


## 6. What is left

1. **Metadata reads after a reset (f3).** A reset can also abort a FAT sector
   read, and one wrong entry sends the rest of a chain walk to another file's
   clusters. `read_fat()` now reads each FAT sector twice and requires
   agreement (a persistent failure is a chain break, not a guess), and the
   gates' authoritative read is itself a two-read agreement. A reset-rich
   window with that change is the next measurement.
2. **The post-load frame-handshake park (§51.4, f1).** With page-in clean the
   1800 s window now ends *alive* in the known park: presents freeze, no
   thread is in `READY`, and the main thread touches a fresh anonymous page
   every ~6 ms (`res_mib` 1 926 at the end). That is the next wall in the
   loader, and it is not a page-in or block-layer problem.
3. **The runtime's own walls (f2).** SIGABRT from Mono's `g_assert` path
   (`x86-codegen.h:403`, §48) still ends some windows once the load is deep;
   f1 shows it is not deterministic.
4. **Post-reset writes (f2's `[FATW] ZERORD`).** One Player.log sector still
   read back zeros after a reset, i.e. a write whose transfer was aborted was
   counted as landed. `virtio_blk_write_raw` now applies the same
   pre/post-readiness and epoch rules as reads; the `[FATW] ZERORD` row is the
   tripwire that will say whether that closes it.

