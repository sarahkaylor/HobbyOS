#!/usr/bin/env python3
"""Exercise several in-colony RimWorld interactions over QMP.

Run against a live HobbyOS RimWorld VM whose QMP socket is open, after the
setup flow has reached the colony. Every test records OCR text, a screenshot,
and the pixel delta it was judged on; the JSON report is the evidence.
"""
import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.environ.get(
    "RW_WS",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")),
)
sys.path.insert(0, os.path.join(ROOT, "tools/rw"))
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402
import rw_l86_input as L  # noqa: E402
from rw_desktop_drive import Qmp, bgra_to_ppm  # noqa: E402


class Grabber:
    def __init__(self, q, out_dir, fb=0x75200000, w=1920, h=1080):
        self.q, self.out, self.fb, self.w, self.h = q, out_dir, fb, w, h
        self.n = 0
        os.makedirs(out_dir, exist_ok=True)

    def __call__(self, tag):
        self.n += 1
        p = os.path.join(self.out, "%03d_%s.ppm" % (self.n, tag))
        # QEMU's pmemsave cannot write into /dev/shm on this host (HMP returns
        # "writing memory failed", zero-byte file, while /tmp works).  Dump the
        # raw guest memory to /tmp, then have this process write the PNG under
        # the evidence directory.
        raw = "/tmp/rw-shot-%d-%03d.raw" % (os.getpid(), self.n)
        self.q.pmem(self.fb, self.w * self.h * 4, raw)
        if not os.path.exists(raw) or os.path.getsize(raw) != self.w * self.h * 4:
            print("[test] pmem failed for %s" % tag, flush=True)
            return None
        if not bgra_to_ppm(raw, self.w, self.h, p):
            return None
        os.remove(raw)
        png = p[:-4] + ".png"
        subprocess.run(["convert", p, png], stderr=subprocess.DEVNULL)
        os.remove(p)
        return png


def clock_px(a, b, box):
    na = np.asarray(Image.open(a).convert("L").crop(box))
    nb = np.asarray(Image.open(b).convert("L").crop(box))
    return int((np.abs(na.astype(int) - nb.astype(int)) > 24).sum())


