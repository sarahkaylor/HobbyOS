# RUN-REQUEST — lane N2 (instrumented kernel)

## Status
Instrumentation landed: `bw-perf-net2` @ `63460da` (net.c counters + watchdog
`[NETDBG]` probe; no behavior change). Merged into `perf-x64` (HEAD = 63460da).

## Run 804 (ALREADY LAUNCHED by controller 2026-10-09 ~10:15)
```
cd ~/hobbyos-lanes/perf-x64 && \
NO_SCRIPT=1 WP_ELF=/home/sarah/hobbyos-perf-lanes/pinned/w2-intel5/WebProcess \
bash tools/run_x64_browser.sh --url cnn.com --run 804 \
  --outdir /home/sarah/hobbyos-perf-lanes/w2/evidence/x64-run804
```
## After 804 — greps
```
grep -a 'NETDBG' w2/evidence/x64-run804/serial.log | tail -40
grep -a 'flow-control\|net fetch\|load-ok\|SR-t\|NET' w2/evidence/x64-run804/serial.log | tail -30
```
## Next step (fix decision tree)
- If `mac=0` on the stalled socket with frozen acktx/wupd → remove/harden the
  `mac_cached` gates: resolve+cache the MAC at connect time (syscall context,
  safe to block); in IRQ paths send with cached-or-gateway-broadcast and count.
- If `rxseg` frozen with `acktx` growing → our TX isn't reaching slirp:
  inspect `virtio_net_send` TX ring reclaim and the batch-notify path.
- If `rh` frozen (`rtmo` growing) → the app-side drain stopped: the stall is
  in the browser/fetch-bridge, not the kernel TCP; hand to the fork lanes.
- If all counters frozen including rxseg, with `wupd>0` → parked-stream
  livelock: add a bounded post-drain re-ACK keepalive (send window-update ACK
  also on `net_socket_ready` polls when idle > 1s and rx_head advanced since
  the last wupd).

## Ops notes
- The `[NETDBG]` dump runs in the watchdog context with NO locks (pure racy
  reads; watchdog must never block). Do not add locking there.
- Watchdog dumps appear on COM1 **and** the raw COM2 side-channel; COM1 lines
  can interleave with the browser's [WIN] writer — parse tolerantly.
