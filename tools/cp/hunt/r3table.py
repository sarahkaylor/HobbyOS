#!/usr/bin/env python3
"""tools/cp/hunt/r3table.py LOGS RUN... -- round 3's per-boot table (lane
H6-hunt3, docs/cp-hunt.md): one row per boot from run-hunt.sh's files in
LOGS (RUN.class.json from classify.py, RUN.log = the boot_p9 timeline,
RUN.shotcheck.json = `shotcheck.py --json OUT` when present), plus the k/n lines.

Columns: the boot's own verdict, then this lane's own reading of the same
boot, so a grader verdict that disagrees with what happened shows up:
  menu    QEMU start -> main menu OCR (s)
  load    LOAD GAME Enter -> PRESS ... TO CONTINUE (s) (`phase loading` -> `expect loaded`)
  world   the HUD seen after the load (expect inworld*)
  exit2t  EXIT TO MAIN MENU confirm -> the title screen (s), mq boots only
  quit    the pause menu's quit confirm -> `[P8] end game` (s); from the main menu
          (no confirm dialog there) QUIT GAME's Enter -> `[P8] end game`
  clean   the game's status 0, wineserver -k status 0, 0 processes left after it,
          sync, `[P8] done` (classify.py's "clean exit")
  -k ms   how long `wineserver -k` took
  faults  kernel fault rows; stalls = kernel stall rows (nfspipe mostly)
  hitch   guest-time fps gaps > 2.5 s in the world phase
  shots   shotcheck flags in world shots (black / partial / tear? / garbage? / same)
  agree   whether the verdict and this reading agree (a pass verdict on a boot
          that did not quit cleanly, or a fail verdict on one that did, is "NO")
"""
import json
import os
import re
import sys


def tl_times(path):
    t = {}
    if not os.path.exists(path):
        return t
    ph = []
    for ln in open(path, errors="replace"):
        if not ln.startswith("[P9W] +"):
            continue
        m = re.match(r"\[P9W\] \+\s+([0-9.]+) s\s+\S+\s+(.*)", ln)
        if not m:
            continue
        s, msg = float(m.group(1)), m.group(2)
        if msg.startswith("phase "):
            ph.append((s, msg.split()[1]))
        if msg.startswith("main menu reached") and "menu" not in t:
            t["menu"] = s
        if msg.startswith("expect loaded:"):
            t["loaded_at"] = s
        if msg.startswith("expect inworld") and "matched" in msg:
            t["world_at"] = s
        if msg.startswith("expect title:"):
            t["title_at"] = s
        if msg.startswith("expect quit-confirm:"):
            t["quitc_at"] = s
        if "game process ended" in msg or "[P8] end game" in msg:
            t.setdefault("end_at", s)
        if msg.startswith("verdict:"):
            t["verdict_at"] = s
    for s, p in ph:
        t.setdefault("ph_" + p, s)
    return t


