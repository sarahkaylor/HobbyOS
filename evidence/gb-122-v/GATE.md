# GB-122-V — gate of PR #247 @ c08913f (verdict: RED)

Graded tree: `c08913ffa4fa117da50bf80c12769adcf8d50f2` (PR #247 head), worktree
`/home/sarah/Documents/GitHub/HobbyOS/.worktrees/verify-gb122v`, host `dgx1`, x86_64 + `/dev/kvm`.
Parent: #8c30226 (`origin/main`).

Files in this directory:

    build-intel.log     make ARCH=intel   (rc=0)
    build-arm.log       make ARCH=arm     (rc=0)
    host_tests.log      make host_tests   (32/33)
    process.c.diff      git diff 8c30226 c08913f -- src/kernel/process.c
    makefile.diff       git diff 8c30226 c08913f -- Makefile
    t3-intel-wave.log   one intel T3 wave at the graded head (17062 lines)
    t3-intel-wave.out   the tier's own verdict on that wave

## Clause 1 — reproduce the gate on the archived serials

Archived serials are in commit `c68d257` (`evidence/gb-122/`). The card's quoted triples
(10/9, 17/18, 6/8) do **not** reproduce on any archived serial; the numbers I measure on the four
"unmodified 215b79b" baseline waves that FIVE-WAVES itself names are:

    baseline 24-scoped-wave.log    resume=20 unattributable=6  total_vector14=26  FAIL
    baseline 25-scoped-wave.log    resume=4  unattributable=13 total_vector14=17  FAIL
    baseline 71-forkfix-wave.log   resume=3  unattributable=7  total_vector14=10  FAIL
    baseline 80-sampler-wave.log   resume=10 unattributable=11 total_vector14=21  FAIL

Their totals (26, 17, 10, 21) are exactly FIVE-WAVES' stated baseline distribution
("Vector: 14 = 10, 17, 21, 26 across four waves"), so these are the right files; the *split* the
card quotes is wrong for this detector revision (the gate script is byte-identical between
`b31216c` and the graded head — checked with `diff`, empty).

Patched serials:

    91-bothfix-wave.log    resume=0 unattributable=0 total_vector14=0  PASS
    92-bothfix-wave.log    resume=0 unattributable=1 total_vector14=1  FAIL (unattributable)
    A1-pagewatch-wave.log  resume=0 unattributable=0 total_vector14=0  PASS
    C4-mask-wave.log       resume=0 unattributable=0 total_vector14=0  PASS
    C5-mask-wave.log       resume=0 unattributable=0 total_vector14=0  PASS

So the direction of the card's claim holds (baseline FAIL, 4/5 patched PASS with wave 92
unattributable), but the quoted counts do not.

## Clause 2 — self-test can fail

    $ bash tools/gate/t3_resume_page.sh --self-test
    self-test clean:        resume=0 unattributable=0 total_vector14=0
    self-test violation:    resume=1 unattributable=0 total_vector14=1
    self-test shredded:     resume=0 unattributable=1 total_vector14=1
    t3_resume_page: SELF-TEST PASS — clean=0, resume-page fault identified, shredded counted as unattributable, absent log is not a pass
    SELFTEST_RC=0

Broken deliberately (copy of the script with the shredded-fixture expectation flipped from
`unattributable=1` to `unattributable=0`):

    self-test clean:        resume=0 unattributable=0 total_vector14=0
    self-test violation:    resume=1 unattributable=0 total_vector14=1
    self-test shredded:     resume=0 unattributable=1 total_vector14=1
    t3_resume_page: SELF-TEST FAIL — the detector does not classify its own fixtures
    BROKEN_SELFTEST_RC=1

## Clause 3 — what the diff actually changes (FAIL)

