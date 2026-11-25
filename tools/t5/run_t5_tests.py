#!/usr/bin/env python3
"""run_t5_tests.py — self-registering host-test suite (GB-064).

Auto-discovers and runs every python host test under tools/t5/ plus the two
tools-root T5 tests, so **adding a host test needs no edit to the shared
Makefile host_tests line** (drop a tools/t5/<name>_test.py and it just runs).
Two branches that each add a distinct _test.py file therefore merge with no
conflict — this removes the host_tests recipe-line collision the swarm saw
(PR #27 replacing #29's lines).

**The verdict is the test's exit status — never a substring of its stdout.**
Until 2026-10-23 this grader was `ok = "PASSED" in output`: two scripts that
exited 0 and printed their own verdict in a different capitalisation
(`CLAIM_RACE_TEST: PASS — …`, `ACCEPTANCE_GATE_TEST: PASS — …`) were graded
FAIL and reddened the whole tier, while a script that crashed *after* printing
"ALL PASSED" would have been graded PASS. A substring match answers a question
about the wording of the log, not about what the test proved.

A script still has to *state* a verdict, so an exit-0 script that silently
prints nothing cannot count as a pass (the "a skip that prints a pass" class,
GB-007). Both directions are proven by `--self-test`, which must be run with
`--self-test` in CI/host_tests: it fails if the grader cannot grade a known-bad
script as bad.

**Three verdicts, not two (2026-11-24).** A host test can have a prerequisite
this tree cannot supply — the live plan doc behind two of the board/gaming
tests was deleted with the decommissioned swarm (c3f78668) — and the honest
answer for that is neither a pass nor a red. A script says so by printing a
`SKIP: <reason>` line and exiting 77 (automake's "skipped" status). The suite
prints SKIP rows with their reasons, counts them apart from passes, and does
not let them hide a failure. Exit 77 *without* a reason, or a `SKIP:` line on a
zero exit, is still graded FAIL: skipping something quietly is the GB-007 bug
in a new costume.

**A failing script's output is shown.** Until 2026-11-24 the suite printed only
the last line of a failing script, so `FileNotFoundError: 'gaming.md'` reached
the CI log with no file, line or stack — main's red linux-ci took a day to name
for that reason alone. Every FAIL now prints the tail of that script's own
captured output.

Exit code: number of failures (0 = all pass).
"""
import glob, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root (tools/t5 -> root)

# A script counts as having stated a verdict if it prints one of these. Both
# spellings of the program's own convention are accepted: "ALL PASSED" /
# "…: PASS" — the point is that the *script* asserted, not the grader.
PASS_MARKERS = ("ALL PASSED", ": PASS", "PASSED")

# The skip convention: exit 77 + a line naming what was missing. 77 is what
# automake's test harness calls "skipped", so it reads right under both runners.
SKIP_RC = 77
SKIP_MARKER = "SKIP:"

# How much of a failing script's output to show (its traceback is at the end).
FAIL_OUTPUT_LINES = 25


# Scripts that match the *_test.py glob but are QEMU harnesses with their own
# make targets, not host unit tests (see discover()).
EXCLUDED = {
    "tools/doom_elf/run_doom_elf_test.py",
    # GB-176 again (H4-kglitch): the RimWorld in-colony gameplay battery boots a
    # guest and drives the game over QMP; it needs the staged game and a slot.
    "tools/rw/rw_gameplay_test.py",
}


def discover(root=None):
    """Ordered, deduped set of python test scripts to execute."""
    root = root or ROOT
    scripts = [os.path.relpath(p, root)
               for p in glob.glob(os.path.join(root, "tools", "*", "*_test.py"))]
    # tools-root T5 tests (stable, low churn); NOT run_selfhost_build_test.py
    # (a heavy standalone selfhost run, deliberately outside host_tests).
    scripts += ["tools/evidence_rig_test.py", "tools/perf_game_cycle_test.py"]
    # GB-176: run_doom_elf_test.py matches the *_test.py glob but is not a unit
    # test — it boots QEMU with a staged disk and grades screendumps, and it
    # needs obj/doom-linux/doom10.elf, which is gitignored. Discovered on a
    # fresh tree it fails at its first line ("no ELF at …"), turning T1 red for
    # every clean machine; discovered on a box that has the artifact it boots a
    # VM inside `make host_tests`. Both are wrong: the harness has its own
    # target (`make doom_elf_test`). Same reasoning as run_selfhost_build_test.py.
    scripts = [s for s in scripts if s not in EXCLUDED]
    seen, out = set(), []
    for s in scripts:
        if s not in seen:
            seen.add(s)
            out.append(s)
    return sorted(out)


