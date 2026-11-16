# cp/nfs — NFS READ throughput for Wine (P2)

C2-wine2's P2 run: wineboot's DefaultInstall reads ~145 MB over NFS; the client
fetched ~1.3 KiB per READ RPC (~111k RPCs at ~1.7 ms each), past Wine's 5-minute
boot-event wait.

## 1. Why 1.3 KiB (measured, base c635edce)

`NFS_READ_RSIZE` was a fixed 1344 bytes: a 1472-byte UDP datagram minus the
READ reply's 128 bytes of RPC/NFS overhead.  The reason was written in
`nfs.h`: the stack "neither emits nor reassembles IP fragments", so every
reply had to fit one Ethernet frame — a larger READ makes slirp fragment the
reply, and the old receive path then parsed fragment 2..n as UDP datagrams.
The bench's `[NFSPIPE] len=65536 rs=1344 issued=49` row is that limit: 49 RPCs
per 64 KiB.  Pipelining (16 READs outstanding, L35) already existed; only the
per-RPC payload was the cap.

## 2. What changed

| Piece | File | Role |
|---|---|---|
| IPv4 reassembly | `src/kernel/net.c` (`ipr_add`, `ipr_release`, hook in `net_rx_packet`) | receive side only (requests never fragment); 8 slots keyed (src, dst, id, proto) per RFC 791; 8-byte-unit bitmap so a duplicate never counts twice; incomplete slot dropped after 1 s or evicted oldest-first; delivery after the slot is detached, so `net_lock` never nests under `ipr_lock`; UDP and ICMP; ~530 KiB BSS |
| FSINFO | `nfs_parse_fsinfo_reply`, `nfs_pick_rsize` (nfs_proto.c), mount (nfs.c) | rsize = min(rtpref, rtmax, 32 KiB), 4 KiB-rounded, never below 1344; one `[NFS] fsinfo` row per mount |
| reply buffers | `nfs_rbig_get/put` (nfs.c) | a 33 KiB reply datagram cannot live on the 64 KiB kernel stack: each READ path takes one frame run |
| fallback | `nfs_rsize_fallback` (nfs.c) | a large READ that times out on both attempts drops the mount to 1344 and finishes the range: a path that loses fragments still works, loudly (`[NFS] rsize fallback`) |
| readahead | `nfs_ra_try` | the 64 KiB window fills with the mount's rsize (2 RPCs, was 49) |

Transport: UDP stays (Linux nfsd answers UDP FSINFO with rtmax 32768, its UDP
payload cap).  TCP was not needed.

## 3. Evidence (strix, 2026-11-15; `docs/cp-nfs-evidence/`)

`tools/cp/nfs/nfs_bench.sh [tree]` exports a 64 MiB xorshift file read-only to
127.0.0.1 only and runs `src/user/cpnfsbench.c` (native: it mounts) in the
intel guest.  FNV-1a-64 checked in the guest against the host's.

| phase | base c635edce | cp-nfs |
|---|---|---|
| 64 KiB read()s, 64 MiB | 82.14 MB/s | 185.38 MB/s |
| 4 KiB read()s, 16 MiB | 76.60 MB/s | 174.76 MB/s |
| 1344-byte read()s, 4 MiB | 69.90 MB/s | 167.77 MB/s |
| RPCs per 64 KiB | 49 | 2 |

Every phase's checksum matches.  The sequential bench was already fast on base
(the pipeline hides most of the RTT); Wine's measured 1.7 ms per 1.3 KiB RPC
says its reads take a serial or amplified path (see §4).  Whatever that path
is, each RPC now carries 24x the bytes.

Tests: kernel unit `test_net_ipv4_reassembly` (out-of-order + duplicate
fragments, an orphan fragment, a misaligned MF fragment, exact bytes);
`nfs_proto_test` FSINFO parse + rsize policy.

## 4. Not done / for C2-wine2

