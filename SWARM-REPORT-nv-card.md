# GB202 — the boot log prints the PFB census, and A5's gate rejects a fenced ucode read (0 boots)

Card `t_cacffe28`. Branch `nv/gb202-census-gate`, cut from `origin/nv/integ-swarm`
`0269b0ea` (the tip rm68 booted at `eb3bbe69`+Tier A2), then merged with the tip
that landed while this card ran — `63a44fab` (`nv/shim-mock-chan2`), merge
`e1ed3173`. The two tiers below ran **on that merge**, not on the older base:
`git diff --stat 0269b0ea..e1ed3173` is this card's four files plus that lane's
`src/kernel/nv_shim.c`, `docs/nv-plan.md` and report.

**No boot, no VM, no `qm`, no GPU was touched.** The GB202 boot window closed at
2026-11-28T06:00Z; this card exists so the next funded boot returns rows worth
reading. Everything below is a unit-tier measurement or a source assertion.

## 1. What was wrong, in the two logs this card was handed

Both defects are visible from rm67 and rm68 alone, and both were reproducible
before any code was touched:

| measurement | rm67 (`t_240a0ac8/5090-gb202fsp-rm67.log`) | rm68 (`t_97b8a05c/5090-gb202fsp-rm68_1.log`) |
|---|---|---|
| `[NV-GSP] probe wpr2` rows | 2 | **0** |
| `[NV-GSP] no gsp window` | `(fenced): falcon-side words not reachable; the WPR2/MOVES rows above are PFB reads and stand` (line 343) | `: firmware stage not reachable` (line 355) |
| A5 row's version word | — | `fsp_ucode=0x0x00000000BADF4100 … wrote=1` (line 267) |
| `falcons=` | `0x0000000000000000` | `0x0000000000000000` |

`eb3bbe69` had moved the `!d->falcon[NV_FALCON_IDX_GSP].present` guard to the top
of `nv_gsp_probe()`, above the four PFB reads. Since the falcon mask is 0 on this
part on every boot, the guard fired every time and the only rows GB202 can give
were never printed. A5's `ucode >= 0x44C` then compared a 0xBADF_xxxx decoupler
answer as if it were a version, so the gate could never fail.

## 2. What changed (`4fac9d01`)

`src/kernel/nv_gsp.c`

- **PFB census as its own helper.** `nv_gsp_pfb_census_read()` /
  `nv_gsp_pfb_census_print()` hold the four rows (WPR2 lo/hi, readprot,
  span_kib, moves a/b, verdict) with no falcon gate and rm67's wording, byte for
  byte. `nv_gsp_probe()` calls them **before** the presence guard.
- **The no-window row names the reason**: `(not-present)` when the window is not
  on the part's map, `(fenced)` when it is on the map but its control word
  answers `0xBADF5620`. The fenced case returns before the mailbox words are
  read — a decoupler answer is never printed as firmware state — and its
  sentence is rm67's `:343` unchanged, so only the parenthesised reason differs
  from the log a reader will diff against.
- **A5's gate rejects what it cannot trust.** `nv_gsp_ucode_version_trusted()`
  refuses a `0xBADF_xxxx` word (`fence`, via the existing `nv_is_badf()`), zero
  (`unset`) and anything above `NV_FSP_UCODE_VERSION_MAX` = `0xFFFF`
  (`out-of-range`). The write now needs `trusted && ucode >= 0x44C`.
- **The A5 row says which of those happened**: `fsp_ucode=<word>
  ucode_gate=passed|not-passed [ucode_reject=fence|unset|out-of-range] wrote=0|1`.
  The row's `fsp_ucode=0x0x…` double prefix (a pre-existing `uart_puts("=0x")` +
  `uart_print_hex()` defect) is fixed on this field only; `sec=` keeps its double
  `0x` because no card asked for that row.

`src/include/nv_gsp_glue.h` — the census struct, its three verdicts, the
version bound and both helper prototypes, so the unit tier can drive them.

`src/kernel/nv_gsp_glue_test.c` — two new cases plus a third block in the
existing A5 case:

- `test_nv_gsp_pfb_census_survives_an_absent_falcon_window` drives rm67's own
  values through a device with `falcon[GSP].present == 0`, asserts the fields,
  `span_kib == 0`, the `hi-is-zero` verdict, that `nv_gsp_probe()` still returns
  `-ENODEV` and **wrote nothing**, then the other two verdicts; and a second
  device that is `present` but `bcr_ctrl = 0xBADF5620` to prove the `fenced`
  branch and that probe stopped after 6 reads (5 census + BCR) instead of
  printing a fenced word as a mailbox.
- `test_nv_gsp_ucode_gate_names_its_refusals` covers `0x44C` and `0x44B`
  (trusted), `0xBADF4100` → `fence`, `0xFFFFFFFF` → `out-of-range`, `0` →
  `unset`, and a NULL reason pointer.
