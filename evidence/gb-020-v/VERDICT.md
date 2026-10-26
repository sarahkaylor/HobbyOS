# GB-020-V — gate evidence and verdict (VERIFIER, deep, swarm-farm-1)

Ticket GB-020-V (#180) · verifies GB-020 (#21) · ship = branch `ticket/020-getdents64` @ `b20b5a3`
Gate tree `4dff4d1` = `origin/main` `c71cb87` × branch (`ort` merge, clean), 12 files +357/−3
Run window 2026-10-26T05:31Z–05:50Z · worktree `hermes-b6dce8e3` (swarm-farm-1)

## VERDICT: RED

Clause 1 (host differential) passes with a real row count and a self-test that provably fails.
Clause 2 (in-guest rows) **fails**: the probe never got a directory to enumerate, so the
`getdents64` row family is absent from the guest. Clause 3 is met only in the weak "opens and returns
bytes" sense. Clause 4 was not run (the gate was already RED — no ARM claim is made).

| # | Clause | Result |
|---|---|---|
| 1 | `make t4_compat_parity` + `--self-test`, non-zero rows, self-test can fail | **PASS** — **84 rows per leg, byte-identical**; self-test flips `ho_open` → RED, restores → PASS |
| 2 | in-guest getdents64 rows matching golden (d_type/d_reclen/name, `.`/`..`) | **FAIL** — `dent_created rc=0`, `dent_opendir rc=-1`; no `dent_total` / `dent_have_*` rows |
| 3 | `/proc/self` stubs openable/readable, non-empty, parseable | **PARTIAL** — `procfd_opendir rc=0`, `procfd_read rc=1` (>0 bytes); content not parsed by the probe |
| 4 | ARM `make test` not regressed | **not run** — gate already RED; no claim either way |

### Clause 1 — quoted

    $ make t4_compat_parity
    t4 host differential: A=0 B=0 cases=3
    t4_parity: PASS (host differential locked; device leg gated)
    rc=0

`cases=3` is the harness's invocation counter, not its row count, so I re-ran both legs by hand exactly
as `src/host/t4_parity.sh` builds them and counted:

    musl-gcc -static -O2 -Wall src/host/t4_compat_probe.c      -> /tmp/legA2
    clang ... -DHOST_TEST -DLEG_B_SHIMS t4_compat_probe.c compat.c -> /tmp/t4probe.host
    rows A=84  rows B=84
    diff  -> empty (byte-identical stdout, both rc=0)

    $ make t4_compat_selftest
    self-test: OK — flipping ho_open FAILs (rc=0 clean pass), restoring passes
    rc=0

84 rows, not 0 — the GB-007 defect is not present. The two new rows are in both legs:
`procfd_opendir rc=0 errno=0`, `procfd_read rc=1 errno=0`.

### Clause 2 — the failing check, quoted from the guest

Probe built the way the corpus doc requires (`musl-gcc -static-pie` → `ET_DYN`, no `PT_INTERP`;
an `ET_EXEC` build is rejected by the loader: *"elf loader: ET_EXEC linked below the user window; a
-static-pie build is required"*), staged as `::T4PROBE.BIN` with a `::WAVE.CFG` naming only it:

    [WAVE-MANIFEST] T4PROBE.BIN
    [WAVE-TRY] T4PROBE.BIN
    [ARRIVED] T4PROBE.BIN rc>0
    row=dent_created rc=0 errno=0
    ...
    row=dent_opendir rc=-1 errno=9
    row=procfd_opendir rc=0 errno=0
    row=procfd_read rc=1 errno=0
    t4-probe-arrived / probe-exit
    System halt from CPU 3.          (halt=2)

against the same binary on Linux (leg A):

    row=dent_created rc=3 errno=0
    row=dent_opendir rc=6 errno=0
    row=dent_total rc=3 errno=0
    row=dent_have_alpha rc=1 errno=0   (beta, charlie likewise)

`dent_created rc=0` means **none of the three files were created** in-guest, and `dent_opendir` got a
negative fd, so the enumeration loop — the only place the `getdents64` row layout is exercised — never
ran. (`errno=9` in the `dent_opendir` row is the probe's own failure marker, not an errno; see
`src/user/t4_probe.c:228`, so it names no OS error. The probe also ignores `mkdir`'s return, so its own
setup is unverified in-guest.)

**Attribution, stated carefully:** this is **not** evidence that the new `getdents64` code is wrong. The
failure is upstream of it — file creation through the guest's FAT16 create path produced zero successes,
and that family is GB-067 (#76, `In Progress`, whose verifier gate I ran earlier today and RED'd on the
same create/stage path). What is *not* met is the card's clause as written: in-guest rows matching the
golden. The `getdents64` path was never reached.

**And it is not the wave-halt blocker the card anticipated:** this manifest-only wave reached
`System halt` (halt=2) with no IDLESTUCK, so GB-057/#184 is not the reason clause 2 is red.

### Clause 3 — what is actually proven

`/proc/self/fd` opens (`procfd_opendir rc=0`) and `getdents64` on it returns bytes (`procfd_read rc=1`,
i.e. `nn > 0`). The probe asserts only "bytes > 0" — it does not parse the entries or check `.`/`..`
offsets, and `/proc/self/stat`-class fields are not probed at all. So "non-empty and parseable as the
fields SDL asks for" is **unproven**, not proven.

### What would make this GREEN

1. Make directory creation work in-guest (GB-067's FAT16 create path) so the probe's own `mkdir` +
   `fopen` produce a real directory, **or** point the probe at a directory that already exists on the
   image (e.g. the staged root) so `dent_*` rows are reachable without the create path.
2. Re-run this gate; the row set must then match the golden including `dent_total`, the three
   `dent_have_*` rows, and the struct fields (`d_type`, `d_reclen`, `d_off` offsets).
3. Have the probe assert `/proc/self` content shape, not just `bytes > 0`, if clause 3 is to mean what it
   says.
4. Run ARM (`make test`) once the above is green, since `fs.c`/`fat16.c`/`trap.c` are shared.