def run_one(path, cwd):
    """Grade one script: returns (verdict, last_line, captured_output).

    verdict is "PASS", "SKIP" or "FAIL":
      PASS  exit 0 and the script stated a verdict
      SKIP  exit 77 *and* the script said what was missing (`SKIP: …`)
      FAIL  everything else — including exit 77 with no reason, and exit 0 with
            no stated verdict (a silent skip is not a pass)
    """
    r = subprocess.run([sys.executable, path], cwd=cwd, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    stated = any(m in out for m in PASS_MARKERS)
    skip_stated = SKIP_MARKER in out
    lines = out.strip().splitlines()

    if r.returncode == SKIP_RC:
        verdict = "SKIP" if skip_stated else "FAIL"
    elif r.returncode == 0 and stated and not skip_stated:
        verdict = "PASS"
    else:
        verdict = "FAIL"

    last = lines[-1] if lines else ""
    if verdict == "SKIP":
        why = next((l.strip() for l in lines if SKIP_MARKER in l), "")
        last = why.split(SKIP_MARKER, 1)[1].strip() or why
    elif r.returncode == SKIP_RC:
        last = f"exit {SKIP_RC} without a 'SKIP: …' line — a silent skip is not a skip"
    elif r.returncode == 0 and not stated:
        last = "exit 0 but printed no PASS verdict — a silent pass is not a pass"
    return verdict, last, out


def show_output(out, limit=FAIL_OUTPUT_LINES):
    """Print the tail of a failing script's output, so its traceback reaches the log."""
    lines = out.rstrip().splitlines()
    if not lines:
        return
    if len(lines) > limit:
        print(f"      … {len(lines) - limit} earlier line(s) omitted")
    for line in lines[-limit:]:
        print("      | " + line)


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def self_test():
    """Prove the grader can return every verdict. A grader that cannot fail is not one.

    Returns (failures, cases); main() counts `cases` so the suite total cannot
    drift from the number of self-test rows.
    """
    d = tempfile.mkdtemp()
    cases = [
        # (name, body, expected verdict, why)
        ("good.py", "print('ALL PASSED')\nraise SystemExit(0)\n", "PASS",
         "exit 0 + stated verdict"),
        ("lowercase.py", "print('SOME_TEST: PASS — fine')\nraise SystemExit(0)\n", "PASS",
         "exit 0 + ': PASS' spelling (the case that reddened the tier)"),
        ("lies.py", "print('ALL PASSED')\nraise SystemExit(1)\n", "FAIL",
         "prints PASSED but exits 1 — the substring-only grader called this a pass"),
        ("silent.py", "raise SystemExit(0)\n", "FAIL",
         "exit 0 with no verdict statement — a silent skip is not a pass"),
        ("crash.py", "raise SystemExit(3)\n", "FAIL", "non-zero exit"),
        ("skip.py", "print('SKIP: no fixture for this host')\nraise SystemExit(77)\n", "SKIP",
         "exit 77 + a named reason — counted apart from a pass"),
        ("fakeskip.py", "raise SystemExit(77)\n", "FAIL",
         "exit 77 with no reason — a skip that says nothing is not a skip"),
        ("mixed.py", "print('ALL PASSED')\nprint('SKIP: one fixture')\nraise SystemExit(0)\n",
         "FAIL", "claims both a pass and a skip on exit 0 — the verdict must be one thing"),
    ]
    fails = 0
    for name, body, want, why in cases:
        p = os.path.join(d, name)
        write(p, body)
        verdict, _last, _out = run_one(p, d)
        good = (verdict == want)
        print(f"  {'PASS' if good else 'FAIL'}: {name} ({why}) -> grader said {verdict}, wanted {want}")
        fails += 0 if good else 1

    # A failing script's own words must reach the caller: that is how a
    # traceback gets into the CI log (2026-11-24, main's red linux-ci).
    p = os.path.join(d, "traceback.py")
    write(p, "def boom():\n    return 1 / 0\nboom()\n")
    verdict, _last, out = run_one(p, d)
    good = verdict == "FAIL" and "ZeroDivisionError" in out and "traceback.py" in out
    print(f"  {'PASS' if good else 'FAIL'}: a failing script's traceback is captured, "
          f"not cut to one line -> {'kept' if good else 'LOST'}")
    fails += 0 if good else 1

    # The QEMU harnesses that match the glob are never discovered (GB-176).
    t = tempfile.mkdtemp()
    harnesses = ["tools/doom_elf/run_doom_elf_test.py", "tools/rw/rw_gameplay_test.py"]
    for rel in harnesses + ["tools/x/plain_test.py"]:
        os.makedirs(os.path.join(t, os.path.dirname(rel)), exist_ok=True)
        write(os.path.join(t, rel), "print('ALL PASSED')\n")
    found = [s for s in discover(t) if s.startswith("tools/") and s.count("/") == 2]
    good = found == ["tools/x/plain_test.py"]
    print(f"  {'PASS' if good else 'FAIL'}: QEMU harnesses excluded from discovery -> {found}")
    fails += 0 if good else 1
    print("RUN_T5_TESTS_SELF_TEST:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
    return fails, len(cases) + 2          # + the traceback row + the discovery row


def main():
    if "--self-test" in sys.argv:
        fails, _cases = self_test()
        return fails
    cwd = ROOT
    scripts = discover()
    print(f"[host-tests] self-registering suite: {len(scripts)} python tests")
    grader_fails, grader_cases = self_test()    # the grader proves it can fail, every run
    fails = grader_fails
    skipped = []
    for s in scripts:
        verdict, last, out = run_one(s, cwd)
        if verdict == "FAIL":
            fails += 1
        elif verdict == "SKIP":
            skipped.append((s, last))
        print(f"  {verdict}  {s}" + (f"  ({last})" if verdict != "PASS" else ""))
        if verdict == "FAIL":
            show_output(out)
    total = len(scripts) + grader_cases
    passed = total - fails - len(skipped)
    print(f"[host-tests] done: {passed}/{total} passed, {fails} failed, {len(skipped)} skipped")
    if skipped:
        print("[host-tests] skipped — each names what this tree cannot supply "
              "(not a pass, not a failure):")
        for s, why in skipped:
            print(f"  SKIP  {s}  ({why})")
    # The exit status is the failure count (0 = green).  Cap it: a shell exit
    # status is one byte, so 256 failures would wrap to a green 0.
    return min(fails, 255)


if __name__ == "__main__":
    sys.exit(main())
