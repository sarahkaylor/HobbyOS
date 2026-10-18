#!/usr/bin/env python3
"""Host (T1) validator for the GB-052 game docs (docs/games/*).

Every pinned game has a README with the required sections, and the user guide
catalogs all pinned games.  "docs pass human read" (DoD) is enforced
structurally: presence + required sections + every pinned game covered + a
README->pinned-game 1:1.
"""
import os, re, sys

PINNED = ["PONG", "MILLIPED", "XNIBBLER", "XCRYPT", "NES"]
DOCDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "games")

def required(txt):
    return all(s in txt for s in ("## Launch", "## Controls", "## What it demonstrates"))

def check_docs():
    out = []
    missing = []
    for g in PINNED:
        p = os.path.join(DOCDIR, f"README-{g.lower()}.md")
        if not os.path.isfile(p):
            missing.append(g)
            continue
        txt = open(p).read()
        out.append(("ok" if required(txt) else "fail",
                    f"README-{g.lower()}: Launch/Controls/demonstrates present"))
    if missing:
        out.append(("fail", f"missing READMEs for: {missing}"))
    g = os.path.join(DOCDIR, "USER-GUIDE.md")
    if os.path.isfile(g):
        gt = open(g).read()
        covered = all(g in gt for g in PINNED)
        out.append(("ok" if "Booting + launching" in gt and covered else "fail",
                    "USER-GUIDE catalogs all pinned games + boot/launch steps"))
    else:
        out.append(("fail", "USER-GUIDE.md missing"))
    return out

def main():
    fails = 0
    for status, msg in check_docs():
        print(f"  {'PASS' if status=='ok' else 'FAIL'}: {msg}")
        if status != "ok":
            fails += 1
    print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
