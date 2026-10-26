# GB-066-V3 — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-066-V3 (#207) · verifies GB-066 (#74) · ship = branch `ticket/066-deviceleg` head `fbd3cf3`
("build the T4 device probe as static-PIE (ET_DYN) so the v2 loader runs it in-guest"; Makefile only,
+10/−1)
Gate tree `357b871` = `origin/main` `44e4672` × `fbd3cf3` (`ort`, clean)
Run window 2026-10-26T22:17Z–23:07Z, worktrees `hermes-bbfc7fc4` (waves) and `hermes-066v3b` (host suite)
Local only — no CI consulted.

## VERDICT: GREEN

The wall this card re-scopes has moved: a static-PIE probe is dispatched by the v2 loader and **runs
in-guest** (17 `row=` lines, `probe-exit`). The row count is GB-108's and is not graded here.

| # | Clause | Result |
|---|---|---|
| 1 | `readelf` on the built probe: DYN, entry above `USER_VA_BASE` | **PASS** — `Type: DYN (Position-Independent Executable file)` |
| 2 | dispatch + in-guest run, quoted with a row count | **PASS** — `[ARRIVED] T4PROBE.BIN rc>0`, `probe-exit`, **17** `row=` lines |
| 3 | build asserts DYN; an ET_EXEC image fails the build | **PASS** — `make: *** Error 1`, rc=2 |
| 4 | absent probe → FAIL, not SKIP | **PASS** — "This is a FAIL, not a SKIP (DoD clause 3)" |
| 5 | `make host_tests` passes | **PASS** — 482 checks, 0 failed |

### 1 — the image class

    $ make ARCH=intel obj/intel/t4_probe.elf            build-rc=0
      Type:                              DYN (Position-Independent Executable file)
      Entry point address:               0x1cc1

`src/include/vm.h:27: #define USER_VA_BASE 0x1000000000ULL`.

**Precision note:** for a static-PIE the header entry is a link-time offset (`0x1cc1`), not an absolute
address — the *load base* is what places the image above `USER_VA_BASE`, and the guest running the probe
(clause 2) is the evidence that it landed there. Read as "the entry in the ELF header is above
`USER_VA_BASE`" the clause would be unsatisfiable for any PIE; read as "the loader maps it above the user
window", it holds and is demonstrated.

### 2 — dispatched and running in-guest (intel T3, probe staged as `::T4PROBE.BIN` + `::WAVE.CFG`)

    [WAVE-MANIFEST] T4PROBE.BIN
    [WAVE-TRY] T4PROBE.BIN
    [ARRIVED] T4PROBE.BIN rc>0
    t4-probe-arrived
    probe-exit
    --- row count: 17
    row=write3 rc=3 errno=0 out=68690a
    row=uname_rc rc=-1 errno=1
    row=uname_sysname_linux rc=0 errno=0

The wave as a whole still reports `FAIL — only 1 programs exited (floor 55)`, which is the *corpus* shortfall
(a single staged program is not a complete wave); it is not a statement about the probe, and the probe's own
arrival, execution and exit are all present. 17 rows in-guest against 41 on Linux is **GB-108 (#206)** —
the differential — and I have graded nothing about it here.

### 3 — the assertion can fail

    $ sed -i '3542s/-static-pie -fPIE/-static/' Makefile        # my edit, then build
    t4_probe: build is not ET_DYN/static-PIE — the v2 ELF loader rejects it (GB-066-V2)
    ELF Header:
      Class:                             ELF64
      ...
    make: *** [Makefile:3543: obj/intel/t4_probe.elf] Error 1     build-rc=2

restored → the `-static-pie -fPIE` recipe returns, build rc=0, `Type: DYN`.

### 4 — absence is a FAIL, not a SKIP

    $ T4_SERIAL=<capture with no probe> T4_LEGA_OUT=<41-row leg A> bash tools/gate/t4_device.sh
    t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
      in … after 120s).  This is a FAIL, not a SKIP (DoD clause 3).        rc=1

### 5 — the host suite

    === Test Results ===
    Checks run:    482
    Checks failed: 0
    ALL APPS SUITE TESTS PASSED
    TEST EXIT: 0                                        host_tests-rc=0

## Measurement error I made, and what it cost (disclosed, not hidden)

My **first two waves** (22:18 and 22:36) both ended `[ARRIVED] T4PROBE.BIN rc<0` with the loader saying

    elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)

That was **my** artefact, not the PR's: the ET_EXEC binary produced by clause 3's break test was written in
the same second as the restored Makefile, so `make` judged it up to date and the tier staged the broken
image twice. `readelf` on the staged file confirmed `Type: EXEC`, entry `0x401cc1`. Removing it and
rebuilding (`rm -f obj/intel/t4_probe.elf && make …`) gave `Type: DYN`, entry `0x1cc1`, and the wave above.

Two consequences worth recording: the two `rc<0` waves are **not** evidence against this PR, and they are
an unintended end-to-end confirmation of the assertion's rationale — an ET_EXEC image does reach the guest
and is rejected by exactly the loader branch the Makefile now guards against.

`make host_tests` was run in a second worktree (`hermes-066v3b`, same gate tree) because a first invocation
in the wave worktree hung past 20 minutes with no output while a concurrent second invocation was also
waiting; the clean worktree finished in ~6 minutes with the numbers above. Nothing about the tree differed
— same `357b871`.