- Wine's own access pattern was not measured here.  Candidates for the
  1.7 ms serial RPCs: the ONE global readahead window (`nfs_ra`) thrashing
  between concurrent readers (each miss refilled 64 KiB = 49 RPCs on base,
  2 now), and the pipeline's serial tail after a stall.  Re-run P2 and read the
  `[NFSPIPE]`/`[NFSRTT]` rows; a per-file readahead table is the next lever.
- Receive copies are still byte-wise (`handle_udp` into the 4 MiB ring, recv
  out of it, XDR parse), ~185 MB/s is probably that.
- IP fragments are never EMITTED; nothing needs it (WRITE is not implemented).

## 5. /etc/exports incident (2026-11-15) and the exports lock

The lane bench added and removed its line inside the QEMU lock and its own
check passed.  C2-wine2's `run-wine-p2.sh` backs `/etc/exports` up BEFORE
taking the QEMU lock and restores the whole file at exit; a run that took its
snapshot during our bench restored our line afterwards (stale export of a
removed directory, localhost-only, ro; its next run logged `exportfs: Failed to
stat /srv/nfs/cp-nfs-bench`).  Cleaned up by removing only our exact line after
no Wine harness was running (no `exportfs -ra` during their boot).
`nfs_bench.sh` now holds `/tmp/cp-exports.lock` (outer) around its whole
export lifetime; the same lock in run-wine-smoke.sh / run-wine-p2.sh (taken
before the backup, released after the restore) closes the race for good.

## 6. Regression guard (cp/nfsguard)

The 32 KiB READ size has been fixed and then lost more than once, usually when
an older `nfs.c` came back through a merge. `main` still has the 1344-byte
clamp on every READ path, so a merge from `main` brings it back without a
single conflict marker in some hunks. There are three layers of guard:

| Layer | Runs | Catches |
|---|---|---|
| `nfs_proto_test` rows "guard: ..." | `make host_tests` | `NFS_READ_RSIZE_MAX` < 32 KiB, a reply buffer too small for a 32 KiB reply, `nfs_pick_rsize` no longer giving 32 KiB. Literal 32768s on purpose: a row written against the macro would still pass after the macro went back to 1344. |
| `tools/cp/nfs/rsize_scan.py` | `make host_tests` (target `nfs_rsize_scan`) | R1 the max constant; R2 `if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;`; R3 readahead calling `nfs_read_pipelined(..., NFS_READ_RSIZE)`; R4 no FSINFO, or `read_size` not set from `nfs_pick_rsize`; R5 no `NFS_RPC_RBIG` reply buffers; R6 `net.c` no longer sending fragments to `ipr_add`. R7 the readahead fill not invalidating the shared window first (§7). `--selftest` re-applies each one to a temporary copy and must see the scan FAIL. |
| `tools/cp/nfs/rsize_guard.sh [tree]` | boot, on a host whose nfsd offers 32 KiB (strix) | Behaviour, however it regressed: G1 every bench phase passes, including the 64 MiB checksum; G6 six concurrent readers of reverse-order 4 KiB pages get every page right (§7); G2 `[NFS] fsinfo ... rsize=32768`; G3 no `rsize fallback`; G4 every sampled `[NFSPIPE] len=65536` row has `rs=32768 issued<=2`; G5 READ bytes / READ RPCs >= 16384 over the whole run. rc 1 = regression, rc 2 = could not judge. |

The guards were checked both ways (2026-11-15):

- `rsize_scan` passes on cp/nfsguard. On c635edce (before cp/nfs) and on `main`
  it fails with every R1–R6 row and the exact clamp lines. Its selftest
  detects all six mutations.
- On the cp/nfs console log the `rsize_guard` judge
  (`RSIZE_GUARD_CONSOLE=<log>`) passes: 2.0 RPCs per 64 KiB, 32768 bytes per
  RPC. On the c635edce log it fails G2, G4 (`rs=1344 issued=49`) and G5 (1337
  bytes per RPC).

When a merge conflicts in `nfs.c`, `nfs.h` or `net.c`, keep the cp/nfs side of
every READ-size line. Then run both guards before moving `cyberpunk`
(docs/cyberpunk.md §7.4).

