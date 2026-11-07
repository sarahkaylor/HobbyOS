#!/usr/bin/env python3
"""Summarise one RimWorld desktop run: window churn, presents, the retry
signature.  Reads the harness's Player.log and serial-desktop.log.

Usage: tools/rw/l40-summary.py <Player.log> [serial.log]
Every number printed here is counted from the files, never typed.
"""
import re
import sys
from collections import Counter

CALL = re.compile(r"\[XC\] (X[A-Za-z0-9_]+)")
EVT = re.compile(r"\[XC\] (?:XIfEvent|XCheckIfEvent) matched type=(\d+)(?: win=(\d+))?")
CREATE = re.compile(r"\[XC\] XCreateWindow id=(\d+)")
GEOM = re.compile(r"\[XC\]   parent=(\d+) geom=([0-9x]+)")


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("latin1").splitlines()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    pj = read(sys.argv[1])
    ser = read(sys.argv[2]) if len(sys.argv) > 2 else []
    # The shim writes rows to fd 2; before Unity's -logFile dup2 they land in
    # the serial log, after it in Player.log.  One ordered-ish view: both.
    rows = ser + pj

    print("== ladder")
    for pat in ("Initialize engine version", "RimWorld 1.6", "requesting fullscreen",
                "requesting resize", "resizing window", "Loaded All Assemblies",
                "Assertion at", "exited unexpectedly"):
        hits = [l for l in rows if pat in l]
        print(f"  {pat:<28} {len(hits):>4}  {hits[-1][:110] if hits else ''}")

    print("== X calls")
    c = Counter(m.group(1) for l in rows for m in [CALL.search(l)] if m)
    for k in ("XCreateWindow", "XMapWindow", "XReparentWindow", "XDestroyWindow",
              "XUnmapWindow", "XCheckIfEvent", "XIfEvent", "XGetWindowAttributes",
              "XGetWindowProperty", "XSendEvent", "XChangeProperty", "XResizeWindow"):
        print(f"  {k:<20} {c.get(k, 0)}")

    print("== [XHB] (last 3)")
    hb = [l for l in rows if "[XHB]" in l]
    for l in hb[-3:]:
        print("  " + l.strip()[:200])
    if not hb:
        print("  (none: the client never pumped 20 000 events)")

    print("== [XSW]")
    sw = [l for l in rows if "[XSW]" in l]
    for l in sw[-6:]:
        print("  " + l.strip()[:200])
    if not sw:
        print("  (none: glXSwapBuffers was never called)")

    print("== fullscreen handshake")
    fs = [l for l in rows if re.search(r"\[XE\] (FSGRANT|FSREV|FSDECL|FSSTATE|WMSTATE|WMSEND)", l)]
    seen = Counter(re.sub(r" n=\d+.*", "", l.strip()) for l in fs)
    for l in fs[:12]:
        print("  " + l.strip()[:200])
    if not fs:
        print("  (none)")
    print(f"  rows={len(fs)} kinds={dict(seen)}")

    print("== retry signature")
    # One fullscreen iteration = create container, map it, reparent the GL
    # window into it, reparent it back.  Print the shape between two reverts.
    rev = [i for i, l in enumerate(rows) if re.match(r"\[XC\] XReparentWindow win=(\d+) parent=1\b", l)]
    print(f"  reverts={len(rev)}")
    if len(rev) >= 2:
        a, b = rev[-2], rev[-1]
        seq = Counter(m.group(1) for l in rows[a:b] for m in [CALL.search(l)] if m)
        print(f"  calls per iteration ({b - a} rows): {dict(seq)}")
        # the arguments that repeat: created geometry + the awaited event
        for l in rows[a:b]:
            if "geom=" in l or "matched type=" in l or "FSDECL" in l or "FSGRANT" in l:
                print("    " + l.strip()[:150])
    return 0


if __name__ == "__main__":
    sys.exit(main())