def row(logs, run):
    j = json.load(open(os.path.join(logs, run + ".class.json")))
    t = tl_times(os.path.join(logs, run + ".log"))
    rl, ses, sc = j["runlog"], j.get("session", {}), j.get("script", {})
    ends = ses.get("ends", {})
    g, k = ends.get("game"), ends.get("kill")
    q = [x for x in ses.get("quiesce", []) if x["after"] == "kill"]
    live = [x for x in q[-1]["left"] if not x.endswith(":s4")] if q else None    # :s4 = exited, unreaped (H6-03)
    clean = bool(g and g["status"] == "0" and k and k["status"] == "0" and q and not live and ses.get("done"))
    load = round(t["loaded_at"] - t["ph_loading"], 1) if "loaded_at" in t and "ph_loading" in t else None
    ex2t = round(t["title_at"] - t["ph_title"], 1) if "title_at" in t and "ph_title" in t else None
    # pause menu: confirm dialog -> Enter -> exit; main menu: QUIT GAME's Enter exits at
    # once, no dialog (h3-c07: "game process ended" 3 s after `phase quit`)
    if "end_at" in t and "quitc_at" in t and t["quitc_at"] < t["end_at"]:
        quit_s = round(t["end_at"] - t["quitc_at"], 1)
    elif "end_at" in t and "ph_quit" in t and "ph_exitmain" in t:
        quit_s = round(t["end_at"] - t["ph_quit"], 1)
    else:
        quit_s = None
    kf = sum(v[0] for v in j["classes"]["kfault"].values())
    ks = sum(v[0] for v in j["classes"]["kstall"].values())
    gaps = j.get("fps_gaps") or {}
    hitch = sum(v.get("hitches", 0) or 0 for p, v in gaps.items() if p == "world")
    wmed = gaps.get("world", {}).get("median")
    shots = "-"
    sp = os.path.join(logs, run + ".shotcheck.json")
    if os.path.exists(sp):
        s = json.load(open(sp))
        fl = {}
        for r in s.get("flagged", []):
            if r["tag"] == "final":       # the desktop after the game quit: not a game frame
                continue
            if (r.get("w"), r.get("h")) == (1920, 1080) and r["flags"] == ["partial-black"] \
                    and abs(r["band_bottom"] - 0.333) < 0.01 and abs(r["band_right"] - 0.333) < 0.01:
                continue                  # the 1280x720 X screen in a 1920x1080 framebuffer: the desktop, no game window

            for f in r["flags"]:
                f = f.split("@")[0]
                fl[f] = fl.get(f, 0) + 1
        shots = (",".join(f"{k}x{v}" for k, v in sorted(fl.items())) or "0") + f"/{s.get('shots', 0)}"
    verdict = rl.get("verdict", "?")
    passed = verdict in ("script-done", "script-stop", "launcher-done")
    agree = "yes" if passed == clean else "NO"
    return {"run": run, "verdict": verdict, "menu": t.get("menu"), "load": load,
            "world": "yes" if sc.get("inworld") else "no", "exit2t": ex2t, "quit": quit_s,
            "clean": "yes" if clean else "no", "kill_ms": k["ms"] if k else None,
            "left": len(live) if q else None, "game_ms": g["ms"] if g else None,
            "faults": kf, "stalls": ks, "hitch": hitch, "wmed": wmed, "shots": shots, "agree": agree,
            "early": rl.get("early_exit"), "devlost": len(ses.get("devlost", [])),
            "emfile": len(ses.get("emfile", []))}


def main():
    logs, runs = sys.argv[1], sys.argv[2:]
    rows = [row(logs, r) for r in runs if os.path.exists(os.path.join(logs, r + ".class.json"))]
    cols = ["run", "verdict", "menu", "load", "world", "exit2t", "quit", "clean", "kill_ms", "left",
            "faults", "stalls", "hitch", "wmed", "shots", "devlost", "agree"]
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    for r in rows:
        print("| " + " | ".join("-" if r[c] is None else str(r[c]) for c in cols) + " |")
    n = len(rows)
    if not n:
        return 0

    def k(f):
        b = [r["run"] for r in rows if f(r)]
        return f"{len(b)}/{n}" + (f" ({' '.join(b)})" if 0 < len(b) < n else "")
    print()
    print(f"world reached: {k(lambda r: r['world'] == 'yes')}")
    print(f"clean exit: {k(lambda r: r['clean'] == 'yes')}")
    print(f"early exits: {k(lambda r: r['early'])}")
    print(f"kernel fault rows: {k(lambda r: r['faults'] > 0)}")
    print(f"device-lost rows: {k(lambda r: r['devlost'] > 0)}; EMFILE/grab_object rows: {k(lambda r: r['emfile'] > 0)}")
    print(f"hitches in the world: {k(lambda r: r['hitch'] > 0)}")
    print(f"verdict disagrees with the boot: {k(lambda r: r['agree'] == 'NO')}")
    for c in ("menu", "load", "exit2t", "quit", "kill_ms"):
        v = sorted(r[c] for r in rows if r[c] is not None)
        if v:
            print(f"{c}: median {v[len(v) // 2]} (n={len(v)}, {v[0]}..{v[-1]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
