#!/usr/bin/env python3
"""tools/cp/hunt/r4table.py LOGS RUN... -- round 4's per-boot table (lane
H9-hunt4, docs/cp-hunt.md): r3table.py's reading of each boot plus

  act     minutes of active input in the world (drive.py `play`: PLAY SUMMARY)
  veh     vehicle entered / tried; drv = seconds driven
  menus   in-world menus opened and closed / tried (map, inventory, journal, phone, ...)
  exit    how the session ended (drive.py --exit: pause, vehicle, load, settings)
  ghitch  glasswatch.py: frames >= 250 ms with no new pixels in a world-like
          drive state (walking, driving); gmax = the longest (ms)
  tlb / free  [TLBTIMEOUT] / [FREERACE] rows
  nts     [NTSYNC] live objects first -> last (Wine's sync-object count)
  cons    the stall-proof console's host stalls (stalls / stolen ms), last row
  sndz / xrun   [SND] zeros rows / kernel [SND] xrun rows (audio boots)
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r3table  # noqa: E402


def _read(path):
    try:
        return open(path, errors="replace").read()
    except OSError:
        return ""


def play_summary(path):
    d = {}
    if not os.path.exists(path):
        return d
    for ln in open(path, errors="replace"):
        if "PLAY SUMMARY" in ln:
            d = dict(re.findall(r"(\w+)=(\S+)", ln.split("PLAY SUMMARY", 1)[1]))
    return d


def glass(path):
    if not os.path.exists(path):
        return None, None
    n = mx = None
    for ln in open(path, errors="replace"):
        m = re.match(r"HITCHES .*?: (\d+)(?:; max (\d+) ms)?", ln)
        if m:
            n, mx = int(m.group(1)), int(m.group(2)) if m.group(2) else 0
    return n, mx


def row(logs, run):
    r = r3table.row(logs, run)
    j = json.load(open(os.path.join(logs, run + ".class.json")))
    kf = j["classes"]["kfault"]
    r["tlb"] = sum(v[0] for s, v in kf.items() if s.startswith("tlbtimeout"))
    r["free"] = sum(v[0] for s, v in kf.items() if s.startswith("freerace"))
    ses = j.get("session", {})
    nt = ses.get("ntsync") or []
    r["nts"] = f"{nt[0]['live']}->{nt[-1]['live']}" if nt else None
    c = ses.get("cons")
    r["cons"] = f"{c['stalls']}/{c['stolen_ms']}" if c else None
    r["sndz"] = ses.get("snd_zeros")
    r["xrun"] = sum(v[0] for s, v in j["classes"]["audio"].items() if "xrun" in s)
    p = play_summary(os.path.join(logs, run + ".drive.txt"))
    r["act"] = p.get("active_min")
    r["veh"] = p.get("vehicle")
    r["drv"] = p.get("drive_s")
    r["menus"] = p.get("menus")
    r["exit"] = p.get("exit")
    if p.get("active_min") or re.search(r"MAIN LOAD 0 .*: world ", _read(os.path.join(logs, run + ".drive.txt"))):
        r["world"] = "yes"          # a drive boot reaches the world through drive.py, not `expect inworld`
    r["ghitch"], r["gmax"] = glass(os.path.join(logs, run + ".glass-report.txt"))
    # fps rows: the world-like phases (drive.py's states are boot_p9 phases from h4-p03 on)
    gaps = j.get("fps_gaps") or {}
    wl = {p: v for p, v in gaps.items() if p in ("world", "walk", "drive") or p.startswith("soak")}
    if wl:
        r["hitch"] = sum(v.get("hitches", 0) or 0 for v in wl.values())
        rows_ = sum(v["rows"] for v in wl.values())
        r["wmed"] = round(sum(v["median"] * v["rows"] for v in wl.values()) / max(1, rows_), 1)
    w = os.path.join(logs, run + ".wav.txt")
    r["wdrop"] = None
    if os.path.exists(w):
        m = re.search(r"dropouts[^0-9]{0,20}(\d+)", open(w, errors="replace").read())
        r["wdrop"] = int(m.group(1)) if m else None
    return r


COLS = ["run", "verdict", "world", "act", "veh", "drv", "menus", "exit", "quit", "clean", "kill_ms", "left",
        "faults", "tlb", "free", "devlost", "stalls", "ghitch", "gmax", "wmed", "nts", "cons", "sndz", "xrun",
        "wdrop", "shots", "agree"]


def main():
    logs, runs = sys.argv[1], sys.argv[2:]
    rows = [row(logs, r) for r in runs if os.path.exists(os.path.join(logs, r + ".class.json"))]
    print("| " + " | ".join(COLS) + " |")
    print("|" + "---|" * len(COLS))
    for r in rows:
        print("| " + " | ".join("-" if r.get(c) is None else str(r[c]) for c in COLS) + " |")
    n = len(rows)
    if not n:
        return 0

    def k(f):
        b = [r["run"] for r in rows if f(r)]
        return f"{len(b)}/{n}" + (f" ({' '.join(b)})" if 0 < len(b) < n else "")
    print()
    print(f"world reached: {k(lambda r: r['world'] == 'yes')}")
    print(f"clean exit: {k(lambda r: r['clean'] == 'yes')}")
    print(f"kernel fault rows: {k(lambda r: r['faults'] > 0)}; [TLBTIMEOUT]: {k(lambda r: r['tlb'] > 0)}; "
          f"[FREERACE]: {k(lambda r: r['free'] > 0)}")
    print(f"device-lost rows: {k(lambda r: r['devlost'] > 0)}")
    print(f"glass hitches >= 250 ms in play: {k(lambda r: (r['ghitch'] or 0) > 0)}; "
          f"total {sum(r['ghitch'] or 0 for r in rows)}")
    print(f"verdict disagrees with the boot: {k(lambda r: r['agree'] == 'NO')}")
    act = [float(r["act"]) for r in rows if r.get("act")]
    if act:
        print(f"active play: {len(act)} boots, {sum(act):.1f} min, >= 15 min in {sum(1 for a in act if a >= 15)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
