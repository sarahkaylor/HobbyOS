# render-pipeline verification + measurement harness (`tools/render_pipeline/`)

Lane rp-h. Probes for the render-pipeline workstream — reproduce/characterize
issue #1 ("browser draws outside its window / doesn't respect the WM layout"),
extract wall-clock rendering receipts, and baseline close responsiveness.

Everything is runnable against ANY WebProcess build via the meta-runner, so
rp-f's checkpoints get re-verified with the same probes.

## Files

| File | Purpose |
|---|---|
| `rp_lib.py` | shared harness: ELF sha pinning, FAT16 disk assembly (wk5 pattern), detached QEMU boot with saved PID, QMP client, serial-marker polling, Apps-menu launching, PPM read/diff |
| `perf_receipts.py` | parse a WK-5 serial log -> `[WIN]` rendering receipts (JSON + table); `--repeat N` for run-to-run stats |
| `tile_bleed_probe.py` | issue-1 probe: load fixture, re-tile via a second window (CONSOLE), QMP screendumps + change census outside the content rect; scenarios `gentle` / `load` / `settle` / `openclose` / `perf` |
| `resp_close_probe.py` | close-responsiveness: F4 + titlebar-X close mid-load / mid-settle / at load-ok; latency + grace assertion |
| `run_rp.sh` | meta-runner: `run_rp.sh <probe> --wp-elf PATH [--instance id --port N]` |

Results land in `tools/render_pipeline/evidence/<probe>-<instance>-<ts>/`
(`report.json` + serial + PPM screendumps). Baselines: `docs/browser/render-pipeline-baselines.md`.

## Quick start

```bash
# receipts from an existing serial log (deliverable gate)
python3 tools/render_pipeline/perf_receipts.py <run-WK5-*.log> --json out.json

# full probes against the frozen binary (defaults)
tools/render_pipeline/run_rp.sh perf      --wp-elf ~/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess
tools/render_pipeline/run_rp.sh tile-bleed --wp-elf $WP --scenario gentle
tools/render_pipeline/run_rp.sh resp-close --wp-elf $WP --scenario midload
```

## Environment

- **OS scratch tree** (RO input): `RP_OS_TREE` (default
  `/home/sarah/hobbyos-scratch/rp-h-os`) — a copy of the OS tree with
  `hobbyos.elf` and `obj/arm/{desktop,console}.bin` already built. The disk
  assembly never builds in the lane/main worktrees.
- **Fixtures**: `RP_FIXTURES` (default the wk5 `fixtures/` with HOME.HTM /
  TALL.HTM). `RP_SMP` / `RP_MEM_MB` for QEMU sizing (default 4 / 4096).
- **Frozen WebProcess ELF** (baseline target):
  `/home/sarah/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess` —
  its sha256 is pinned in every report and in the baselines doc.
- **Firmware**: AAVMF from `~/.local/share/AAVMF/AAVMF_CODE.fd`
  (fallback `/usr/share/AAVMF/AAVMF_CODE.fd`). Tools needed: `qemu-system-aarch64`,
  `clang`, `llvm-objcopy`, `mkfs.fat`, `mtools` (`mmd`/`mcopy`), `python3`.
- **Sockets/locks**: `/tmp/rp-<instance>[-<port>]-qmp.sock`, `-serial.log`,
  `/tmp/rp-<instance>[-<port>].lock`. Two lanes may run in parallel iff
  they use distinct `--instance` AND distinct `--port`.
- **QEMU discipline**: one QEMU per disk; processes are detached with the PID
  saved to `<evdir>/qemu.pid`; the probes kill ONLY that PID. Never bare
  `pkill -f qemu-system*` on a shared host.

## Markers parsed (byte-compatible with the frozen driver tip)

`[WIN] frame-times render=ms blit=ms total=ms`, `viewport apply begin/end ms=`,
`reblit cached`, `load-ok url=… ms=…`, `frame view=… checksum=…`, `geom`,
`painted rect`, `close ok`, `exit rc=…`.

## Gate mapping (docs/browser/render-pipeline.md §Gates)

| Workstream gate | Probe | Pass condition |
|---|---|---|
| tile-bleed probe: zero out-of-rect pixels | `tile_bleed_probe.py` (gentle + load + settle + openclose) | all capture pairs `unattributed == 0` (masks: browser content rect, chrome, taskbar, second-window rect, menu, cursor) |
| perf table before/after | `tile_bleed_probe.py --scenario perf`, `perf_receipts.py` | same command + ELF sha, N=3 runs on HOME+TALL |
| responsive-close green under load | `resp_close_probe.py` (midload/midsettle/starvation/titlebar) | `[WIN] close ok` + `exit rc=0` within grace |

Honest-limitation note: a "NO-BLEED-OBSERVED" verdict on the frozen binary is
a legitimate baseline; the issue may still reproduce under different traffic
(captured per-scenario in the baselines doc with exact commands + evidence).
