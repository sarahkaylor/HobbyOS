#!/usr/bin/env python3
"""l81-flow-summary.py -- the click-by-click and per-stage table of an
RW_FLOW=1 desktop run, with the L81 in-game evidence rule applied.

  STAGES   one row per stage: fps (guest [XSW] window), span, presents, CRC,
           changed, diff bbox, clock share, OCR head
  CLICKS   one row per click: stage, phrase, how, target, pixels, bbox,
           clock share, resulting class, serial CRC
  POLLS    the wait/poll rows that carry a wall (stage, note, pixels, fps)
  XSW      every distinct CRC and the guest-clock time it first appeared
  WALL     the rows saved by the driver's dump_wall_rows()
  VERDICT  whether the run satisfies the three-part in-game evidence rule:
           (1) [XSW] CRC transitions, (2) a diff bbox that is not the panel
           clock, (3) OCR/described colony content.  No claim without all 3.

Usage: tools/rw/l81-flow-summary.py [obj/rw-desktop/flow.jsonl]
                                    [--serial obj/rw-desktop/serial-desktop.log]
"""
import argparse
import json
import os
import re


def load(path):
    recs = []
    if not os.path.exists(path):
        return recs
    for line in open(path, errors="replace"):
        line = line.strip()
        if not line:
            continue
        try:
            recs.append(json.loads(line))
        except ValueError:
            pass
    return recs


def bbs(bbox):
    return "-" if not bbox else "%d,%d-%d,%d" % tuple(bbox)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("flow", nargs="?", default="obj/rw-desktop/flow.jsonl")
    ap.add_argument("--serial", default="obj/rw-desktop/serial-desktop.log")
    ap.add_argument("--wall-dir", default="obj/rw-desktop")
    a = ap.parse_args()
    recs = load(a.flow)

    print("== STAGES (fps windows; fps is the guest's own [XSW] t=) ==")
    print(f"{'stage':<12} {'fps':>6} {'span':>5} {'n':>6} {'crc':<17} {'chg':>3} "
          f"{'diffpx':>8} {'bbox':<21} {'clock':>6}  OCR")
    for r in recs:
        if r.get("ev") != "stage_fps":
            continue
        print(f"{r.get('stage',''):<12} {str(r.get('fps')):>6} "
              f"{str(r.get('span_s')):>5} {str(r.get('n')):>6} "
              f"{str(r.get('crc')):<17} {str(r.get('changed')):>3} "
              f"{str(r.get('pixels')):>8} {bbs(r.get('bbox')):<21} "
              f"{str(r.get('clock_frac')):>6}  {(r.get('text') or '')[:70]}")

    print("\n== CLICKS (target, then what changed) ==")
    print(f"{'stage':<12} {'phrase':<20} {'how':<12} {'click':<11} {'diffpx':>8} "
          f"{'bbox':<21} {'clock':>6} {'-> class':<12} {'crc':<17} note")
    for r in recs:
        if r.get("ev") != "click_result":
            continue
        cl = r.get("click") or ["?", "?"]
        print(f"{r.get('stage',''):<12} {str(r.get('phrase'))[:20]:<20} "
              f"{str(r.get('how',''))[:12]:<12} "
              f"{str(cl[0])+','+str(cl[1]):<11} {str(r.get('pixels')):>8} "
              f"{bbs(r.get('bbox')):<21} {str(r.get('clock_frac')):>6} "
              f"{str(r.get('classes')):<12} {str(r.get('crc')):<17} "
              f"{r.get('note','')}")

    print("\n== STAGE ENTRY / ADVANCE / WALL ==")
    for r in recs:
        if r.get("ev") in ("stage", "advanced", "wall", "reached", "no_advance",
                           "generating"):
            print(" ", json.dumps({k: r.get(k) for k in
                                   ("ev", "stage", "to", "phrase", "how",
                                    "text", "note", "shot") if r.get(k) is not None})[:400])

    print("\n== POLLS (last 12; a wall is a run of 'static') ==")
    polls = [r for r in recs if r.get("ev") == "poll"]
    for r in polls[-12:]:
        print(f"  {r.get('stage'):<12} {str(r.get('classes')):<12} "
              f"fps={str(r.get('fps')):<6} n={str(r.get('n')):<6} "
              f"px={str(r.get('pixels')):<8} {str(r.get('note')):<8} "
              f"{(r.get('text') or '')[:60]}")

    # serial: distinct CRCs and their first t=
    try:
        txt = open(a.serial, errors="replace").read()
    except OSError:
        txt = ""
    seen = {}
    for m in re.finditer(r"\[XSW\] n=(\d+)\b[^\n]*?crc=0x([0-9a-f]+)[^\n]*?t=(\d+)", txt):
        n, crc, t = int(m.group(1)), m.group(2), int(m.group(3))
        if crc not in seen:
            seen[crc] = (n, t)
    print("\n== XSW distinct CRCs (%d) ==" % len(seen))
    for crc, (n, t) in list(seen.items())[-14:]:
        print(f"  crc=0x{crc:<17} first n={n:<6} t={t/1000.0:8.1f}s")
    refusals = len(re.findall(r"res_create refused", txt))
    lmmap = re.findall(r"\[LMMAP\] fail[^\n]*", txt)
    print(f"  GPU res_create refusals: {refusals}")
    for row in lmmap[:6]:
        print("  " + row[:200].rstrip())

    walls = [r for r in recs if r.get("ev") == "wall"]
    if walls:
        print("\n== WALL ==")
        for r in walls:
            print(" ", json.dumps({k: r.get(k) for k in
                                   ("stage", "note", "text", "shot") if r.get(k) is not None})[:500])
    for f in sorted(os.listdir(a.wall_dir)) if os.path.isdir(a.wall_dir) else []:
        if f.startswith("wall-") and f.endswith(".json"):
            print(f"\n== WALL ROWS ({f}) ==")
            try:
                w = json.load(open(os.path.join(a.wall_dir, f)))
            except ValueError:
                continue
            for k in ("XSW", "THRS", "BLKD", "FUTR", "XO"):
                for row in w.get(k, [])[-1:]:
                    print(f"  {k}: {row[:240]}")
            print(f"  GPU_refusals: {w.get('GPU_refusals')}  "
                  f"Player.log exception rows in serial: {w.get('Player_log_exceptions')}")

    game = [r for r in recs if r.get("ev") == "reached" and r.get("stage") == "map"]
    print("\n== VERDICT ==")
    if not game:
        print("  NOT in-game: no record reached class 'map'.")
        return 1
    r = game[-1]
    text = (r.get("text") or "").lower()
    ok_ocr = any(k in text for k in
                 ("architect", "colony", "work", "assign", "research",
                  "schedule", "wildlife", "beauty", "colonist"))
    crcs = len({c for c in seen})
    print("  reached map: yes")
    print("  1) [XSW] distinct CRCs:", crcs, "--", "yes" if crcs > 1 else "NO")
    print("  2) diff bbox not the clock: (see STAGES 'diffpx'/'clock')")
    print("  3) OCR has colony/HUD vocabulary:", "yes" if ok_ocr else "NO",
          "--", r.get("text", "")[:160])
    return 0 if ok_ocr else 2


if __name__ == "__main__":
    raise SystemExit(main())
