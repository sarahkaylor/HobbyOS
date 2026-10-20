# XNIBBLER — GB-040 evidence bundle

- Game: `XNIBBLER.BIN` (launcher slot 19, arm)
- Commit: 98247dd9eccbf9ec434a8841abb1b3b267dea5f5
- Captured: 2026-10-20T03:35:53.210273+00:00

Assets landed in `evidence/{game}/<timestamp>/` (gitignored; see manifest.json there):

- `boot` screendump: `boot.ppm`
- `launch` screendump: `launch.ppm`
- `gameplay` screendump: `gameplay.ppm`
- `serial.log` (full boot + run), `replay.jsonl` (input trace)
- `manifest.json` (artifact sha256 hashes for provenance)

Verify: relaunch with `--arch {arch} --slot {slot}` and diff screendumps; the replay.jsonl reproduces the input.
