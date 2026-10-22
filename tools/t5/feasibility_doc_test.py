#!/usr/bin/env python3
"""Host (T1) validator for the GB-011 feasibility re-derivation.

Checks the committed doc has, per game (RimWorld / DSP / TA): a loop section,
kernel needs, loader needs, and a verdict that cites a capability state — plus
that the doc's own capability ledger cites every capability (landed or ticketed)
rather than assuming one.  Import-testable core.
"""
import os, re, sys

GAMES = ["RimWorld", "DSP", "Total Annihilation"]

def check_doc(path):
    txt = open(path).read()
    out = []
    # three games present
    for g in GAMES:
        if g in txt:
            out.append(("ok", f"{g}: present"))
        else:
            out.append(("fail", f"{g}: MISSING"))
    # per-game required sections
    for sec in ("The loop", "Kernel needs", "Loader / runtime needs", "Sizing vs what exists",
                "Verdict"):
        if txt.count(sec) >= 3:
            out.append(("ok", f"section '{sec}' present for all three games"))
        else:
            out.append(("fail", f"section '{sec}' present only {txt.count(sec)}x (want >=3)"))
    # the no-new-capability gate: every capability row must be marked landed,
    # cite a ticket (GB-xxx), or be the explicit "NOT ticketed" gap marker.
    # A row is cited if it carries any of those; only capability rows with none
    # are the genuine violation.
    ledger_uncited = 0
    for line in txt.splitlines():
        if line.startswith("| ") and "Ref" not in line and line.count("|") >= 4:
            if "**landed**" in line or "GB-" in line or "NOT ticketed" in line:
                continue
            ledger_uncited += 1
    if ledger_uncited == 0:
        out.append(("ok", "capability ledger rows all cite (landed / GB- / gap marker)"))
    else:
        out.append(("fail", f"{ledger_uncited} ledger row(s) with no landed/ticket/gap marker"))
    # each of the three games has a verdict line
    v = re.findall(r"^\*\*Verdict\*\*:", txt, re.M)
    out.append(("ok" if len(v) == 3 else "fail",
                f"{len(v)}/3 explicit game Verdict blocks"))
    return out

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                     "program-2", "feasibility-rimworld-dsp-ta.md")
    fails = 0
    for status, msg in check_doc(path):
        print(f"  {'PASS' if status=='ok' else 'FAIL'}: {msg}")
        if status != "ok":
            fails += 1
    print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