`process.c` is the resume-page commit (`gb122_commit_resume()` + two call sites) **and** it deletes
the GB-071 diagnostics `[GB071-CREATE]` and `[GB071-FORK]` (`process.c.diff`, hunks at
`process_create_internal_ver()` and `process_fork()`). No test greps them and GB-071 is closed, so
that removal is not a build break — it is still a change GB-122's subject does not mention.

The `Makefile` change is **not** only the new target (`makefile.diff`):

    -ARCH ?= intel
    +ARCH ?= arm

    -  QEMU_CMD = ... -M q35,vmport=on -smp ...
    +  QEMU_CMD = ... -M q35 -smp ...

    -        ifneq ($(ARCH),intel)
    -          GX_GPU_GL := 1
    -        endif
    +        GX_GPU_GL := 1

    -	    echo "  => x64 verdict: gtk,gl=on starts — but auto stays soft on x64 ... "
    +	    echo "  => x64 verdict: GL OK — auto picks virtio-gpu-gl-pci + -vga none + gtk,gl=on ..."

Three separate regressions relative to `origin/main`, each reverting a landed fix:

* `bd7fe87` "Makefile: make vmport=on explicit on the intel QEMU line" — removed (2 occurrences on
  main, 0 on the head), so the intel desktop loses the vmmouse/absolute-pointer path.
* `eb45a60` "gx: x64 `make run` defaults to the soft display (gtk,gl=on blanks the window here)" —
  its exact `ifneq ($(ARCH),intel)` guard is reverted, and the `gpu-check` message that documented
  the failure is rewritten to claim the opposite.
* the default architecture for a bare `make` flips intel → arm.

Per the card's own instruction this is a RED finding, not a green line.

## Clause 4 — target + tier untouched

    $ make t3_resume_page_selftest
    ... SELF-TEST PASS ...
    MAKE_TARGET_RC=0

`git diff --name-only 8c30226 c08913f` → `Makefile`, `src/kernel/process.c`,
`tools/gate/t3_resume_page.sh` — `tools/t3_tier.sh` is not modified.

## Clause 5 — builds and host tier

    make ARCH=intel -j8   → intel_rc=0
    make ARCH=arm   -j8   → arm_rc=0

    [host-tests] done: 32/33 passed
      FAIL  tools/board/merge_pr_test.py  (merge_pr: error: unrecognized arguments: --head deadbeef)

The single failure is not this PR's: `tools/board/merge_pr.py`, `merge_pr.sh` and `merge_pr_test.py`
are blob-identical between `8c30226` and `c08913f` (`b2f4d24`, `4e0b077`, `8ec645c`), and the PR
touches none of them. `host_tests` exercises neither the resume path nor the new commit — it is a
T1 build/registry tier, and its green says nothing about GB-122.

## Clause 6 — one intel wave at the graded head (FAIL)

    T3_intel: 17062 console lines, 333 FAIL lines, 14 user memory fault records across 11 processes, 88 programs exited, halted=1
    T3_intel: FAIL — 333 FAIL line(s)
    T3_intel EXIT=1

    $ bash tools/gate/t3_resume_page.sh evidence/gb-122-v/t3-intel-wave.log
    t3_resume_page: resume=0 unattributable=1 total_vector14=1 in 'evidence/gb-122-v/t3-intel-wave.log'
    t3_resume_page: FAIL — resume-copy #PF invariant violated (or unconfirmable: see unattributable count)

Halted, 88 programs exited (floor 85 met). 333 FAIL lines is inside this host's observed band
(archived waves: 83, 85, 86, 132, 133, 135, 186, 372), so I do not attribute it to the PR without a
same-host baseline run — but the gate's own reading of this wave is FAIL: one `Vector: 14` that the
detector cannot attribute to a resume copy.

## Verdict

RED. Clause 3 fails (the Makefile carries three reversions of landed fixes plus a default-ARCH
flip), clause 6 fails (`resume=0 unattributable=1` → gate FAIL on the wave's own serial), and
clause 1's quoted counts do not reproduce. Clauses 2, 4 and 5 pass.