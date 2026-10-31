# GB-138-V4 — RED at 020f6a63 (verifier run, 2026-10-31)

Graded head **`020f6a63`** (PR #261). Clean worktree, `third_party` staged, intel build from scratch.

**VERDICT: RED sha=020f6a63**

The gate is well built — it derives `T3_LOG`, refuses to grep a stale file, and the two `mcopy`
lines are in `T3_STAGE`. The problem is the tree it was graded on: **the GB-140 contract it
depends on is not in this head**, so clause 4 cannot be satisfied and clause 7's target does not
exist.

## What passes

**Clause 1 — `disk.img` does not carry the compat member by default** ✓

```
$ make -n disk.img ARCH=intel | grep -c w0_hello_pie
0
```

**Clause 2 — the member is built by a real target, and it is ET_DYN** ✓

```
$ make ARCH=intel obj/intel/w0_hello_pie.elf        BUILD_RC=0
$ file obj/intel/w0_hello_pie.elf
ELF 64-bit LSB pie executable, x86-64, …, static-pie linked, with debug_info
$ readelf -h obj/intel/w0_hello_pie.elf
  Type:   DYN (Position-Independent Executable file)
  Entry:  0x11b8
```

**Clause 3 — staged in `T3_STAGE`, manifest names the member** ✓

```
tools/gate/t4_w0_dispatch.sh:44:
STAGE="mcopy -o -i $ROOT/disk.img $PIE ::W0PIE.BIN; mcopy -o -i $ROOT/disk.img $CFG ::WAVE.CFG"

tests/fixtures/abi/WAVE.CFG: HELLO.BIN / CAT.BIN / SH.BIN / LS.BIN / W0PIE.BIN
```

## What fails

**Clause 4 — `abi=0x1` in a real wave: unsatisfiable at this head.** The gate derives the console
exactly as GB-140 asks:

```
tools/gate/t4_w0_dispatch.sh:54:  LOG="$(sed -n 's/^T3_LOG=//p' "$OUT" | tail -1)"
tools/gate/t4_w0_dispatch.sh:55:  if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
tools/gate/t4_w0_dispatch.sh:56:    echo "t4_w0_dispatch: FAIL — t3_tier printed no usable T3_LOG (GB-140 clause 3)"; exit 1
```

and on a full intel build that is exactly what happens:

```
$ make disk.img ARCH=intel                     DISK_RC=0
$ bash tools/gate/t4_w0_dispatch.sh
=== t4_w0_dispatch: booting intel guest with W0PIE-only manifest (GB-138) ===
t4_w0_dispatch: FAIL — t3_tier printed no usable T3_LOG (GB-140 clause 3)
DISPATCH_RC=1
```

The cause is in the tree, not the gate:

```
$ grep -n "T3_LOG" tools/t3_tier.sh
23:# Env:    T3_TIMEOUT …, T3_LOG (default obj/t3_<arch>.log)
163:LOG="${T3_LOG:-$ROOT/obj/t3_${ARCH}.log}"

$ git show origin/main:tools/t3_tier.sh | grep -n "T3_LOG="
35:  echo "T3_LOG=$LOG"

$ git merge-base --is-ancestor 74a6adf HEAD   # GB-140 on main
74a6adf NOT in head
$ git log --oneline -1 $(git merge-base HEAD origin/main)
c5308644 plan: record GB-138 … (#260)
```

So the head predates GB-140 by three commits; the tier never prints the contract line, and the
gate — correctly — fails loudly on every run. A gate that can only ever fail on its own tree
cannot evidence `abi=0x1`.

**Clause 7 — `make t4_t3log_selftest` does not exist here**

```
$ make t4_t3log_selftest
make: *** No rule to make target 't4_t3log_selftest'.  Stop.        ESC=2
```

The target lives on main (74a6adf), not in this head.

**Clause 5 — the falsifier cannot distinguish anything at this head.** The verdict is `FAIL`
before and after removing an `mcopy`, because the failure happens one step earlier (no `T3_LOG`).
A constant verdict is not a falsifiable gate, so this clause is not merely unverified — it is
unverifiable until the head contains the contract.

**Clause 6 — native members in the same wave**: the wave aborts at the `T3_LOG` check before the
guest runs, so there is nothing to observe.

## Suites (clause 7, the rest)

```
groom_test            EXIT=0
check_stale_test      EXIT=0
drift_check_test      EXIT=0
t4_w0.sh --self-test  EXIT=0
merge_gate_test.sh    EXIT=0
make t4_t3log_selftest  EXIT=2  (No rule to make target)
```

## Verdict

**RED sha=020f6a63** — clause 4 unsatisfiable and clause 7's target absent, both because the head
is three commits behind `main` and does not contain GB-140 (`74a6adf`). Clauses 1–3 pass and the
gate's own logic looks right.

Required next step is mechanical, not a redesign: **rebase `020f6a63` onto `main` (≥ 74a6adf)** so
`t3_tier.sh` prints `T3_LOG=` and `make t4_t3log_selftest` exists, then re-run this card at the
new sha. I would expect clause 4 to pass on that tree — but that is a prediction, not a verdict,
and it must be re-graded, not carried forward.