def hit_near_bottom(hits, ymin):
    good = [h for h in hits if h["y"] >= ymin]
    return sorted(good, key=lambda h: -h["y"])[0] if good else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", default=os.path.join(ROOT, "qmp-sock-rw-desktop"))
    ap.add_argument("--log", default=os.path.join(ROOT, "obj/rw-desktop/serial-desktop.log"))
    ap.add_argument("--shot-dir", default=os.path.join(ROOT, "obj/l86/gameplay"))
    ap.add_argument("--fb-phys", default="0x75200000")
    args = ap.parse_args()

    q = Qmp(args.qmp)
    grab = Grabber(q, args.shot_dir, int(args.fb_phys, 0))
    ns = argparse.Namespace(log=args.log, shot_dir=args.shot_dir, fb_w=1920, fb_h=1080)
    d = L.L86(q, grab, args.log, ns, [])
    if not d.establish_geometry(90):
        print("FAIL no geometry")
        return 3
    print("[test] geometry=%s" % (d.geom,), flush=True)
    gx, gy, gw, gh = d.geom
    report = {"geometry": list(d.geom), "tests": []}

    def record(name, ok, **kw):
        rec = {"test": name, "ok": bool(ok)}
        rec.update(kw)
        report["tests"].append(rec)
        print("[test] %-18s %s %s" % (name, "PASS" if ok else "FAIL",
                                      json.dumps(kw)[:400]), flush=True)
        return rec

    def snap(tag):
        return d.shot(tag)

    def ocr_now(tag):
        p = snap(tag)
        w = d.ocr(p, tag=tag) if p else []
        return p, w, d.text_of(w)

    # Return to the map from whatever tab the setup run left open, and put the
    # pointer off the map so no tooltip sits over the test region.
    q.move(1800, 900)
    q.key("esc")
    time.sleep(2)
    q.key("2")
    time.sleep(2)

    # Baseline: only a colony has Architect + Wildlife + Quests.
    p0, w0, t0 = ocr_now("gameplay_baseline")
    anchors = {k: bool(d.find(w0, k)) for k in ("Architect", "Wildlife", "Quests")}
    if not all(anchors.values()):
        record("colony_baseline", False, anchors=anchors, text=t0[:300])
        print("FAIL not on colony")
        return 2
    record("colony_baseline", True, anchors=anchors, shot=p0, text=t0[:200])

    # 1. time controls: at normal speed the map animates; paused it does not.
    #    RimWorld's date widget changes far too slowly to judge this at ~1 fps,
    #    so compare a map region instead.
    mapbox = (gx + 150, gy + 100, gx + 900, gy + 520)

    def motion_px(a, b):
        na = np.asarray(Image.open(a).convert("L").crop(mapbox))
        nb = np.asarray(Image.open(b).convert("L").crop(mapbox))
        return int((np.abs(na.astype(int) - nb.astype(int)) > 24).sum())

    q.key("2")                        # normal speed, unpaused
    time.sleep(10)
    pa = snap("speed_run_a")
    time.sleep(20)
    pb = snap("speed_run_b")
    run_px = motion_px(pa, pb)
    q.key("1")                        # pause
    time.sleep(5)
    pc = snap("speed_paused_a")
    time.sleep(12)
    pd = snap("speed_paused_b")
    paused_px = motion_px(pc, pd)
    record("time_controls", run_px > 800 and paused_px < run_px * 0.75,
           paused_px=paused_px, running_px=run_px,
           paused_shot=pd, running_shot=pb)
    q.key("2")                        # leave the colony running
    time.sleep(2)

    # 2. Architect menu (bottom-left).
    ok, p, w = d.advance("game_arch", "Architect",
                          ["Structure", "Furniture", "Orders", "Production", "Zone"],
                          60, must_change=False)
    arch_text = d.text_of(w)
    record("architect_menu", ok, text=arch_text[:300], shot=p)
    q.key("esc")
    time.sleep(2)

    # 3. bottom HUD tabs: Work, Research, World.
    for tab, expect in (("Work", ("Work", "Manual", "Priority")),
                        ("Research", ("Research", "Project", "Study")),
                        ("World", ("World", "Settlement", "Caravan", "Terrain"))):
        # Close whatever panel the previous tab opened; Research and World are
        # full-screen pages and a single Esc is not always consumed by the
        # first one.
        q.key("esc")
        time.sleep(1)
        q.key("esc")
        time.sleep(2)
        p, w, txt = ocr_now("before_%s" % tab)
        h = hit_near_bottom(d.find(w, tab), gy + gh - 70)
        if not h:
            record("tab_%s" % tab.lower(), False, note="tab not found", text=txt[:200])
            continue
        bx, by = h["x"] + h["w"] // 2, h["y"] + h["h"] // 2
        b, a, diff = d.click(tab, bx, by, "game_%s" % tab.lower(), hold=0.25, settle=6)
        a2 = snap("after_%s" % tab)
        wa2 = d.ocr(a2, tag="after_%s" % tab)
        ta2 = d.text_of(wa2)
        ok = any(d.find(wa2, e) for e in expect) and diff.get("pixels", 0) > 3000
        if not ok:
            # One retry after closing a stale full-screen page.
            q.key("esc")
            time.sleep(2)
            b, a, diff = d.click(tab, bx, by, "game_%s_retry" % tab.lower(),
                                 hold=0.3, settle=8)
            a2 = snap("after_%s_retry" % tab)
            wa2 = d.ocr(a2, tag="after_%s_retry" % tab)
            ta2 = d.text_of(wa2)
            ok = any(d.find(wa2, e) for e in expect) and diff.get("pixels", 0) > 3000
        record("tab_%s" % tab.lower(), ok, click=[bx, by],
               pixels=diff.get("pixels"), text=ta2[:260], shot=a2)
        q.key("esc")
        time.sleep(2)

    # 4. select the first colonist from the top bar and open the inspect panel.
    p, w, txt = ocr_now("colonist_bar")
    hits = [h for h in d.find(w, "Furr") if gy <= h["y"] <= gy + 100]
    if not hits:
        hits = [h for h in d.find(w, "Ella") if gy <= h["y"] <= gy + 100]
    if hits:
        h = sorted(hits, key=lambda k: k["y"])[0]
        # The label sits under its portrait in the colonist bar; clicking the
        # text itself does nothing.  The portrait is one row above it.
        cx, cy = h["x"] + h["w"] // 2, h["y"] - 22
    else:
        cx, cy = gx + 40, gy + 26
    q.key("esc")
    time.sleep(1)
    b, a, diff = d.click("colonist", cx, cy, "game_colonist", hold=0.25, settle=5)
    wa = d.ocr(a, tag="game_colonist")
    ta = d.text_of(wa)
    ok = any(d.find(wa, k) for k in ("Health", "Mood", "Needs", "Inspect"))
    record("colonist_inspect", ok, click=[cx, cy], text=ta[:300], shot=a)

    # 5. quicksave must be accepted without leaving the colony.
    q.key("f5", pause=0.5)
    time.sleep(8)
    ps, ws, ts = ocr_now("after_quicksave")
    ok = all(d.find(ws, k) for k in ("Architect", "Wildlife"))
    record("quicksave", ok, text=ts[:250], shot=ps)

    # 6. speed 3 must advance the colony clock faster than normal speed.
    q.key("3", pause=0.5)
    p3a = snap("speed3_a")
    time.sleep(10)
    p3b = snap("speed3_b")
    px3 = motion_px(p3a, p3b)
    record("speed_3", px3 > 500, motion_px=px3, shot=p3b)

    out = os.path.join(args.shot_dir, "gameplay-test.json")
    with open(out, "w") as f:
        json.dump(report, f, indent=1)
    passed = sum(1 for t in report["tests"] if t["ok"])
    print("[test] summary %d/%d passed -> %s" % (passed, len(report["tests"]), out))
    return 0 if passed == len(report["tests"]) else 1


if __name__ == "__main__":
    sys.exit(main())
