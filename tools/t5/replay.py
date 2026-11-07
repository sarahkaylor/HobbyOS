#!/usr/bin/env python3
"""replay.py — per-game QMP playthrough replay runner (GB-043).

Each native game ships >=1 replay script under tools/t5/replays/<GAME>.jsonl —
a JSONL sequence matching the evidence_rig replay schema: {"t":"click","x":..,
"y":...,"b":..}, {"t":"mouse","x":..,"y":..}, {"t":"key","k":..}. This runner
boots HobbyOS once and replays a script against the QEMU guest, capturing
screendumps so the playthrough is reproducible and verifiable.

  python3 tools/t5/replay.py --game XNIBBLER.BIN --replay tools/t5/replays/xnibbler.jsonl
       [--arch arm] [--root ...] [--shot-dir ...]
  python3 tools/t5/replay.py --game STARDUST.BIN --replay tools/t5/replays/stardust.jsonl \
       --probe 1030:1055:80,220,255     # GB-213: ship-band x-centroid, before/after

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
def split_nav(events):
    """Split a replay into (nav prefix, in-game events).

    GB-091: the authored nav prefix (f1/home/downs/ret) ends at the launcher's
    confirm; the runner launches the title by name and replays only the events
    after it.  A replay with no `ret` is all in-game.
    """
    for i, e in enumerate(events):
        if e.get("t") == "key" and e.get("k") == "ret":
            return events[:i + 1], events[i + 1:]
    return [], events


# --- GB-182: whole-frame delta instrument ------------------------------
# The two REDs on GB-091 were about the *instrument*, not the engine: the
# with-input vs no-input claim rested on a local, uncommitted patch.  This
# is the committed instrument — two screendumps of the same run (before and
# after the in-game events) reduced to a whole-frame MAD and the non-black
# bbox of the changed pixels, printed as one marker line the gate can grep.
DELTA_THRESHOLD = 8  # per-channel |d| above which a pixel counts as changed


def read_ppm(path):
    """Parse a binary PPM (P6, maxval 255) -> (w, h, rgb bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM (P6)")
    # Header: P6 <w> <h> <maxval>\n, arbitrary whitespace, '#' comments.
    fields, i, n = [], 2, len(data)
    while len(fields) < 3:
        while i < n and data[i:i + 1].isspace():
            i += 1
        if i < n and data[i:i + 1] == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < n and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1  # single whitespace byte before the raster
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval} != 255")
    raster = data[i:i + w * h * 3]
    if len(raster) != w * h * 3:
        raise ValueError(f"{path}: short raster ({len(raster)} != {w * h * 3})")
    return w, h, raster


def frame_delta(before_path, after_path, threshold=DELTA_THRESHOLD):
    """Whole-frame MAD (0..255) + non-black bbox of changed pixels.

    Returns dict(mad, changed, total, bbox=(x0,y0,x1,y1) or None, w, h).
    A pixel is "changed" when any channel differs by more than `threshold`.
    """
    w0, h0, a = read_ppm(before_path)
    w1, h1, b = read_ppm(after_path)
    if (w0, h0) != (w1, h1):
        raise ValueError(f"frame size changed: {w0}x{h0} vs {w1}x{h1}")
    total = w0 * h0
    s = 0
    changed = 0
    x0 = y0 = 10 ** 9
    x1 = y1 = -1
    for p in range(total):
        o = p * 3
        dr = abs(a[o] - b[o])
        dg = abs(a[o + 1] - b[o + 1])
        db = abs(a[o + 2] - b[o + 2])
        s += dr + dg + db
        if dr > threshold or dg > threshold or db > threshold:
            changed += 1
            x, y = p % w0, p // w0
            if x < x0: x0 = x
            if x > x1: x1 = x
            if y < y0: y0 = y
            if y > y1: y1 = y
    bbox = None if changed == 0 else (x0, y0, x1, y1)
    return {"mad": s / (total * 3.0), "changed": changed, "total": total,
            "bbox": bbox, "w": w0, "h": h0}


def format_delta(tag, d):
    """The one marker line the gate greps (GB-182)."""
    box = "none" if d["bbox"] is None else "(%d,%d)-(%d,%d)" % d["bbox"]
    return (f"[REPLAY-DELTA] {tag} mad={d['mad']:.3f} bbox={box} "
            f"changed={d['changed']}/{d['total']} frame={d['w']}x{d['h']}")


# --- GB-213: bounded-region probe --------------------------------------
# The whole-frame MAD cannot answer "did the *ship* move": STARDUST animates a
# starfield and falling meteors, so two frames always differ.  A probe over a
# named row band and colour turns one game fact into a number
# (`--probe 1030:1055:80,220,255` -> ship-band x-centroid), and the no-input
# control (`--drop-in-game`) is what proves the number tracks input.
PROBE_TOL = 24  # per-channel |d| counted as "that colour"


