**GREEN — GB-119-V, graded at `b9e8647c5dae9d1aa3cd4629cf3c48b3d6735d34` (PR #238, branch `ticket/GB-119-menu-cap`). DEEP verifier `wgfx-verify`; every run local, no CI cited, used, or gated on.**

The cap fix is real and it is proven the way the card wants — a **new** name, staged after the build, listed by the guest on **both** arches, with the pre-fix truncation reproduced on the pre-fix head. One clause needs a caveat before it is read: **the rig at this head has no `--stage`**, so clause 1's "the rig must not FATAL" half could not be exercised with the named instrument. I measured the same property with the tier's own mechanism (stage after `disk.img`, before QEMU) and say so rather than borrowing the rig's name for it.

Evidence: `evidence/gb-119-v/` (both arches' staged and unstaged serials, the pre-fix head's truncated serial, host_tests log + rc, the case-9 bail, the PR diff).

## 1. A NEW name is provable end to end — MET by the tier's mechanism (see caveat)

Fixture `RIGFIX.BIN` (a copy of `APPS_T.BIN`, `sha256 61c835ded091b889c7d162b19e27c175fa3ef572eab1d42092eb75026eeca436`), staged onto the built image and the guest booted directly with the same QEMU command `make run` uses:

```
$ mdir -i disk.img ::/ | grep -i rigfix        # on the image, after the build
RIGFIX   BIN    590104 2026-10-28  10:49
$ qemu-system-x86_64 … -serial file:…          # intel, PR head
[MENU] count=177
[MENU] 176=RIGFIX.BIN
```
Caveat, stated plainly: `python3 tools/evidence_rig.py --stage …` at this head prints `error: unrecognized arguments: --stage`, and `grep -c stage tools/evidence_rig.py` is **0** — the rig's staging is GB-118's work (PR #231), which is **not merged** and which I RED'd this morning (`$ROOT` expansion). So "the rig must not FATAL" is unmeasurable here; what is measurable is the property the clause is about, and it passes. If the Coordinator wants the rig-shaped proof, it has to land GB-118 first.

**The pre-fix head reproduces the failure**, which is what makes the above a proof rather than a coincidence:
```
$ # worktree at 32cbbdb (MAX_MENU_ITEMS 128), same fixture staged the same way
[MENU] count=128
rigfix-lines=0
```

## 2. The proof can fail — MET

Without the fixture on the image, the same instrument reports it absent, on both arches:
```
intel: [MENU] count=176   RIGFIX lines: 0
arm:   [MENU] count=175   RIGFIX lines: 0
```

## 3. The truncated case specifically — MET

On the arm image the staged name lands at **index 175** — the ≥175 the card asked for:
```
arm, staged:  [MENU] count=176
              [MENU] 175=RIGFIX.BIN
```
The intel root count **as I measured it** is **176** (not the 158 in the card): `[MENU] count=176` before staging, 177 after. The card's 158 was presumably taken on a different image composition; the arm figure (175 entries before staging, vs 176 in the card) is likewise within one entry.

## 4. The bound is real, both ways — MET

* Dump stays bounded: `[MENU] count=177` on intel (≤256), and case 9's second assertion `menu dump stays bounded (no unbounded growth / no truncation lie)` passes.
* No regression in the host suite at the graded head: `rc=0`, `Checks run: 482`, `Checks failed: 0`, `ALL APPS SUITE TESTS PASSED`, `TEST EXIT: 0`, with `[TEST] desktop menu: 23 checks, 0 failures`.

## 5. Case 9 executes and can fail — MET, re-run by me

Inside `make host_tests` at the head:
```
1056:[TEST] desktop menu: 23 checks, 0 failures
1054:… PASS: a root name at index 175 is listed (256 cap covers the 176-entry root; old 128 dropped it)
```
Then in a throwaway worktree at the same head with `MAX_MENU_ITEMS` bailed back to **128** (`src/user/desktop.c:38`), the target `desktop_menu_test_host` builds (`rc=0`) and case 9 **fails**:
```
  FAIL: a root name at index 175 is listed (256 cap covers the 176-entry root; old 128 dropped it)
  PASS: menu dump stays bounded (no unbounded growth / no truncation lie)
[TEST] desktop menu: 23 checks, 1 failures        (rc=1)
```
So the test is not decorative: the same bail that restores the old cap turns it red.

One note from that bail, for whoever writes the next one: the test declares `extern char menu_items[256][16]` while the bailed `desktop.c` defines `[128][16]`, so the run also prints a long run of digits as it walks past the smaller array — the FAIL is unambiguous, but the harness confirms the array-size coupling is one-directional and silent otherwise.

## 6. Both arches — MET

Clauses 1-3 were run on both, and the two images behave as the fix intends:

| arch | root before | root after staging | staged name listed |
|---|---|---|---|
| intel | 176 | 177 | `[MENU] 176=RIGFIX.BIN` |
| arm | 175 | 176 | `[MENU] 175=RIGFIX.BIN` |

## Verdict

GREEN. The defect GB-119 names — "a NEW staged fixture is not provable because the listing truncates" — is fixed and demonstrated with a falsifier on both arches. GB-119 (#259) → **Merge Gate**; GB-119-V (#260) → **Done**.

The one thing this GREEN does **not** certify: that the **rig** can stage. That is GB-118 (PR #231), still In Progress, and its `--stage` is absent from this branch. Landing GB-119 makes the guest's listing able to show a 176th name; landing GB-118 is what lets the rig put one there.