# xnibbler — GB-040 evidence bundle

- Game: `XNIBBLER.BIN` (launcher slot 18, arm)
- Commit: 8f5d2fc259356b252bfea2e493536e1fd90339a7
- Captured: 2026-10-17T09:29:56.162663+00:00

Assets landed in `evidence/{game}/<timestamp>/` (gitignored; see manifest.json there):

- `boot` screendump: `boot.ppm`
- `launch` screendump: `launch.ppm`
- `gameplay` screendump: `gameplay.ppm`
- scripted input: up,left,up,left
- `serial.log` (full boot + run), `replay.jsonl` (input trace)
- `manifest.json` (artifact sha256 hashes for provenance)

Verify: relaunch with `--arch {arch} --slot {slot}` and diff screendumps; the replay.jsonl reproduces the input.
