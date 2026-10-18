#!/usr/bin/env python3
"""acceptance_run.py — S0/S1 final acceptance run (GB-053).

Boots HobbyOS once under QEMU (ARM), launches every pinned native game by NAME
from the [MENU] dump, and captures a screendump per game, asserting the game
renders a live (non-blank) frame.  Aggregates a per-game PASS/FAIL table into
S0-S1-ACCEPTANCE.md plus S0-S1-ACCEPTANCE-MANIFEST.json (sha16 provenance),
the evidence base for "S0+S1 signed off".

The liveness check is a real pixel diff (image is not uniformly one color), so
the verdict comes from the actual framebuffer, not from "it launched".
"""
import argparse, glob, json, os, re, subprocess, sys, time, hashlib
from datetime import datetime, timezone
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

PINNED = ["PONG.BIN", "MILLIPED.BIN", "XNIBBLER.BIN", "XCRYPT.BIN", "NES.BIN"]

def sha16(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()[:16]

def ppm_live(path, samples=200000):
    """True if the screendump has real content (more than one distinct pixel
    value) — i.e. it rendered a frame, not a blank/black screen."""
    data = open(path, "rb").read()
    # skip header line ("P6\nW H\n255\n")
    try:
        parts = data.split(b"\n", 3)
        raw = parts[3]
    except Exception:
        return False
    seen = set()
    n = 0
    for i in range(0, min(len(raw), samples * 3), 9):
        px = raw[i:i+3]
        seen.add(px)
        n += 1
        if len(seen) > 1:
            return True
    return len(seen) > 1 and n > 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    args = ap.parse_args()
    root = args.root
    os.chdir(root)

    outdir = "evidence/acceptance/" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    os.makedirs(outdir, exist_ok=True)
    serial = os.path.join(outdir, "serial.log")
    qmp = "/tmp/acceptance-qmp"
    qemu_make = ["make", "run", f"ARCH={args.arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp},server,nowait"]

    from evidence_rig import boot, find_menu_slot, launch_from_menu
    q = boot(qemu_make, serial, qmp, args.arch)
    menu = open(serial, errors="ignore").read()

    rows = []
    all_ok = True
    for game in PINNED:
        slot = find_menu_slot(serial, game)
        if slot is None:
            rows.append({"game": game, "slot": None, "live": False, "ok": False,
                         "note": "not in [MENU] dump"})
            all_ok = False
            continue
        launch_from_menu(q, slot)
        time.sleep(3)
        ppm = os.path.join(outdir, game.replace(".BIN", "") + ".ppm")
        q.screendump(ppm)
        live = ppm_live(ppm)
        rows.append({"game": game, "slot": slot, "live": live, "ok": live,
                     "sha16": sha16(ppm)})
        if not live:
            all_ok = False
        q.key("esc", 0.4)
        time.sleep(1.2)

    # report
    L = ["# S0/S1 Final Acceptance Run (GB-053)\n",
         f"- arch: {args.arch}, pinned games: {len(PINNED)}, at {datetime.now(timezone.utc).isoformat()}\n",
         "- verdict from each game's own screendump (live non-blank frame), "
         "not from 'it launched'.\n\n",
         "| game | slot | live frame | result |\n|---|---|---|---|\n"]
    for r in rows:
        L.append(f"| {r['game']} | {r.get('slot','-')} | {r.get('live','-')} |"
                 f" {'PASS' if r['ok'] else 'FAIL'} |\n")
    L.append(f"\nSummary: **{'PASS' if all_ok else 'FAIL'}**\n")
    L.append("\n- intel leg gated on GB-057 (intel spawn deadlock); this run is ARM.\n")
    L.append("- 'S0+S1 signed off' is the milestone judgment; this run is its evidence base.\n")
    with open("S0-S1-ACCEPTANCE.md", "w") as f:
        f.write("".join(L))

    manifest = {"arch": args.arch, "captured_at": datetime.now(timezone.utc).isoformat(),
                "results": rows,
                "summary": "PASS" if all_ok else "FAIL"}
    with open("S0-S1-ACCEPTANCE-MANIFEST.json", "w") as f:
        json.dump(manifest, f, indent=2)

    print("".join(L))
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
