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


def ppm_dims(path):
    """Parse a P6 PPM header -> (width, height).  Header is 'P6\\n<W> <H>\\n255\\n'."""
    with open(path, "rb") as f:
        head = f.readlines(4)
    # P6, dims, maxval, then binary RGB.  Tolerate a comment line if present.
    line = 1
    dims = None
    while line < len(head) and dims is None:
        tok = head[line].strip().split()
        while tok and tok[0].startswith(b"#"):
            line += 1
            if line >= len(head):
                break
            tok = head[line].strip().split()
        if not tok:
            line += 1
            continue
        dims = (int(tok[0]), int(tok[1]))
    return dims


def ppm_raw(path):
    """Return the binary RGB payload (bytes) after the P6 header."""
    data = open(path, "rb").read()
    try:
        return data.split(b"\n", 3)[3]
    except Exception:
        return b""


def ppm_live(path, samples=200000, whole=True):
    """True if the screendump has real content (more than one distinct pixel).

    GB-075: the previous implementation sampled only raw[0 : samples*3] at a
    9-byte stride, i.e. the top ~104 of 1080 rows, so a live frame whose
    content sits lower on screen read as blank (a false negative) and, worse,
    could not distinguish a game that never loaded (still on a live menu).  Now
    the whole frame is sampled: the pixel stride is chosen to span every row, so
    liveness reflects the entire framebuffer, and a 'live' desktop-only frame no
    longer passes the *gate* (identity is checked separately, see gate_game).
    """
    dims = ppm_dims(path)
    if not dims:
        return False
    w, h = dims
    raw = ppm_raw(path)
    total = w * h
    if total == 0 or len(raw) < total * 3:
        return False
    step = max(1, total // samples)           # pixel stride to cover whole frame
    seen = set()
    n = 0
    for i in range(0, total, step):
        b = 3 * i
        seen.add(raw[b:b + 3])
        n += 1
        if len(seen) > 1:
            return True
    return len(seen) > 1 and n > 0


def ppm_whole_mad(path_a, path_b, samples=200000):
    """Mean-absolute-difference over the whole frame, /255 (0..1).

    GB-075: the five 'different' games were pairwise mean-abs-diff 0.15-0.97/255
    (96.9-99.8 % shared black), i.e. the rig could not tell them apart.  This is
    the margin that proves a title that 'runs' actually changed the frame.  Uses
    the same whole-frame stride as ppm_live so both measures are comparable.
    """
    ra, rb = ppm_raw(path_a), ppm_raw(path_b)
    da, db = ppm_dims(path_a), ppm_dims(path_b)
    if not da or not db:
        return 0.0
    n = min(len(ra), len(rb), da[0] * da[1] * 3)
    if n < 3:
        return 0.0
    total = n // 3
    step = max(1, total // samples)
    acc = 0
    cnt = 0
    for i in range(0, total, step):
        b = 3 * i
        acc += (abs(ra[b] - rb[b]) + abs(ra[b + 1] - rb[b + 1])
                + abs(ra[b + 2] - rb[b + 2])) / 3
        cnt += 1
    return (acc / cnt / 255.0) if cnt else 0.0


# Serial "the title loaded" markers (kernel program_loader.c / desktop path).
LOAD_RE = re.compile(r"Loading program(?: for scheduler(?: \(v2 AS\))?)?: (\S+\.BIN)")


def title_loaded(serial_text, name):
    """Identity as an OBSERVED event: the serial must show THIS .BIN actually
    LOADED (a load line), not merely that it is NAMED anywhere.

    GB-075: XCRYPT/NES 'PASSED' whose only appearance was the mcopy and the
    [MENU] dump — never a load line, never a process.  A title whose BIN never
    appears in the LOAD stream did not load, so it must FAIL regardless of what
    the (still-live) menu shows.  Matching the plain name is NOT enough: every
    pinned game is listed in the [MENU] dump and the mcopy regardless of load,
    which is precisely the fabricated-PASS vector this fixes.
    """
    base = name.upper()
    if not base.endswith(".BIN"):
        base += ".BIN"
    for m in LOAD_RE.finditer(serial_text):
        if m.group(1).upper() == base:
            return True
    return False


# Stated whole-frame margin (GB-075): a 'different game' that ran must move the
# frame by at least this mean-abs-diff over the whole frame, or it did not
# render a distinct title (it is a fabricated PASS).  Grounded on GB-053-V's
# measured fabrication: the five never-different 'games' were pairwise mean-
# abs-diff 0.15-0.97/255 (max 0.0038), so a margin of 1/255 (0.004) sits just
# above that ceiling while real distinct renders exceed it by an order of
# magnitude.
GAME_DIFF_MARGIN = 0.004  # ~1 / 255 over the whole frame


def gate_game(serial_text, name, ppm, prev_ppm=None):
    """Return (ok, reasons) for one game's acceptance, from OBSERVED evidence.

    FAILS when: the title never loaded (no serial load line/process), the frame
    is blank, or (when a previous frame exists) the two 'different' titles are
    within GAME_DIFF_MARGIN of each other over the whole frame.
    """
    reasons = []
    if not title_loaded(serial_text, name):
        reasons.append("never loaded (no serial load line / process)")
    if not ppm_live(ppm):
        reasons.append("blank/non-live frame")
    if prev_ppm and not reasons:
        mad = ppm_whole_mad(prev_ppm, ppm)
        if mad < GAME_DIFF_MARGIN:
            reasons.append(
                f"frame changed only {mad:.3f} < margin {GAME_DIFF_MARGIN} "
                "(not a distinct rendered title)")
    return (len(reasons) == 0, reasons)


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
    prev_ppm = None
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
        serial_text = open(serial, errors="ignore").read()
        live = ppm_live(ppm)
        ok, reasons = gate_game(serial_text, game, ppm, prev_ppm)
        rows.append({"game": game, "slot": slot, "live": live, "ok": ok,
                     "reasons": reasons, "sha16": sha16(ppm)})
        if not ok:
            all_ok = False
        prev_ppm = ppm
        q.key("esc", 0.4)
        time.sleep(1.2)

    # report
    L = ["# S0/S1 Final Acceptance Run (GB-053)\n",
         f"- arch: {args.arch}, pinned games: {len(PINNED)}, at {datetime.now(timezone.utc).isoformat()}\n",
         "- verdict from OBSERVED evidence (GB-075, no fabricated PASS): each game\n"
         "  must LOAD (serial load line / live process), render a live whole-frame,\n"
         "  and (after the first title) differ from the previous one over the whole\n"
         "  frame by at least the stated margin.\n\n",
         "| game | slot | loaded | live frame | result |\n|---|---|---|---|---|\n"]
    for r in rows:
        if 'note' in r:
            L.append(f"| {r['game']} | - | no | {r.get('live','-')} | FAIL |\n"
                     f"  - {r['note']}\n")
            continue
        loaded = "yes" if title_loaded(open(serial, errors="ignore").read(), r['game']) else "no"
        L.append(f"| {r['game']} | {r.get('slot','-')} | {loaded} | {r.get('live','-')} |"
                 f" {'PASS' if r['ok'] else 'FAIL'} |\n")
        if r.get('reasons'):
            L.append(f"  - reasons: {'; '.join(r['reasons'])}\n")
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