- the existing `test_nv_gsp_time_write_gated_on_fsp_version` gained the two
  refusal cases and asserts `writes == 0` for both (the card's "a unit case
  drives `0xBADF4100` … and one out-of-range value").

`tools/nv/check_tiera.py` — **64 assertions, 0 failures**. Eight new source
assertions (census printed before the presence guard; both reason strings; the
census wording; `nv_is_badf(raw)`; the exported bounds/struct; the row's
`ucode_gate`/`ucode_reject`/`fsp_ucode` fields; `trusted && ucode >= 0x44C`
gating the write) and five assertions against the committed intel tier log, so
the gate is checked against rows that actually printed, not only source text.
The unit-case lists now name twelve cases.

`evidence/nv/gb202-census-gate-tier-{intel,arm}.log` — the two tier consoles,
committed (`evidence/` is gitignored) so `check_tiera.py` and the quotes below
are reproducible from the tip.

## 3. Acceptance, measured

**1. The census block prints when `falcon.present == 0`, in rm67's wording.**
The new case's console, `evidence/nv/gb202-census-gate-tier-intel.log:2337-2340`:

```
[NV-GSP] probe wpr2 lo=0x000000007FFFFE00 hi=0x0000000000000000 readprot=0x000000000884CB8F (LO/HI are a start/END pair in 4 KiB units)
[NV-GSP] probe wpr2 span_kib=0x0000000000000000 verdict=hi-is-zero (region not established); still: the booter builds it, the host never writes these.
[NV-GSP] probe moves a=0x000000007FFFF000 b=0x0000000000000000 (this pair changes across init; identity unexplained)
[NV-GSP] no gsp window (not-present): the falcon window does not decode on this part, so the falcon-side words are not readable; the WPR2/MOVES rows above are PFB reads and stand
```

Lines 2337-2339 are **byte-identical** to rm67 `:340-342` (checked mechanically,
not read by eye — see `quotecheck.py` in the card's attachments, 0 failures).
The fourth row is rm67's `:343` with the reason renamed, which is what the card
asks for. The fenced variant is byte-identical to rm67 `:343` as well, and its
own console rows are at `:2341-2345`.

**2. A5's row rejects what it cannot trust, and names the raw value.**
Same log, `:2300`, `:2303`, `:2304`:

```
[NV-GSP] gpu-time … fsp_ucode=0x000000000000044C ucode_gate=passed wrote=1
[NV-GSP] gpu-time … fsp_ucode=0x00000000BADF4100 ucode_gate=not-passed ucode_reject=fence wrote=0
[NV-GSP] gpu-time … fsp_ucode=0x00000000FFFFFFFF ucode_gate=not-passed ucode_reject=out-of-range wrote=0
```

A normal value still passes (and `0x44B` — a real but pre-fix version — passes
the trust gate and is still not written, `:2302`, unchanged vendor behaviour).

**3. `tools/nv/check_tiera.py`** — `assertions 64 failures 0` (49 before this
card, measured by running the base commit's copy in a detached worktree at
`0269b0ea`), including the five log-row assertions.

**4. Both tiers green from this tip**

| tier | command | result |
|---|---|---|
| intel | `SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 bash run_unit_tests_intel.sh` | `Tests run: 623`, `Tests failed: 0`, `UNIT TESTS PASSED` (`evidence/nv/gb202-census-gate-tier-intel.log:3220-3222`) |
| ARM | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 bash run_unit_tests.sh` | `Tests run: 486`, `Tests failed: 0`, `UNIT TESTS PASSED` (`evidence/nv/gb202-census-gate-tier-arm.log:2463-2465`) |

Baseline floor was intel ≥ 616 / ARM 486 with 0 failures. Intel gained the two
new cases (623 ≥ 616); ARM is unchanged at 486 **because
`nv_gsp_glue_test.c` is inside `#ifdef __x86_64__`** — the two new cases are
intel-only by construction, and ARM's number is the same suite it ran before.
Both consoles begin `Running unit tests with QEMU_ARGS: `, i.e. TCG (`SKIP_KVM=1`,
`/dev/kvm` is root:kvm 660 and this uid is not in the group).

## 4. What the next GB202 boot should now return (predicted, not measured)

Nothing here proves silicon: no boot was spent. From the unit evidence the next
mode-67 boot's rows change in exactly two ways, and both are the point:

- the `[NV-GSP] probe wpr2 …` / `span_kib …` / `moves …` block reappears at the
  `no gsp window` site, with `(not-present)` as the reason on this part;
- A5's row becomes `fsp_ucode=0x00000000BADF4100 ucode_gate=not-passed
  ucode_reject=fence wrote=0` — i.e. the row will report that it did **not**
  write the GC6 offset, where rm68 reported `wrote=1` on the same word.

The second is a behaviour change on the boot path, not just a log change: on
GB202 the offset writes stop. That is the correct reading of a decoupler answer
at `+0x334`, and it does not touch the wall (`m0`, `statq/linked/initdone`) or
any other Tier A row.

## 5. Left undone, deliberately

- **No doc edits.** `docs/nv-plan.md` is coordinator-owned and untouched.
  `tools/nv/check_tiera.py`'s docstring still cites `docs/gb202-vendor-diff.md`,
  which is not on `nv/integ-swarm` (it is referenced by
  `SWARM-REPORT-GB202-TIERA.md` as the source doc); that provenance question
  belongs to the card that wrote it, not to this one.
- **No re-derivation of any Tier A row** and no re-touch of the wall row.
- The census's own verdict lines still say "the SEC2 booter builds WPR2" — true
  for AD102 and stale for GB202, whose ACR never arms a usable WPR2 span. That
  wording is rm67's by requirement; changing it would break the byte-for-byte
  diff this card exists to restore. It is a candidate for the next
  census-wording card, not for this one.
