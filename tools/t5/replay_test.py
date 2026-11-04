#!/usr/bin/env python3
"""Host (T1) self-test for replay.py's ReplayParser + replay scripts (GB-043)."""
import sys, os, json, glob, tempfile, shutil
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "t5"))
from replay import ReplayParser, split_nav, frame_delta, format_delta

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

# every authored replay must parse and validate, and must launch a game
# (nav events: click + home + downs + ret)
rep_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "replays")
replays = sorted(glob.glob(os.path.join(rep_dir, "*.jsonl")))
check(len(replays) >= 5, f"{len(replays)} per-game replay scripts present (>=5 games)")

for rp in replays:
    try:
        p = ReplayParser(rp)
        n = p.validate()
        ev = list(p)
        has_click = any(e["t"] == "click" for e in ev)
        # GB-116: F1 opens the Apps menu on both arches; a click cannot land on
        # intel (no pointer device), so a replay that opens the menu with `f1`
        # is as valid a nav as one that clicks.
        has_f1 = any(e["t"] == "key" and e["k"] == "f1" for e in ev)
        has_launch = any(e["t"] == "key" and e["k"] == "ret" for e in ev)
        check(n >= 20 and (has_click or has_f1) and has_launch,
              os.path.basename(rp) + f" parses ({n} events), navigates + launches")
    except Exception as ex:
        check(False, os.path.basename(rp) + f" FAILED: {ex}")

# schema errors must raise (GB-085: per-process fixture -- two concurrent
# `make host_tests` must not share a path, or one unlinks _bad.jsonl while the
# other is parsing it and the loser throws FileNotFoundError)

# GB-091/GB-059: the runner launches by name and replays only the events after
# the launcher's confirm, so the split must be exact.
print("split_nav (launch by name: nav prefix ends at the first ret)")
nav, rest = split_nav([{"t": "key", "k": "f1"}, {"t": "key", "k": "ret"},
                       {"t": "key", "k": "up"}, {"t": "key", "k": "ret"}])
check(len(nav) == 2 and nav[-1]["k"] == "ret", "nav ends at the FIRST ret")
check(len(rest) == 2 and rest[0]["k"] == "up", "in-game events start after it")
nav2, rest2 = split_nav([{"t": "key", "k": "up"}])
check(nav2 == [] and len(rest2) == 1, "no ret -> the whole replay is in-game")
nav3, rest3 = split_nav([])
check(nav3 == [] and rest3 == [], "empty replay splits into two empty halves")

_bad_dir = tempfile.mkdtemp(prefix="gb085_replay_")
try:
    bad = os.path.join(_bad_dir, "_bad.jsonl")
    open(bad, "w").write(json.dumps({"t": "teleport"}) + "\n")
    try:
        ReplayParser(bad).validate()
        check(False, "unknown event type rejected")
    except ValueError:
        check(True, "unknown event type rejected")
finally:
    shutil.rmtree(_bad_dir, ignore_errors=True)

# GB-182: the committed delta instrument.  Two frames of the same size reduce
# to a whole-frame MAD and the non-black bbox of the pixels that changed; the
# marker line is the gate's grep target, so it must survive formatting.
print("frame_delta / format_delta (GB-182 instrument)")

_d = tempfile.mkdtemp(prefix="gb182_delta_")
try:
    def _write_ppm(name, w, h, pixels):
        p = os.path.join(_d, name)
        with open(p, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h))
            f.write(bytes(pixels))
        return p

    black = _write_ppm("a.ppm", 4, 2, [0] * (4 * 2 * 3))
    same = _write_ppm("b.ppm", 4, 2, [0] * (4 * 2 * 3))
    d0 = frame_delta(black, same)
    check(d0["mad"] == 0.0 and d0["changed"] == 0 and d0["bbox"] is None,
          "identical frames: mad 0.000, no bbox")

    # one white pixel at (3,1) in an otherwise black 4x2 frame
    px = [0] * (4 * 2 * 3)
    o = (1 * 4 + 3) * 3
    px[o] = px[o + 1] = px[o + 2] = 255
    lit = _write_ppm("c.ppm", 4, 2, px)
    d1 = frame_delta(black, lit)
    check(d1["changed"] == 1 and d1["bbox"] == (3, 1, 3, 1),
          "one lit pixel: changed=1, bbox=(3,1)-(3,1)")
    check(abs(d1["mad"] - (255.0 * 3 / (4 * 2 * 3))) < 1e-6,
          "MAD is the mean per-channel absolute difference")

    # a per-channel change at or below the threshold is not \"changed\"
    dim = [0] * (4 * 2 * 3)
    dim[0] = 5
    dimmed = _write_ppm("d.ppm", 4, 2, dim)
    d2 = frame_delta(black, dimmed)
    check(d2["changed"] == 0 and d2["bbox"] is None,
          "diff <= threshold does not count as changed")

    line = format_delta("DOOM.BIN", d1)
    check(line.startswith("[REPLAY-DELTA] DOOM.BIN mad=") and
          "bbox=(3,1)-(3,1)" in line and "changed=1/8" in line,
          "marker line carries mad, bbox, changed/total")
    check("bbox=none" in format_delta("X.BIN", d0),
          "no-change marker says bbox=none")
finally:
    shutil.rmtree(_d, ignore_errors=True)

print("\nTests:", "ALL PASSED" if fails == 0 else f"{fails} FAILED")
sys.exit(1 if fails else 0)