def parse_probe(spec):
    """'y0:y1:r,g,b' -> (y0, y1, (r, g, b)); raises ValueError on nonsense."""
    try:
        band, rgb = spec.rsplit(":", 1)
        y0, y1 = (int(v) for v in band.split(":"))
        r, g, b = (int(v) for v in rgb.split(","))
    except (ValueError, TypeError):
        raise ValueError(f"bad --probe {spec!r}; want y0:y1:r,g,b") from None
    if not (0 <= y0 < y1):
        raise ValueError(f"bad --probe band {y0}:{y1}; want 0 <= y0 < y1")
    for c in (r, g, b):
        if not 0 <= c <= 255:
            raise ValueError(f"bad --probe colour component {c}")
    return y0, y1, (r, g, b)


def probe_band(path, y0, y1, rgb, tol=PROBE_TOL):
    """Count pixels matching `rgb` (±tol per channel) in rows [y0, y1) and
    return their x-centroid.  Rows past the frame bottom are clipped."""
    w, h, px = read_ppm(path)
    y1 = min(y1, h)
    count = s = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            if (abs(px[o] - rgb[0]) <= tol and abs(px[o + 1] - rgb[1]) <= tol
                    and abs(px[o + 2] - rgb[2]) <= tol):
                count += 1
                s += x
    return {"count": count,
            "xcentroid": (s / count) if count else None,
            "w": w, "h": h, "y0": y0, "y1": y1, "rgb": rgb}


def format_probe(tag, p):
    """The one marker line the gate greps (GB-213)."""
    xc = "none" if p["xcentroid"] is None else f"{p['xcentroid']:.1f}"
    return (f"[REPLAY-PROBE] {tag} y={p['y0']}-{p['y1']} "
            f"rgb={p['rgb'][0]},{p['rgb'][1]},{p['rgb'][2]} "
            f"count={p['count']} xcentroid={xc}")



def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="menu name, e.g. XNIBBLER.BIN")
    ap.add_argument("--replay", required=True, help="path to the replay .jsonl")
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--shot-dir", default=None,
                    help="dir for a post-launch screendump (else cwd)")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to wait after the launch before replaying the "
                         "in-game events (DOOM needs ~5 for its title screen)")
    ap.add_argument("--delta", dest="delta", action="store_true", default=True,
                    help="GB-182: capture a frame before and after the in-game "
                         "events and print the whole-frame MAD/bbox marker")
    ap.add_argument("--no-delta", dest="delta", action="store_false",
                    help="GB-182 falsifier: suppress the [REPLAY-DELTA] marker "
                         "(the gate must then FAIL)")
    ap.add_argument("--drop-in-game", action="store_true",
                    help="GB-182 no-input control: replay only the launch, with "
                         "the in-game events removed")
    ap.add_argument("--probe", default=None, metavar="Y0:Y1:R,G,B",
                    help="GB-213: also report the x-centroid of pixels matching "
                         "R,G,B in rows [Y0,Y1) of the before/after frames "
                         "(e.g. 1030:1055:80,220,255 for STARDUST's ship)")
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

    from evidence_rig import boot, find_menu_slot, launch_from_menu
    q = boot(qemu_make, serial_log, qmp_sock, args.arch)

    # GB-091/GB-059: launch the requested title BY NAME from the [MENU] dump.
    # The authored nav prefix (f1/home/downs/ret) is a fixed slot count and
    # goes to the wrong row as soon as the image gains a file (measured: 18
    # downs launched MILLIPED.BIN on a 177-entry menu).  Resolve the slot here
    # and replay only the events after the launcher's confirm.
    events = list(parser)
    nav, in_game = split_nav(events)
    if args.drop_in_game:
        print(f"[replay] no-input control: dropping {len(in_game)} in-game events")
        in_game = []
    print(f"[replay] nav prefix {len(nav)} events; {len(in_game)} in-game events")
    slot = find_menu_slot(serial_log, args.game)
    if slot is None:
        print(f"[replay] FATAL: {args.game} not in the start-menu dump")
        sys.exit(1)
    print(f"[replay] {args.game} slot: found by name = {slot}")
    launch_from_menu(q, slot, arch=args.arch, gesture="keys")
    if args.settle > 0:
        time.sleep(args.settle)
    start_shot = os.path.join(path, "replay_start.ppm")
    if args.delta or args.probe:
        q.screendump(start_shot)
    print(f"[replay] replaying {len(in_game)} in-game events")
    driver = events_to_qmp(in_game)
    driver(q)
    time.sleep(2)
    shot = os.path.join(path, "replay_end.ppm")
    q.screendump(shot)
    print(f"[replay] screendump after replay: {shot}")
    if args.delta:
        d = frame_delta(start_shot, shot)
        print(format_delta(args.game, d))
    if args.probe:
        y0, y1, rgb = parse_probe(args.probe)
        before = probe_band(start_shot, y0, y1, rgb)
        after = probe_band(shot, y0, y1, rgb)
        print(format_probe(f"{args.game} before", before))
        print(format_probe(f"{args.game} after", after))
        if before["xcentroid"] is not None and after["xcentroid"] is not None:
            print(f"[replay] band displacement: "
                  f"{after['xcentroid'] - before['xcentroid']:+.1f} px")
    with open(os.path.join(path, "replay.replayed.jsonl"), "w") as f:
        for e in list(parser):
            f.write(json.dumps(e) + "\n")
    print(f"[replay] replay reproduced (events={n}) for {args.game}")
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0)


if __name__ == "__main__":
    main()
