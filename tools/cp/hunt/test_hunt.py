#!/usr/bin/env python3
"""tools/cp/hunt/test_hunt.py -- golden tests for the H1-hunt harness pieces
(docs/cp-hunt.md).  Run from the repo root: python3 -I tools/cp/hunt/test_hunt.py

1. classify.py on golden/serial-mini.txt + golden/runlog-mini.txt must print
   exactly golden/expected-class.txt (regenerate with --regen after a
   deliberate change, and read the diff).
2. classify.py --aggregate on two copies gives 2/2 for every signature.
3. wavgaps.py (needs numpy; skipped without it) finds the dropout and the gap
   planted in a synthetic WAV and none in a clean one.
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
G = os.path.join("tools", "cp", "hunt", "golden")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(f"[HUNTT] {name}: {'PASS' if ok else 'FAIL'} {detail}")
    fails += 0 if ok else 1


def run(*a):
    return subprocess.run([sys.executable, "-I", *a], cwd=REPO, capture_output=True, text=True)


r = run("tools/cp/hunt/classify.py", "--run", "golden", "--log", f"{G}/runlog-mini.txt", f"{G}/serial-mini.txt")
exp_p = os.path.join(REPO, G, "expected-class.txt")
if "--regen" in sys.argv:
    open(exp_p, "w").write(r.stdout)
exp = open(exp_p).read()
if r.stdout != exp:
    import difflib
    sys.stdout.writelines(difflib.unified_diff(exp.splitlines(True), r.stdout.splitlines(True), "expected", "got"))
check("classify golden", r.stdout == exp and r.returncode == 0)

with tempfile.TemporaryDirectory() as t:
    j = os.path.join(t, "early.json")
    run("tools/cp/hunt/classify.py", "--log", f"{G}/runlog-early.txt", "--json", j, f"{G}/serial-mini.txt")
    j2 = os.path.join(t, "menu.json")
    run("tools/cp/hunt/classify.py", "--log", f"{G}/runlog-mini.txt", "--json", j2, f"{G}/serial-mini.txt")
    e, m = json.load(open(j))["runlog"], json.load(open(j2))["runlog"]
    check("early exit flagged", e.get("early_exit") is True and e.get("game_ms") == 2886 and not m.get("early_exit"),
          f"early={e.get('early_exit')} menu={m.get('early_exit')}")

with tempfile.TemporaryDirectory() as t:
    js = []
    for i in (1, 2):
        j = os.path.join(t, f"b{i}.json")
        run("tools/cp/hunt/classify.py", "--run", f"b{i}", "--json", j, f"{G}/serial-mini.txt")
        js.append(j)
    a = run("tools/cp/hunt/classify.py", "--aggregate", *js).stdout
    rows = [ln for ln in a.splitlines() if ln.strip()[:2].isdigit() or ln.strip()[:1].isdigit()]
    check("aggregate 2/2", rows and all(ln.strip().startswith("2/2") for ln in rows), f"({len(rows)} signatures)")

# round 4 (H9-hunt4): [PFE] is an instrument row of the pf class (not an
# unclassified line), [FREERACE] a kernel fault, the ntsync live-object trend
# (Wine's sync-object count: the handle proxy) and the console's host stalls
r = run("tools/cp/hunt/classify.py", "--run", "r4", f"{G}/serial-r4.txt")
o = r.stdout
check("r4 pf class has [PFE]", "pf: [PFE] kind=7 prot=5 present=0" in o and "pf: [PFK]" in o, "")
check("r4 FREERACE is a kfault", "freerace: [FREERACE] freeing root=0x# while cpu" in o, "")
check("r4 ntsync trend", "-- [NTSYNC] live objects: 3 rows, 847 -> max 951 -> 951" in o, "")
check("r4 console stalls", "-- [CONS] last: stalls=2 stolen_ms=310" in o, "")
check("r4 fence stats", "-- [VENUS] fence stats: 1 rows, errors max 0, post->retire >11000 us max 3" in o, "")
# glasswatch.py report: a planted 400 ms world gap counts, a 3 s map gap does not
with tempfile.TemporaryDirectory() as t:
    g, sfile = os.path.join(t, "glass.txt"), os.path.join(t, "states.txt")
    open(sfile, "w").write("1000.000 world\n1010.000 menu:map\n1020.000 world\n")
    open(g, "w").write("C 999.000 1280x720\nG 1005.400 400 99.0 400\nG 1015.000 3000 2.0 3000\n"
                       "G 1030.120 120 99.0 120\nM 1060.000 1500 1490 3000 33 90\n")
    rr = run("tools/cp/hunt/glasswatch.py", "report", g, "--states", sfile).stdout
    check("r4 glasswatch hitches", "HITCHES (world-like states, gap >= 250 ms): 1; max 400 ms" in rr, rr.splitlines()[-2:])

try:
    import numpy  # noqa: F401
    have_np = True
except ImportError:
    have_np = False
if not have_np:
    print("[HUNTT] wavgaps: SKIP (no numpy)")
else:
    def wav(path, samples, rate=48000):
        b = b"".join(struct.pack("<hh", s, s) for s in samples)
        hdr = b"RIFF" + struct.pack("<I", 36 + len(b)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
        open(path, "wb").write(hdr + b"data" + struct.pack("<I", len(b)) + b)
    rate = 48000
    tone = [int(8000 * math.sin(2 * math.pi * 440 * i / rate)) or 1 for i in range(rate * 3)]
    bad = tone[:rate] + [0] * (rate // 50) + tone[:rate] + [0] * rate + tone[:rate]   # 20 ms drop, 1 s gap
    with tempfile.TemporaryDirectory() as t:
        wav(os.path.join(t, "clean.wav"), tone)
        wav(os.path.join(t, "bad.wav"), bad)
        c = json.loads(run("tools/cp/hunt/wavgaps.py", "--json", os.path.join(t, "clean.wav")).stdout)
        d = json.loads(run("tools/cp/hunt/wavgaps.py", "--json", os.path.join(t, "bad.wav")).stdout)
        check("wavgaps clean", c["dropouts"] == 0 and c["gaps_ge_500ms"] == 0, f"{c['dropouts']} drops {c['gaps_ge_500ms']} gaps")
        # round 4: shotcheck reads shots past s999, in number order
        from PIL import Image
        sd = os.path.join(t, "run", "shots")
        os.makedirs(sd)
        for n in (998, 999, 1000, 1001):
            Image.new("RGB", (1280, 720), (40 + n % 7, 90, 60)).save(os.path.join(sd, f"s{n:03d}-t{n:06d}-rec.png"))
        sc = json.loads(run("tools/cp/hunt/shotcheck.py", "--json", os.path.join(t, "run")).stdout)
        sc = sc[0] if isinstance(sc, list) else sc
        check("shotcheck past s999", sc.get("shots") == 4, f"shots={sc.get('shots')}")
        check("wavgaps planted", d["dropouts"] == 1 and abs(d["dropouts_first"][0][1] - 20) < 1 and d["gaps_ge_500ms"] == 1,
              f"{d['dropouts']} drops {d.get('dropouts_first')} {d['gaps_ge_500ms']} gaps {d.get('gaps_first')}")
print(f"[HUNTT] done: {fails} failed")
sys.exit(1 if fails else 0)
