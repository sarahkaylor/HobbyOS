# Browser fixtures — canonical staging (T0 ladder + OS start pages)

Owned by FS-lane V2 (brief: reconcile fork `wk3/fixtures` FIX01-05 vs OS
`tests/fixtures/browser/` HOME/TALL).  Last reconciled: 2026-10-06.

## Two authoritative sets, one `/fixture/<name>` route

The acceptance runner (`tools/run_browser_accept.sh` V1, modes) serves every
T0 fixture **byte-identical** from the two in-tree directories below.  The
proxy `--fixtures` search list (colon-separated, first hit wins) is:

1. fork `HobbyOS/continuation/wk3/fixtures/`   ← authoritative for FIX01..FIX05 (+ FIX02D) and their sub-resources
2. OS  `tests/fixtures/browser/`               ← authoritative for HOME / TALL (browser start + scroll pages)

The sets are **disjoint by name** (no FIX* in the OS dir, no HOME/TALL in the
fork dir), so the search order never causes ambiguity.  This README is the
canonical mapping; the two trees are the single sources of truth (no third
copy of either set is authoritative anywhere else on the host — V1's `fs-v1`
lane, `ib`'s `fs-r1` evidence and the review copy may carry *copies* for
their own runs; do not treat them as authoritative for staging).

## Row → source file → checksums (2026-10-06 tips)

| Row (runner id) | Authoritative file | Bytes | sha256 (file) | On-device FPC anchor (V1/T2 dry) |
|---|---|---|---|---|
| fixture-FIX01  | fork `wk3/fixtures/FIX01.HTM`  | 267  | fe4df6bc781989af…  | **0x759431c5** (known-good) |
| fixture-FIX02  | fork `wk3/fixtures/FIX02.HTM`  | 314  | aa45c5de77dfecf7…  | 0x63309a45 |
| fixture-FIX02D | fork `wk3/fixtures/FIX02D.HTM` | 507  | 7e8953f17c5349d3…  | 0x5f62b9c5 (paints; gated load-timeout, F6/F7 → WN2) |
| fixture-FIX03  | fork `wk3/fixtures/FIX03.HTM`  | 578  | cfdce5d1a2234189…  | 0xb1e75dc5 |
| fixture-FIX04  | fork `wk3/fixtures/FIX04.HTM`  | 642  | aa50f7eb61772cc6…  | 0xbc622575 |
| fixture-FIX05  | fork `wk3/fixtures/FIX05.HTM`  | 440  | d2ae22e802e33344…  | 0xa0bf6dc5 (paints; gated load-timeout, F6/F7 → WN2) |
| fixture-HOME   | OS `tests/fixtures/browser/HOME.HTM` | 483  | 3e03777ccb302615…  | **0x0e8f25c5** (known-good) |
| fixture-TALL   | OS `tests/fixtures/browser/TALL.HTM` | 817  | 47e0fe49aabd13c4…  | 0xe0982225 |

Anchors `0x759431c5` (FIX01) and `0x0e8f25c5` (HOME) are the known-good
on-device frame checksums from V1's committed report
(`fs-v1/evidence/fs-v1/demo-fixture/report.json`, rows fixture-FIX01 and
fixture-HOME) and from the WK-3/WK-5 white-paint 0x0e8f25c5 series; they are
the deterministic pixels of the 320×240 render, not file hashes.

Support files living in the fork dir (not fixture rows themselves): `IMG01.PNG`
(FIX02's `<img src="IMG01.PNG">`; 133 B, sha c899801c…), `PROBE01.JS` (314 B,
sha 9a5ffb80…), `BUNDLED-FONT.TTF` (759 720 B, sha b4c632e3…).

## Disk staging (the second copy)

The `Makefile` disk recipe installs the OS pair on the boot disk as
`::/HOME.HTM` and `::/TALL.HTM` (start page + scroll page for the desktop
browser).  The fork FIX set is **not** baked onto the disk — fixtures are
served live by the proxy `/fixture/<name>` route so the host stays the single
writer (edit + Rerun, no disk rebuild).  This asymmetry is intentional:

- HOME/TALL have a dual role (on-disk start pages **and** `/fixture` rows) —
  both roles read the **same** `tests/fixtures/browser/` file.
- FIX01-05 exercise render/parse paths only, and only over the wire; keeping
  them host-side means fixture edits never require a disk rebuild.

## Verification commands (host, no QEMU)

```sh
# byte-identical check: what the proxy serves == what is committed
python3 tools/test_wiki_proxy.py          # F-R2 guard: 13/13 (incl. /fixture byte-exactness)

# or by hand
diff <(curl -s http://127.0.0.1:PORT/fixture/FIX01) fork/.../W3/fixtures/FIX01.HTM
```

## Update protocol

- Any FIX01-05 / FIX02D / IMG01 / PROBE01 / BUNDLED-FONT change: edit the
  fork dir only (fork owner), then update this table + re-run the fixture leg.
- Any HOME/TALL change: edit `tests/fixtures/browser/` here (OS lane V2
  owns these refs), update the disk (`make … disk.img` re-bakes them) and
  this table.

Diff-status against the working-reference copies on this host (2026-10-06):
fork `fs-v2` tip `4536f622cc` FIX set == V1's `fs-v1` copy (byte-identical —
V1 ran from its own fork snapshot, which is content-equal to the merged tip);
OS HOME/TALL == `~/Documents/GitHub/HobbyOS/tests/fixtures/browser` (byte-identical).
