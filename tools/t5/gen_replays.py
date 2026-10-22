#!/usr/bin/env python3
"""Generate per-game replay scripts (GB-043) under tools/t5/replays/.

Each replay is a JSONL sequence matching the evidence_rig schema: navigate the
launcher to the game's menu slot then send a scripted in-game input so the
playthrough is both launchable and interactive.  Slot comes from the [MENU]
dump at capture time (de-magic); the generator defaults are informational.
"""
import json, os

H, W = 1080, 1920
TASKBAR = H - 26            # evidence_rig launch_from_menu uses H-26
replays = {
    # game_menu_name: (slot, [scripted qcode inputs])
    "XNIBBLER.BIN": (18, ["left", "up", "left", "up"]),
    "XCRYPT.BIN":   (19, ["up", "left", "up", "left"]),
    "NES.BIN":      (20, []),     # full-screen; launch is the playable proof
    "PONG.BIN":     (16, ["left", "left", "ret"]),
    "MILLIPED.BIN": (17, ["up", "up"]),
}

def nav(slot):
    ev = [{"t": "click", "x": 38, "y": TASKBAR + 13, "b": "left"},
          {"t": "key", "k": "home"}]
    for _ in range(slot):
        ev.append({"t": "key", "k": "down"})
    ev.append({"t": "key", "k": "ret"})
    return ev

outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "replays")
os.makedirs(outdir, exist_ok=True)
for name, (slot, inputs) in replays.items():
    ev = nav(slot)
    for k in inputs:
        ev.append({"t": "key", "k": k, "pause": 0.2})
    stem = name.replace(".BIN", "").lower()
    with open(os.path.join(outdir, stem + ".jsonl"), "w") as f:
        for e in ev:
            f.write(json.dumps(e) + "\n")
    print(f"wrote {outdir}/{stem}.jsonl ({len(ev)} events, slot {slot})")
