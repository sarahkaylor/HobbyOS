# Lane N2 — kernel TCP bulk-receive stall (self-driven, controller)

Delegation attempts deleg_90045af3 + deleg_bcfb9ef6 both died to a provider
connection outage before doing any work; the CONTROLLER executed the lane
directly in-session (same brief). This file records the deliverables.

## Commit
- `63460da` on `bw-perf-net2` (worktree `~/hobbyos-lanes/net2`, branched from
  `browser/perf-x64` @ ac58ef5, merged back into perf-x64 at the same hash).
- Files: `src/kernel/net.c` (+68) — per-socket stall counters +
  `net_dump_stall_probe()`; `src/kernel/arch/x64/trap.c` (+4) — call the probe
  after the `[BLK]` ticker line in the stall watchdog.
- Builds: `make ARCH=intel|arm MODE=unit_tests hobbyos.elf` exit 0 both;
  `make ARCH=intel hobbyos.elf` exit 0; symbol present in the ELF.
- No behavior change (instrumentation only) — safe for run 804.

## The probe
One `[NETDBG]` line per in-use socket, on every watchdog stall dump (~20s
cadence during console silence):
`s= st= ack= seq= rh= rt= mac= rxseg= rxstor= tx= acktx= wupd= drops= rtmo= arpf=`
- `rxseg` = matched TCP segments received (peer-activity detector; delta>0 =
  peer still sending)
- `rxstor` = payload bytes stored (our ack advancement source)
- `tx` / `acktx` = segments / pure-ACKs sent (are our ACKs leaving?)
- `wupd` = drain-side window-update ACKs (net_socket_recv path)
- `rh` = app drain position; `drops` = out-of-window refusals;
- `rtmo` = 5s recv timeouts; `arpf` = broadcast-fallback sends (mac never
  cached → both ACK sites are gated on `mac_cached` — if mac=0 shows up with
  frozen acktx/wupd, the ACK gate is the stall).

## ROOT CAUSE (2026-10-09 ~11:50) — SOLVED, and it was NOT the kernel
The stall = **O(n²) reallocation crawl in the port's own fetch sink**, proven three ways:
1. Kernel exonerated: 804b/806 NETDBG series — healthy TCP (1 MB wire delivered/ACKed,
   mac=1, zero drops, peer active); run 806's `rxb` probe reached 5.05 MB decoded
   (gzip ~5:1 reconciles with ~1.02 MB wire).
2. Counters (run 807): `al − n = 69603` and `bg − n = 8` CONSTANT across dumps =
   **exactly one malloc (large, >128 KB) per write-callback, zero others**.
3. In-code proof: WTF `Vector::appendRange` calls `reserveCapacity(size()+range)` —
   EXACT-size reserve (alloc + full move via `TypeOperations::move` = libc memcpy +
   free) — the one-shot form. Feeding it 8-16 KB chunks reallocs and copies the whole
   buffer per chunk; at 5 MB in, each callback copies ~5 MB at ~2.5 MB/s (guest slow
   memcpy) = the observed multi-minute "freeze". The memcpy-hot sampler rip = the move.
Fix: commit `4a7612fedf` — amortized pre-reserve (1.5×) in `wk5NetWrite` before the
appendRange (inner exact-reserve becomes a no-op). Other appendRange sites audited:
one-shot only, no change needed. Probe confirmation target (run 808): `bg` stops
tracking `n` 1:1, cadence fast, fetch completes.

## Design review outcome (net.c, 1222 lines, read in full)
The flow-control design is structurally present and sane: truthful window in
every outgoing segment (`send_tcp_segment` computes real free rx space,
capped 0xFFFF), contiguous-only store, immediate per-segment ACK in
`handle_tcp`, drain-side window-update ACK in `net_socket_recv[_timeout]`,
zero-window probes handled by the drop branch + immediate ACK. Therefore the
stall is in the DELIVERY/ACCOUNTING of the mechanism, not its structure.
Prime suspects to discriminate with the probe:
(a) `mac_cached == 0` for the stalled socket → BOTH ACK sites silently skip
    (gated `if (mac_cached)` / `if (pcb->mac_cached)`) → peer starves waiting
    for ACKs it can never get; the broadcast fallback can't route.
(b) Peer gave up retransmitting (rxseg frozen) after slirp-side RTO
    exhaustion while our window-update ACK never went out (same gate, or
    wupd=0 because the app never drains — rh frozen).
(c) Partial-store accounting mismatch (to_store < data_len edge where ack
    advances but the peer's snd_una view diverges).

## Run 804 (the discriminating run)
Started 2026-10-09 ~10:15 on perf-x64 @ 63460da, same staging as 803
(NO_SCRIPT=1, pinned/w2-intel5 binary, --url cnn.com --run 804).
Post-run greps:
  grep -a 'NETDBG' serial.log              # the time series
  grep -a 'flow-control\|net fetch\|load-ok\|SR-t' serial.log
Interpretation guide (deltas between consecutive dumps):
  rxseg growing + acktx frozen       -> (a) ACK suppression (check mac=)
  rxseg frozen  + acktx growing      -> peer gave up; our ACKs are shouting
                                        into the void (TX path / routing)
  rxseg frozen  + acktx frozen       -> both sides quiet: parked stream
  rh frozen                          -> the APP stopped draining (not net)
