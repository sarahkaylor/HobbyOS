# GB-059-V — verification evidence (DEEP Verifier, 2026-10-23)

Subject: GB-059 (#61) "one rig runs a native and a compat title" — **NO VERDICT: the native half passes,
the compat half is unmeasurable on this tree.** Card returned to Backlog+blocked; no GREEN and no RED.
Revision: `b3082f8d0091905114ab2bd3eea6499391e67450` = `origin/main`, fresh worktree.

| # | file | what it is |
|---|---|---|
| 01 | `01-native-by-name.log` | `evidence_rig.py --game XNIBBLER --arch arm` → PASS, launched by name |
| 02 | `02-first-attempt-qmp-timeout.log` | first attempt: QMP greeting timeout behind a leftover guest (environment, retried) |
| 03 | `03-arm-build.log` | `make ARCH=arm disk.img` → exit 0 (after §11.5 staging was symlinked) |
| 04 | `04-static-checks.txt` | image/rig/git greps quoted in the report |

## Native half — passes, by name, with a real frame
```
[rig] booting arm QEMU, capturing to evidence/XNIBBLER/20261023T045745Z
[rig] XNIBBLER.BIN slot: found by name = 19 (launching 19, by name)
[rig] evidence bundle: evidence/XNIBBLER/20261023T045745Z
NATIVE2_RC=0
```
No `--slot` was supplied. `find_menu_slot` is the launch value (`tools/evidence_rig.py:159, 231`).

## Compat half — nothing to launch, and the rig has no compat reference
```
$ mdir -i disk.img ::/ | grep -iE '\.ELF'      → (none; the only 'elf' substring is SELFHOST.BIN)
$ grep -ciE 'compat|\.elf|linux' tools/evidence_rig.py → 0
```
There is no compat title on the image. The card's own 2026-10-22 correction names the blocker precisely:
a compat title that **renders**, which is GB-041 (Doom as static-musl ELF), and GB-041 is `Ready` +
`blocked` on the M1/M2 wall (its 2026-10-21 comment). So the only missing piece is the title, not a
launcher path — the rig launches any `[MENU]` name the same way.

## Static clauses — pass
```
$ git ls-files | grep -cE 'run_(doom|stardust|xnibbler|xcrypt)_test\.py'   → 0   (none tracked)
$ grep -cE 'slot\s*=\s*[0-9]+|index\s*=\s*[0-9]+' tools/evidence_rig.py    → 0   (no magic index)
```

## Why no verdict
DoD clause 1 says **both** titles. Certifying GREEN after running one would be the fabricated pass this
program keeps paying for; RED would blame GB-059 for a dependency it does not own (GB-041 / the M1 wall),
and the previous pass on this same card reached the same conclusion at `79d14e7`. Re-measured at
`b3082f8`, nothing about the compat blocker has changed since.

**Process note for the plan:** PLAN-010 re-armed this card on GB-072/GB-073/GB-076 ("the chain is
landed"). Those unblock the T4 *probe* cards (#87/#88/#89); they do not produce a compat title, which is
what this card's own 2026-10-22 correction says it needs. The plan and the card disagree, and the card is
right.

**Environment note, same trap as §11.5:** a cold worktree without `third_party/llvm-21.1.8/dist-staging`
starts a *hours-long* LLVM build inside `make disk.img` (I caught it at 2613/2613 targets and killed it).
Symlink the primary tree's staging before the first `make`, and pre-build `disk.img` before invoking the
rig or the ~100 s QMP wait races the build.
