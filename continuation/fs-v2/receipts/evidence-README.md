# fs-v2 evidence — T2 dry run receipts

Generated 2026-10-06 by FS-lane V2 (brief: T2 dry on the current tip via the
V1 acceptance runner, own instance v2).

Runner: V1's canonical multi-mode `tools/run_browser_accept.sh` (fs-v1 lane;
my tree's `tools/` copy is the legacy WC runner until V1's merges).
Instance scoping: `--instance v2` / `ACCEPT_ID=v2`, own lock
`/tmp/fs-accept-v2.lock`, own port **8855**, own sockets
`/tmp/br-wc-ctrl-v2.sock` + `/tmp/br-wc-qmp-v2.sock`, own port-file
`/tmp/wiki-proxy-port-v2`.  Default 8800 and the F-R2 `/tmp/wiki-proxy-port`
contract untouched.

Disk: `fs-v1/disk.img` — built from the merged OS tip (61f6dbf-era == this
tip 8a5558f minus two exec-bit-only third_party commits; kernel bytes
identical) with the **pre-R1-fix** windowed WebProcess (`::/BROWSER.BIN` flat
sha 943bdff8fa9fb9e9 == source ELF flat), i.e. exactly the current merged
fork tree 4536f622cc.  Deliberately NOT the R1-fixed binary (054b307db1 is
unmerged) so full-skin rows classify gated-OQ-1, not false-pass.

Fixture dirs exported to the runner (canonical staging, this lane's map):
`/home/sarah/webkit-lanes/fs-v2/HobbyOS/continuation/wk3/fixtures` (FIX01-05)
+ `/home/sarah/hobbyos-lanes/fs-v2/tests/fixtures/browser` (HOME/TALL).

## Runs

| Dir | Mode | Report | Verdict | Notes |
|---|---|---|---|---|
| `dry-fixture/` | fixture | report.json v2 | (from run) | 8 rows FIX01-05/FIX02D/HOME/TALL |
| `dry-reader/` | reader | report.json v2 | (from run) | reader/Hobbyist_operating_system |

Direct/full modes: classified **gated — OQ-1 fix pending merge (R1
054b307db1 + rebuild)**; not run this dry per the brief (R1's decisive
re-test is running in parallel on its own slot).