## 7. Concurrent-reader corruption (2026-11-16, cp/nfsfix)

C2-wine2 found that after cp/nfs, six processes reading the same files over
NFS sometimes got wrong `pread()` bytes. ntdll.so came back wrong from byte
212992 on, and Wine processes jumped to garbage addresses. One process alone
was always right.

**Cause.** `nfs_ra_try()` set `busy=1`, dropped the lock and refilled the
single global `nfs_ra_buf`, but it left `valid=1` with the old `(fh, base)`.
A concurrent reader whose range matched the old window was served bytes the
fill was overwriting. The race predates cp/nfs, but small reads take the
window only when `len <= rs`. At rs=1344, 4 KiB page-ins never got there; at
32 KiB they all do.

**Fix.** Set `nfs_ra.valid = 0` under the lock before the fill.

**Proof.** On the unfixed tree, conc6 fails: reader 1 got `bad=16` pages,
exactly one 64 KiB window, first bad byte 3276800. On the fixed tree:
- the full gate batch passes;
- G6 passes in 5 of 5 boots;
- sequential reads run at 188–193 MB/s.

R7 in `rsize_scan.py` keeps the invalidation line from going missing.

**Still open.** The single global window thrashes between concurrent readers.
That costs performance, not correctness: each 4 KiB read can cost a 64 KiB
refill. A per-file window table is the next step if Wine's page-in time calls
for it.

## 8. Per-file readahead windows (cp/nfs2, item 1)

