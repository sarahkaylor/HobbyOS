# GB-203-V — VERDICT: GREEN

- Graded: PR #370 head **09cf771730dce4483a38f0c5e4627dab101210a3** (`x11: let awk drain in build-server.sh (a silent SIGPIPE killed the build)`)
- Base for the pre-change run: merge-base `5efe9a40` (origin/main, predates the PR)
- Host: swarm-farm-3; CI not consulted (policy 2026-10-26). Host-side gate, no T3 boot.
- Build cache: a populated `third_party/x11/build/intel` tree copied from a lane worktree; `X11_DL_DIR` pointed at its populated `dl/`. The xserver re-links from the cached source/obj tree; every stage run below is attributable.

## Clause 1 — the reads drain (PASS)

Head `third_party/x11/build-server.sh:145-146`:

```
base="$(llvm-readelf -l "$HB_BUILD/sym/XHOBBY.elf" | awk '$1=="LOAD" && !f{print $3; f=1}')"
start="$(llvm-nm "$HB_BUILD/sym/XHOBBY.elf" | awk '$3=="_start" && !f{print "0x"$1; f=1}')"
```

Post-change, five runs each on the elf built at the head (`pipelines_post.log`):

```
=== new readelf pipeline (5x) ===
run1 rc=0 ... run5 rc=0
=== new nm pipeline (5x) ===
run1 rc=0 ... run5 rc=0
```

## Clause 2 — the pre-change failure is real (PASS, with a disclosed sample caveat)

The base tree (`5efe9a40`) still has the early `exit`:

```
base="$(llvm-readelf -l "$HB_BUILD/sym/XHOBBY.elf" | awk '$1=="LOAD"{print $3; exit}')"
start="$(llvm-nm "$HB_BUILD/sym/XHOBBY.elf" | awk '$3=="_start"{print "0x"$1; exit}')"
```

Pipeline sample, 5 runs each (`pipelines_post.log`): old readelf **4x rc=141, 1x rc=0**; old nm **5x rc=141**. The single rc=0 is the same SIGPIPE race the fix addresses (readelf can finish writing before awk's exit closes the pipe). A 20-run sample of both old pipelines (`race_counts.log`) is **20/20 rc=141 each**, so the failure is real; the card's "five times" was a sample, not a deterministic count, and this host's first sample lost the race once.

The real build reproduces it. Four pre-change stage runs (`server_pre.log`, `server_pre2.log`, `server_pre3.log`, `server_pre4.log`) each:

```
[x11:intel] pixman: cached
[x11:intel] libfontenc: cached
[x11:intel] libXfont2: cached
[x11:intel] libxkbfile: cached
[x11:intel] libsha1: cached
[x11:intel] xserver: building XHOBBY.BIN (meson)
```
then exit **141** with no error message. In the last run the freshly linked artifacts are timestamped at the run's own time and the ninja log ends at the link, so the exit is after a clean link:

```
-rwxr-xr-x 1 sarah sarah  2734184 2026-11-04T17:14:34Z .../bin/XHOBBY.BIN
-rwxr-xr-x 1 sarah sarah 11721240 2026-11-04T17:14:34Z .../sym/XHOBBY.elf
[311/311] Linking target hw/hobbyos/Xhobby
```

## Clause 3 — the stage completes (PASS)

Post-change (`server_post.log`, rc=0):

```
[x11:intel] xserver: building XHOBBY.BIN (meson)
[x11:intel] xserver: ok (/home/sarah/.hermes/cache/scratch/x11build-gb203/bin/XHOBBY.BIN, 2734184 bytes)
[x11:intel] xkm: /home/sarah/.hermes/cache/scratch/x11build-gb203/disk/X11/US.XKM (12316 bytes)
[x11:intel] == stage server done
```

## Clause 4 — suites and links (PASS)

```
$ make host_tests
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0

$ make ARCH=intel hobbyos.elf   rc=0
$ make ARCH=arm   hobbyos.elf   rc=0
```

## Clause 5 — no stage skipped (PASS)

Post-change `--stage all`, rc=0 (`stage_all.log`); every stage named and completed (stage markers, `grep '== stage'`):

```
[x11:intel] == stage clientlibs
[x11:intel] == stage clientlibs done
[x11:intel] == stage extlibs
[x11:intel] == stage extlibs done
[x11:intel] == stage server
[x11:intel] == stage server done
[x11:intel] == stage icewm
[x11:intel] == stage icewm done
[x11:intel] == stage clients
[x11:intel] == stage clients done
```
No `skipped` line.

## Verdict

All applicable clauses pass at 09cf7717. The fix is not cosmetic: the old stage exits 141 four times out of four after a clean link, while the new stage completes and names all stages. GREEN.
