# GB-020-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-22.
Tree: `origin/main` @ `46f19bb`, clean worktree; `disk.img` built here; intel waves run here.

**Both holds named in the card have lifted** — GB-072 (manifest dispatch) and GB-073 (the six
`dent_*` rows) are both done, and I re-measured that they are real:

```
$ sed -n '143,156p' src/kernel/main.c        # GB-072: WAVE.CFG dispatch
  143:  /* GB-072: if a manifest is on disk, the wave IS that manifest … */
  155:    if (fat16_open("WAVE.CFG", &mfile) == 0) {
$ grep -c 'prow("dent' src/user/t4_probe.c
6                                            # GB-073: all six rows present
$ ./obj/intel/t4_probe.elf | grep "^row=dent"
row=dent_created rc=3 errno=0   row=dent_opendir rc=6 errno=0   row=dent_total rc=3 errno=0
row=dent_have_alpha rc=1        row=dent_have_beta rc=1         row=dent_have_charlie rc=1
```

**The block has moved one layer down: the guest cannot execute the probe to its first instruction.**
That is a different wall from GB-072's, and it is measured, not inferred.

## 1. `--self-test` — PASS (the gate can fail, and says so)

```
$ bash tools/gate/t4_device.sh --self-test
[self-test] clause 2 (wrong shim must FAIL):   OK — wrong shim FAILED as required
[self-test] clause 3 (absent probe must FAIL, not skip): OK — absent probe FAILED as required
self-test result: fail_count=0        EXIT=0
```

## 2. Shipped probe recipe — the loader refuses the binary

```
$ T3_STAGE='mcopy … obj/intel/t4_probe.elf ::T4PROBE.BIN && mcopy … /tmp/wave.cfg ::WAVE.CFG' \
  bash tools/t3_tier.sh intel
[WAVE-MANIFEST] T4PROBE.BIN
[WAVE-TRY] T4PROBE.BIN
Loading program for scheduler (v2 AS): T4PROBE.BIN
/T4PROBE.BIN
elf loader: ET_EXEC linked below the user window; a -static-pie build is required (docs/elf-loader.md §1)
[ARRIVED] T4PROBE.BIN rc<0
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000000 … pd480=0x00000000810000FB
```

The Makefile rule is `musl-gcc -static` (`Makefile: T4_PROBE_ELF`), which produces **ET_EXEC** at
`0x400000` — below the user window the loader allows. So the instrument GB-020 is gated by is built in a
flavour the kernel has never accepted.

**Secondary defect, visible in the same log:** after the refused load the kernel takes a GP fault and
**halts the machine** (`vector=13 rip=0x0`, then `[HALT-CD] … exited=2`). A load failure should fail the
process, not kill the wave — the wave exited 0 programs and the tier correctly reported
`FAIL — only 0 programs exited (floor 55)`.

## 3. Rebuilt `-static-pie` (the flavour the loader asks for) — it arrives, then dies at rip=0

```
$ musl-gcc -static-pie -O2 -Wall src/user/t4_probe.c -o /tmp/t4_probe_pie.elf
$ readelf -h /tmp/t4_probe_pie.elf | grep Type
  Type:  DYN (Position-Independent Executable file)      # 39 rows on the host
… staged as T4PROBE.BIN, wave re-run …
[ARRIVED] T4PROBE.BIN rc>0
[KERNEL] FATAL: Exception in Kernel Mode! Vector: 13 … User process fault! Vector: 13
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=13 rip=0x0000000000000000 …
```

`rc>0` means the image was accepted this time, then the process faulted with `rip=0` — the entry point
was never established (a musl `-static-pie` enters through `rcrt1.o`, which self-relocates and expects
the aux-vector/`AT_PHDR` contract). So the loader maps static-PIE but does not start it correctly.

Also note the message: the fault is a **user** process fault, yet it is announced as
`Exception in Kernel Mode` and halts the CPU. That mislabelling is why a user-space crash reads as a
kernel panic in the tier's verdict line.

## 4. Comparator — FAIL, not SKIP (correct)

```
$ T4_LEGA_OUT=obj/t4_legA.out T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh
=== t4_device: leg A reference: obj/t4_legA.out ===
=== t4_device: capture leg B from serial 'obj/t3_intel.log' ===
t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
  in obj/t3_intel.log after 120s).  This is a FAIL, not a SKIP (DoD clause 3).
```

## DoD table

| # | criterion | result |
|---|---|---|
| 1 | `t4_device.sh --self-test` exits 0 | PASS |
| 2 | every named row agrees with leg A | **FAIL — unmeasurable**: the guest never executes the probe (ET_EXEC refused; static-PIE arrives then faults at rip=0) |
| 3 | state whether anything was relaxed | **nothing relaxed** — no row was compared, so no tolerance was applied; `dent_total` was never reached |

## What closes it (in order)

1. **Loader:** start a musl `-static-pie` correctly (entry from `e_entry`, aux vector incl. `AT_PHDR`,
   and the self-relocation path `rcrt1.o` expects), or document the flavour it *does* accept and make the
   probe build match it. This is the GB-013/`docs/elf-loader.md` surface.
2. **Probe build flag:** `Makefile`'s `musl-gcc -static` must become `-static-pie` once (1) holds, or the
   probe can never be dispatched.
3. **Fault handling:** a refused load / user fault must kill the process, not halt the CPU, and must not
   be labelled `Exception in Kernel Mode`.
4. Then re-run this card: the six `dent_*` rows can finally be compared against leg A (`06_legA_rows.txt`).

Nothing here says GB-020's shims are wrong — **they have never been executed**, so this verdict is about
the instrument, and GB-020 keeps its `In Progress` with that stated plainly.
