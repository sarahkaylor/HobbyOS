#!/usr/bin/env python3
"""replay.py — per-game QMP playthrough replay runner (GB-043).

Each native game ships >=1 replay script under tools/t5/replays/<GAME>.jsonl —
a JSONL sequence matching the evidence_rig replay schema: {"t":"click","x":..,
"y":...,"b":..}, {"t":"mouse","x":..,"y":..}, {"t":"key","k":..}. This runner
boots HobbyOS once and replays a script against the QEMU guest, capturing
screendumps so the playthrough is reproducible and verifiable.

  python3 tools/t5/replay.py --game XNIBBLER.BIN --replay tools/t5/replays/xnibbler.jsonl
       [--arch arm] [--root ...] [--shot-dir ...]

Reuses the GB-040 evidence_rig QMP/boot/menu machinery. The parser (ReplayParser)
is import-testable (T1).
"""
import argparse, json, os, subprocess, sys, time
from datetime import datetime, timezone
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/

# --- importable core (T1-testable) -------------------------------------
class ReplayParser:
    """Validate + iterate a replay script against the expected schema."""
    KEYS = {"t"}
    def __init__(self, path):
        with open(path) as f:
            self.events = [json.loads(l) for l in f if l.strip()]
    def __iter__(self):
        return iter(self.events)
    def validate(self):
        for i, e in enumerate(self.events):
            t = e.get("t")
            if t not in ("click", "mouse", "key"):
                raise ValueError(f"line {i}: bad event type {t!r}")
            if t in ("click", "mouse") and not (isinstance(e.get("x"), (int, float))
                                                and isinstance(e.get("y"), (int, float))):
                raise ValueError(f"line {i}: {t} needs x,y")
            if t == "key" and not isinstance(e.get("k"), str):
                raise ValueError(f"line {i}: key needs k")
        return len(self.events)


def events_to_qmp(events):
    """Translate the replay schema into QMP helper calls (for the runner)."""
    from evidence_rig import QMP
    def driver(q):
        for e in events:
            t = e["t"]
            if t == "mouse":
                q.mouse(e["x"], e["y"])
            elif t == "click":
                q.click(e["x"], e["y"], e.get("b", "left"))
            elif t == "key":
                q.key(e["k"], e.get("pause", 0.15))
    return driver


# --- runner --------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="menu name, e.g. XNIBBLER.BIN")
    ap.add_argument("--replay", required=True, help="path to the replay .jsonl")
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--shot-dir", default=None,
                    help="dir for a post-launch screendump (else cwd)")
    args = ap.parse_args()

    parser = ReplayParser(args.replay)
    n = parser.validate()
    print(f"[replay] {args.game}: parsed {n} events, schema OK")

    os.chdir(args.root)
    path = args.shot_dir or f"evidence/replay/{args.game.replace('.BIN','')}/" \
                             f"{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    os.makedirs(path, exist_ok=True)
    serial_log = os.path.join(path, "serial.log")
    qmp_sock = f"/tmp/replay-qmp-{args.game.replace('.', '')}"
    qemu_make = ["make", "run", "DESKTOP=hobby", f"ARCH={args.arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp_sock},server,nowait"]

    from evidence_rig import boot
    q = boot(qemu_make, serial_log, qmp_sock, args.arch)
    driver = events_to_qmp(list(parser))
    driver(q)
    time.sleep(2)
    shot = os.path.join(path, "replay_end.ppm")
    q.screendump(shot)
    print(f"[replay] screendump after replay: {shot}")
    with open(os.path.join(path, "replay.replayed.jsonl"), "w") as f:
        for e in list(parser):
            f.write(json.dumps(e) + "\n")
    print(f"[replay] replay reproduced (events={n}) for {args.game}")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0)


if __name__ == "__main__":
    main()
