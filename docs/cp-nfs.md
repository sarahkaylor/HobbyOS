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
| `tools/cp/nfs/rsize_scan.py` | `make host_tests` (target `nfs_rsize_scan`) | R1 the max constant; R2 `if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;`; R3 readahead calling `nfs_read_pipelined(..., NFS_READ_RSIZE)`; R4 no FSINFO, or `read_size` not set from `nfs_pick_rsize`; R5 no `NFS_RPC_RBIG` reply buffers; R6 `net.c` no longer sending fragments to `ipr_add`. `--selftest` re-applies each one to a temporary copy and must see the scan FAIL. |
| `tools/cp/nfs/rsize_guard.sh [tree]` | boot, on a host whose nfsd offers 32 KiB (strix) | Behaviour, however it regressed: G1 the 64 MiB checksum matches; G2 `[NFS] fsinfo ... rsize=32768`; G3 no `rsize fallback`; G4 every sampled `[NFSPIPE] len=65536` row has `rs=32768 issued<=2`; G5 READ bytes / READ RPCs >= 16384 over the whole run. rc 1 = regression, rc 2 = could not judge. |

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