Gate P2 (run23) read 1.66 GB in 51k 32 KiB READs to bring up ~200 MB of
Wine; `[NFSRA] hits=21536 fills=8477`: 555 MB of 64 KiB fills served 65 MB.
The one global window lost three ways: interleaved readers of different
files evicted it; a fill started AT the request and ran forward, so a
backwards or scattered reader (pfxls's reverse 4 KiB preads cost 69 MB for
13.5 MB of hashing; a loader's page faults) paid a fresh 64 KiB per page; and
nothing survived for the next process paging the same DLL.

**Now** (`nfs.c`, "readahead window"): `NFS_RA_WINDOWS` (256) windows, each
an aligned 64 KiB block of one file, keyed by (mount, file handle, block) and
replaced LRU; buffers are taken from the frame pool on first use (16 MiB).
Every read up to `NFS_RA_MAXREQ` (1 MiB) goes through them piece by piece;
a miss loads the blocks the read still needs, and a miss right after a block
that was read to its end (per-window `next`) loads `NFS_RA_SEQ` (4) blocks in
one pipelined fill.  Larger reads stream straight into the caller's buffer.

**The cp/nfsfix rule, per window.** A fill marks each claimed window
`busy = 1; valid = 0` in the same locked section, drops the lock for the
READs, and publishes `valid = 1` under the lock; readers copy only from
`valid && !busy` windows, under the lock.  A reader that needs a block being
filled reads its own bytes directly (`direct=` in `[NFSRA]`).  `rsize_scan`
R7 checks both halves per window (three selftest mutations: the invalidation
removed, moved after the unlock, the reader's busy check removed); R8 fails
on fewer than 8 windows or a lookup that stops keying on (handle, block).

**Transport, same commit.** A 32 KiB reply is ~23 fragments and the RX
virtqueue holds 256 frames; run23's stalls ("inflight=10 issued=22
replies=12") are that queue overflowing.  Pipelines now share an in-flight
budget (`NFS_INFLIGHT_MAX`, 6 x 32 KiB; a pipeline with nothing in flight may
always send one), and a reply timeout re-sends the in-flight READs instead of
finishing the whole range serially (`resent=` in `[NFSPIPE]`, `stalls=` in
`[NFSRA]`).

**Bench** (`cpnfsbench` phase `multi6`): six processes read six different
2 MiB files at once (even ones forward, odd ones backward, 4 KiB pages), every
page compared with the host's xorshift stream; `rsize_guard.sh` G7 requires
all pages right and `amp_pct` (READ bytes / bytes read) <= 200.

| strix, intel guest | base 5eea3712 | cp/nfs2 |
|---|---|---|
| multi6 amp_pct (fetched / read) | 345 (42400 KiB for 12288) | 100 (12288 for 12288) |
| multi6 hit_pct_est | 78 | 93 |
| conc6 / multi6 pages | all correct | all correct |

## 9. 64-bit offsets and files > 4 GiB (cp/nfs2, item 2)

Audit of every offset and size on the NFS read, getattr, lseek and
page-cache paths:

| Where | Was | Now |
|---|---|---|
| `nfs_read_file()` (nfs.h) | `uint32_t offset` | `uint64_t`; count clamped to INT_MAX (returned as int) |
| serial READ, pipeline slots, `issue_off`/`contig`/`end`, windows | `uint32_t` (a READ past 4 GiB asked the server for offset mod 2^32) | `uint64_t` |
| `struct file.nfs.cursor`, `.ra_next` (fs.h) | `uint32_t`: read() wrapped to offset 0 after 4 GiB | `uint64_t` |
| `file_seek()` NFS branch | `cursor = (uint32_t)newpos` | 64-bit |
| `l76_nfs_read()` window/page cache (fs.c) | `uint32_t off`, `base = off & ~(uint32_t)4095` | 64-bit (the mask no longer clears the high half) |
| `file_nfs_ident()` size for mmap | `(uint32_t)size`: a 4 GiB + 10 MB file mapped as 10 MB | saturates at 0xFFFFFFFF, so the page cache's size cap refuses it |
| fattr3 size, `f->nfs.size`, `k_stat.st_size` | already 64-bit | unchanged |
| `nfs_obj_get()`/`nfs_obj_page()` (file_obj.c), the NFS reads in `vm.c` | `uint32_t` sizes/offsets | unchanged: objects are capped at `VM_OBJ_MAX_BYTES` (< 4 GiB) and a > 4 GiB file no longer reaches them; the casts are for C3-pfx/C1-k4 when that cap grows past 4 GiB |
| readdir cache sizes (`nfs_list_dir` `size_out`) | `uint32_t` | unchanged (native `ls` only; the API is vfs.c's) |

Gate (`NB_BIG=1 bash tools/cp/nfs/nfs_bench.sh`): a 5 GiB + 12345 byte
xorshift file, written under `obj/` on strix's root filesystem, renamed
into the export and removed with it; the guest reads it with 4 MiB read()s
(the direct pipeline) and SHA-256s it (`cpnfsbench`'s own SHA-256, checked
against `sha256sum` on the host), checks fstat and `lseek(SEEK_END)` give
the 64-bit size, and re-reads a 4 KiB page at 4 GiB + 8 MiB + 123 through
the window path after an lseek.

## 10. One read() of ~172 MB (cp/nfs2, item 3)

The game reads `shader_final.cache` (172 MB, cyberpunk.md §0.2 item 12) with
one `read()`.  The path is native/compat `read()` -> `file_read` (int count)
-> `l76_nfs_read` (no L76 window: the count is > 16 KiB) ->
`nfs_read_file` -> `nfs_read_pipe` straight into the caller's buffer (reads
above `NFS_RA_MAXREQ` bypass the windows).  What had to change was the
pipeline's failure mode: one lost reply used to end the pipeline and read
the REST of the range serially, one round trip per 32 KiB block (thousands
for 172 MB); it now re-sends the READs in flight (item 1).
Gate: `cpnfsbench` phase `one176m`, 176 MiB + 777 bytes in one read() into
one malloc'd buffer, SHA-256 against the host's (`rsize_guard.sh` G8).

## 11. stat times and symlinked directories (cp/nfs2, item 4)

- **fstat/stat times.** `nfs_parse_fattr3` now keeps atime and ctime;
  `file_stat_fd` (fstat) and the NFS branch of stat(path) report all three
  (fstat gave 0 for all three; stat(path) gave mtime for all three).  fstat needs the attributes of an open handle: the ones
  every LOOKUP returns are kept in a 64-entry cache keyed by (mount, handle),
  3 s TTL like Linux's acregmin, else GETATTR (`nfs_getattr_cached`).
- **Symlinks in intermediate components.** `nfs_lookup` walks the path one
  component at a time; when ANY component is NF3LNK it reads the target,
  joins it against the link's directory (`nfs_symlink_join`), appends the
  rest of the path and restarts from the root, at most 40 links per lookup
  (Linux MAXSYMLINKS; ends a cycle).  Before, only a link in the last
  component was followed: `dlink -> sub` made `/NFS/dlink/file` fail, since
  the server answers LOOKUP in a symlink handle with NFS3ERR_NOTDIR.
- Probe rows (`cpnfsbench`): `[NB] fstat`/`[NB] stat` against the host's
  `stat -c '%X %Y %Z'` of a file whose times the harness set with touch;
  `[NB] symlink path=...` for `dlink/`, `d2/` (link to a link), `sub/up/`
  (`..`) and `sub/up/d2/` (three links in one walk), each opened, stat()ed
  and read whole.  Tests: `nfs_proto_test` (fattr3 times + a 5 GiB size),
  kernel `nfs_test` (atime/ctime).

## 12. Where lost READ replies go (cp/nfs2, net.c counters)

Gate P2 on the item-1 commit stalled 544 times with ONE READ in flight
system-wide (`[NFSPIPE] stall inflight=1 ... budget=32768`), about one per
MiB; the single-reader bench never stalls.  So it is not RX-queue overflow.
`net.c` now counts why reassembly drops an incomplete datagram
(`ipr_drop_timeout`: a fragment never came; `ipr_drop_evict`/`_full`: all 8
slots busy; `ipr_drop_bad`) and complete UDP datagrams no socket took
(`udp_nosock`, `udp_dgram_full`); `nfs_perf_dump` prints them as one
`[NFSNET]` row with the stall count.  A minimal diff (counters only, no
behaviour change), in its own commit.

## 13. Window table, second pass (cp/nfs2)

The item-1 P2 run showed the 16 MiB table was too small for rundll32's
repeated passes over the 185 MB tree (hit rate 97% in wineboot, 83% by the
quiesce), and the per-MiB console set was costing throughput:

- `NFS_RA_WINDOWS` 256 -> 4096 (256 MiB at most), found through a 1024-bucket
  hash on (fh, block) instead of a linear scan, replaced by a second-chance
  clock.  New buffers are taken only while a quarter of the frame pool is
  free; while less than an eighth is, every miss gives up to 32 idle window
  buffers back (`nfs_ra_shrink()`, exported for a future frame-pool reclaim
  hook; `shrunk=` in `[NFSRA]`).
- The first wait for a pipelined reply is 150 ms (`NFS_PIPE_TIMEOUT_MS`;
  replies that come at all came within 99 ms in run23), re-sends get 500 ms.
- `[NFSRTT]`/`[NFSRA]`/`[NFSNET]` print every 8 MiB of READ traffic instead
  of every 1 MiB (each console byte is a trapped port write; run23 printed
  12576 `[NFSRTT]` rows).
- `[NFSRA]` gains `big_kib` (reads above `NFS_RA_MAXREQ`, no windows),
  `tail_kib` (re-read serially after a hole) and `wins` (windows in use).
- `NFS_RA_MAXREQ` 1 MiB -> 16 MiB, `NFS_RA_FILL_MAX` 8 -> 16: P2 run 2 read
  223 MB with read()s above 1 MiB (the same DLLs once per Wine process)
  against 158 MB through the windows.  Only reads above 16 MiB (the 172 MB
  shader cache) stream straight into the caller's buffer.

## 14. nfs_bench.sh on QEMU slots (cp/nfs2)

`nfs_bench.sh` (and so `rsize_guard.sh`) no longer holds
`/tmp/cp-exports.lock` across its boot: it fills the export dir, adds its
line with `tools/cp/exports.sh add`, boots under `tools/cp/qemu-slot.sh`,
and its EXIT/INT/TERM/HUP trap runs `exports.sh remove` and removes the
dir (the run-wine-p2.sh pattern of cp-pfx f8ceec91).  The default export is
`/srv/nfs/cp-nfs-bench-<tree dir name>`, so a lane and a base bench can run
in two slots at once.

## 15. Hard-mount semantics: a lost reply is never data (cp/nfs2)

C5-vk2 (strix, 4 guests) saw `[NFSPIPE] stall` rows, `[NFS] rpc: no reply
from server`, `[NFLK] retry comp=WINE tries=3 rc=-1`, then wineboot fault
on an all-zero `sortdefault.nls` section.  The client gave up after a short
budget (READ: 2 x 500 ms; LOOKUP: ~6 s) and the READ paths returned the
bytes they had so far as a successful short read, which a caller that
zero-fills short reads turns into zeros.

**Now:**
- A READ with no reply is re-sent (fresh xid, fresh socket, backoff 0/250/
  500/1000/2000 ms on top of each 500 ms wait) until it is answered or
  `NFS_HARD_MS` (180 s) have passed; every retry is a bounded `[NFSHARD]
  retry` row, a give-up is `[NFSHARD] GIVE UP ... -> EIO`.  The same holds
  for LOOKUP, GETATTR, READLINK, READDIRPLUS and FSSTAT on a mounted export
  (`rpc_call_hard`); mount-time calls stay soft so a missing server fails
  `mount` quickly.
- A failed read fails WHOLE: `nfs_read_file` returns -1, never a partial
  count (serial READ, pipeline tail, window fill, the direct piece of a
  windowed read).  `read()`/`pread()` see -EIO; a page-in (`nfs_obj_page`,
  the exec gate) fails and the fault is SIGBUS (trap.c, `VM_MAP_IOERR`),
  never a zero page.  Only end of file ends a read short; a short non-EOF
  reply is followed by a READ for the rest.
- The rsize fallback (1344-byte READs) now needs evidence: three large
  READs unanswered while a single-frame READ of the same block IS answered.
  A server that answers nothing no longer drops the mount to 1344.

**Guard (G9):** test kernels can drop NFS reply datagrams on receipt
(`/.cp-nfs-inject.every.N`, `.blackout.MS`, `.off` as a LOOKUP name under
the mount).  `cpnfsbench` phase `inject` reads fresh files page by page and
maps one, with every 5th reply dropped and with all replies dropped for 8 s,
and compares every page with the host's stream.  It fails on the commit
before the fix (the blackout read gives up) and passes after it.

## 16. Why the server "stopped answering" (strix, 2026-11-16)

Measured during Gate P2 run 2 with four guests up: the guest's reassembly
saw no partial datagram (`[NFSNET] frags=36503 done=1604 drop_timeout=0
drop_evict=0 ... stalls=29`): whole replies vanished.  On the host,
`/proc/net/snmp` Udp `RcvbufErrors` grew by 203 in 90 s while that guest
counted 28 stalls, and `ss -uamnp` showed the drops on qemu-system-x86's
slirp sockets (`rb212992 ... d3030` for one guest, `d62` for ours).  So the
loss is QEMU's slirp host socket, whose 212992-byte default receive buffer
holds only about three 32 KiB replies (truesize ~2x), overflowing while
QEMU drains slowly.  It is not `exportfs -ra` (no partial or deferred
pattern needed to explain it; the counters account for it).

Client side: at most two 32 KiB READs outstanding per socket
(`NFS_PIPE_INFLIGHT`), and hard-mount retries for the rest.  Host side
(proposed, not done: it is a system setting): `net.core.rmem_default` of
1-4 MiB would give every slirp socket room for a full pipeline.

Run 3 (39c639f4, 2 replies per socket): stalls=1 where run 2 had 65 at the
same point, and every one of our QEMU's 5775 slirp sockets showed `d0`
(`docs/cp-nfs2-evidence/slirp-rcvbuf-strix.txt`).  The per-socket cap is
enough; no host setting needed for our guests.

One more shape: in run 3 a LOOKUP went unanswered for 95 s (30 hard
retries) while READs on other sockets were answered.  A small reply cannot
overflow a buffer, so this is nfsd not answering.  Suspected (not proven):
`exportfs -ra` in tools/cp/exports.sh flushes nfsd's export caches, and
nfsd drops (defers) UDP requests that arrive before mountd refills them.
Proposed for exports.sh (integrator-owned, not edited here): add with
`exportfs -o ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534 127.0.0.1:DIR`
and remove with `exportfs -u 127.0.0.1:DIR` (keeping the /etc/exports edit
for persistence), so other exports' cache entries survive; and log each
call with a timestamp so a no-reply window can be matched to it.

## 17. Gate P2 with cp/nfs2 (strix)

| | run23 (base) | run 1 (15ad85b7) | run 2 (528e5c81) | run 3 (39c639f4) |
|---|---|---|---|---|
| READ bytes, wineboot phase | 269 MB | 96 MB | <= 85 MB | <= 43 MB |
| READ bytes, whole run | 1.66 GB (complete) | 569 MB at the hang | 393 MB at the hang | 201 MB at the hang |
| wineboot | status=53 ms=306756 | status=53 ms=304877 | status=53 ms=304031 | status=53 ms=303393 |
| window hit rate | 72% (fills/hits) | 83% | 93% | 91% |
| pipeline stalls | (8 rows capped) | 568 | 412 | 1 |
| guests on strix | 1 | 1-2 | up to 4 | 4 |

Every cp/nfs2 run hung in the post-wineboot quiesce with C1-k4's epoll
signature (all clients st=5, futex=0x0, wineserver idle, no NFS traffic),
so no run reached G1-G3; the NFS numbers are judged on page-in volume,
stalls and correctness (`docs/cp-nfs2-evidence/p2-runs-strix.txt`).
wineboot still ends at the 300 s boot-event timeout: NFS now accounts for
a few seconds of RPC wait in that window, not for the timeout.


## 18. The 180 s LOOKUP give-up: host ephemeral ports (cp/nfs2b)

After cp-nfs2 merged, C5-vk2 run 5 logged `[NFSHARD] GIVE UP op=RPC off=0
want=3 tries=55 after_ms=180000 -> EIO` (`want=` carries the procedure
there: 3 = LOOKUP, of the component `WINE` in the export root; the
`[NFLK] retry comp=WINE tries=1 rc=0` right after it succeeded at once).
During the 180 s, READs on the same mount kept flowing and the guest's
reassembly lost nothing (`[NFSNET] drop_timeout=0`); nfsd's own UDP socket
showed no drops (`ss`: d0), and nothing in the mountd/nfsd journal.

What differs between the two: READs use the mount's long-lived socket,
while every LOOKUP/GETATTR/READLINK/READDIRPLUS went out on a FRESH guest
UDP socket (`rpc_call`: create, connect, send, close).  A fresh guest port
is a fresh host UDP socket inside QEMU's slirp, kept for 240 s, bound to a
host ephemeral port.  strix's range is 32768-60999 (28232 ports); our
Gate P2 QEMU held 5775-8459 UDP sockets (`ss -uap`), so four guests booting
Wine exhaust the range, and from then on slirp cannot send any NEW
socket's datagrams (the kernel's autobind fails) while old sockets keep
working.  A LOOKUP that happens to start then is never sent, retry after
retry, until enough slirp sockets expire.

**Fix:** every RPC on a mounted export goes out on one of a small pool of
long-lived sockets per mount (`NFS_SOCK_POOL` 6: READ pipelines, the serial
READ and `rpc_call_pool` for the other procedures); a socket is kept on a
timeout (a late reply is skipped by xid; a NEW socket is what an
exhausted host cannot serve).  Only mount-time calls (portmap, MNT,
FSINFO) and an overflow when all six stay busy for 20 ms (`pool_temp=` in
`[NFSNET]`) create a socket.  xids are taken atomically.

**Guard G10:** `cpnfsbench` phase `lookstorm` (4 processes x 1500 stat()s);
the storm may create at most 32 guest sockets (`socks=` in the `[NFSNET]`
rows the bench requests before and after it).
