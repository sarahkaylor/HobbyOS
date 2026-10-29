# GB-126-V — verdict: **GREEN** at `a7e5ce6` (PR #245)

Graded sha: **`a7e5ce6`** ("abi(GB-126): use EXCEPTION_ERR for #CP/#VC/#SX (vectors 21/29/30); add a
table-driven host scan"), PR #245, against `origin/main` `f2cdf5c`. Patch under test is
`e057da7..a7e5ce6`: `src/kernel/arch/x64/trap.c` (2 lines), `Makefile` (hook), `tools/gate/vector_shape_scan.py`
(new). farm-3. Host-side only — no QEMU performed, and **no CI result is cited or used**.

Disclosed: the latent skew this fixes is the one I documented in GB-125 (#274). I reported it; the patch is
GB-126's. I held the DoD's own falsifier to the letter, including running the discriminator against a pre-fix
tree rather than trusting the fix's own claim.

| # | Clause | Result |
|---|---|---|
| 1 | 21/29/30 are `EXCEPTION_ERR`; no `NO_ERR` remains; any sentinel named | **PASS** — `grep -nE "EXCEPTION_NO_ERR\((21\|29\|30)\)"` empty; ERR lines quoted below |
| 2 | binary proof: no `push 0` before the vector push; unchanged neighbours still `push $0x0` | **PASS** — `push $0x15` / `$0x1d` / `$0x1e`; `exception_22` still `push $0x0` first |
| 3 | `make host_tests` green and the scan is a failing-capable discriminator | **PASS** — `53 checks, 0 FAIL` in-gate; the same script on the pre-fix tree gives `53 checks, 6 FAIL`, exit 1 |
| 4 | sha named, x64-only, host failures identical on main | **PASS** — `a7e5ce6`; the change is `arch/x64` + a host-side scanner; no host failures at all |

## 1 — source

```
$ grep -nE "EXCEPTION_NO_ERR\((21|29|30)\)" src/kernel/arch/x64/trap.c
(no output; exit 1)

$ grep -nE "EXCEPTION_ERR\((21|29|30)\)" src/kernel/arch/x64/trap.c
3340:EXCEPTION_NO_ERR(20);EXCEPTION_ERR(21);   EXCEPTION_NO_ERR(22);EXCEPTION_NO_ERR(23);
3342:EXCEPTION_NO_ERR(28);EXCEPTION_ERR(29);   EXCEPTION_ERR(30);   EXCEPTION_NO_ERR(31);
```

No sentinel is used; the fix relies on the architectural fact that `#CP`/`#VC`/`#SX` always push an error code
(Intel SDM Vol. 3A/3B), which is the same table the new scanner encodes.

## 2 — the binary, which is the part that can fail silently

`objdump -d obj/intel/trap.o`:

```
00000000000000af <exception_21>:
      af: 6a 15        push   $0x15
00000000000000f5 <exception_29>:
      f5: 6a 1d        push   $0x1d
      f7: e9 b8 00 00 00  jmp  1b4 <common_trap_wrapper>
00000000000000fc <exception_30>:
      fc: 6a 1e        push   $0x1e
      fe: e9 b1 00 00 00  jmp  1b4 <common_trap_wrapper>
00000000000000b6 <exception_22>:          <- unchanged NO_ERR neighbour
      b6: 6a 00        push   $0x0
      b8: 6a 16        push   $0x16
      ba: e9 f5 00 00 00  jmp  1b4 <common_trap_wrapper>
```

Three stubs lost their `push 0` and the neighbouring `NO_ERR` kept it. This is the check the card asks for, and it
is the one that would catch a source-level "fix" that never reached the stub (the GB-101-V5 class).

## 3 — the discriminator can fail

In-gate, at the graded head:

```
  PASS vector 129: NO_ERR matches architecture
  PASS vector 130: NO_ERR matches architecture
vector_shape_scan: 53 checks, 0 FAIL
```
```
Checks run:    482
Checks failed: 0
TEST EXIT: 0        HOST_RC=0
```

Same script, same day, run against a **pre-fix** tree (`.worktrees/verify-gb101v4`, `d50be85`, whose dispatcher
still says `EXCEPTION_NO_ERR(21)`):

```
  FAIL vector 21: GB-126 requires EXCEPTION_ERR
  FAIL vector 29: GB-126 requires EXCEPTION_ERR
  FAIL vector 30: GB-126 requires EXCEPTION_ERR
vector_shape_scan: 53 checks, 6 FAIL          (exit 1)
```

So it fails for the right reason on the right vectors, not because it cannot run.

## 4 — scope

`e057da7..a7e5ce6` touches only `src/kernel/arch/x64/trap.c`, the `Makefile` hook and the new host tool — no arm
change, which matches a vector table that exists only in the x64 dispatcher. I diffed from `e057da7` (the merged
GB-124 head) deliberately: the branch also carries GB-124's `[GB124-AS]` instrumentation in `vm.c`, and that
belongs to #244, already merged — it is not part of this patch. Worth saying out loud because a naive
`git diff main` on this branch would look like scope creep and is not.

## Reproduce

```
cd .worktrees/verify-gb126v            # a7e5ce6
grep -nE "EXCEPTION_NO_ERR\((21|29|30)\)" src/kernel/arch/x64/trap.c   # empty
make obj/intel/trap.o ARCH=intel && objdump -d obj/intel/trap.o --disassemble=exception_21
make host_tests                        # 482/0, vector_shape_scan 53/0
python3 tools/gate/vector_shape_scan.py   # in a pre-fix tree: 53 checks, 6 FAIL
```