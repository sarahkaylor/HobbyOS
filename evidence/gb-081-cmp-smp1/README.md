# GB-081 / GB-086 — the ARM tier's red set depends on the CPU count

Two runs of `tools/t3_tier.sh arm` on the same tree family, differing only in `T3_SMP`:

| | 8 CPUs (`-smp 8`, `4fa985d`) | 1 CPU (`T3_SMP=1`, `2d92fe2`) |
|---|---|---|
| console lines | 31,990 | 8,875 |
| FAIL lines | 4 | 3 |
| CMPTEST | **2 FAIL / 19 pass** (`FAIL /EXPAND.BIN-b quotes differing bytes`, `FAIL -s prints nothing`) | **19 PASS / 0 FAIL** |
| POLLTST timeout check | PASS | **FAIL** (`POLLTST elapsed_ms=` printed *empty*) |
| SHELLTEST watchdog | FAIL | FAIL (GB-058, both) |
| glued console lines (GB-082 counter) | 540 | 321 |

**What this proves:** the ARM red is not one defect. `CMPTEST`'s two failures need
8 CPUs to appear (a fixture-visibility/race question, GB-081); `POLLTST`'s
select-timeout check needs 1 CPU to appear (a timing question, GB-086, whose own
diagnostic prints no number). And `SHELLTEST` stalls at both (GB-058).

**Why the evidence is committed rather than regenerated:** the 8-CPU log is from
the pre-fix tree (`4fa985d`), so the comparison cannot be re-made byte-for-byte —
these are one-off measurements, force-added per the repo's `evidence/` convention.
