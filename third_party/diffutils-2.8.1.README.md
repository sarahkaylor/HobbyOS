# GNU diffutils 2.8.1 — vendored pin (cmp strict-parity reference)

Pinned for the HobbyOS `cmp` port's byte-exact acceptance harness
(`src/host/cmp_parity.sh`): the host-built port (`cmp_host`) is compared
against the ORIGINAL diffutils-2.8.1 `cmp`, compiled from these sources by
`src/host/build_diffutils_cmp_ref.sh`.  The ref build now resolves its
source tree in-repo, so a pristine checkout **or worktree** needs no
`third_party/_staging` extraction — this closes F1 carried item #3
("`_staging` seeding for host_tests in pristine worktrees").

Vendoring layout per AD-11 / Appendix C: **committed** = this README + the
tarball + its `.sha256`; **gitignored** = the extraction.  The extraction
lives at `obj/third_party/refs/diffutils-2.8.1/` (already covered by the
`obj/` ignore — no new `.gitignore` pattern needed), not in `third_party/`.

## Provenance

| Item | Value |
|---|---|
| Tarball | `diffutils-2.8.1.tar.gz` — 780,086 bytes |
| URL | https://ftp.gnu.org/gnu/diffutils/diffutils-2.8.1.tar.gz |
| sha256 | `c5001748b069224dd98bf1bb9ee877321c7de8b332c8aad5af3e2a7372d23f5a` |
| Verified against | `docs/gnu-ports.md` §6 pin table (recorded 2026-09-25, same hash); recomputed at vendoring 2026-09-30 (`sha256sum -c` green) |
| Extraction check | `diff -r` byte-identical to the pre-existing `third_party/_staging/diffutils-2.8.1/` tree (both come from this tarball) |
| License | GPL-2-or-later (upstream `COPYING`) — compiled into a host-only reference binary; not linked into shipped HobbyOS code |

## How the ref build uses it

`src/host/build_diffutils_cmp_ref.sh` resolves the source tree in priority
order:

1. `third_party/_staging/diffutils-2.8.1/` if present — legacy path, kept so
   existing trees (and the workstation `lane-gate.sh` `_staging` seeding,
   retained as belt-and-braces) behave exactly as before.
2. otherwise the committed tarball above: checksum-verified against
   `diffutils-2.8.1.tar.gz.sha256`, extracted once into the gitignored
   `obj/third_party/refs/diffutils-2.8.1/` build area (stamp
   `.diffutils-2.8.1.extract-ok` alongside it), and compiled from there.

Both paths carry identical source bytes, so the reference binary's
sources/flags/behavior are unchanged; failure messages name the fix
(restore the committed tarball, or re-fetch from the URL and verify).

## Extraction / rebuild (from a clean tree)

```sh
cd third_party
sha256sum -c diffutils-2.8.1.tar.gz.sha256
# extract into the gitignored build area the script would use:
tar -xzf diffutils-2.8.1.tar.gz -C ../obj/third_party/refs
```

or just run the official gate, which does checksum + extraction + build
itself (the cmp parity target is part of `make host_tests`):

```sh
make host_tests            # builds obj/arm/diffutils_cmp_ref + runs the full suite
make cmp_parity_strict     # byte-exact cmp parity alone (55 cases)
```
