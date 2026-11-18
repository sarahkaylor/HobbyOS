#!/usr/bin/env python3
"""tools/cp/hunt/compare.py A_LOGDIR B_LOGDIR [--names A B] -- two rounds of
H1-hunt boots side by side (docs/cp-hunt.md "integ15 vs integ17"): per kind
the boots and verdicts, early exits, kernel faults, QEMU start -> launcher
time (first NFS mount), launch -> menu time, fpsprobe world medians, and the
k/n of every classify.py signature (benign Wine/DXVK lines left out).
Markdown on stdout."""
import argparse
import collections
import csv
import glob
import json
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import benign, q10  # noqa: E402


def load(d):
    runs = []
    for f in sorted(glob.glob(os.path.join(d, "*.class.json"))):
        r = json.load(open(f))
        log = os.path.join(d, r["run"] + ".log")
        t = open(log, errors="replace").read() if os.path.exists(log) else ""
        m = re.search(r"kind=(\S+)", t)
        r["kind"] = m.group(1) if m else "?"
        m = re.search(r"\+\s+([0-9.]+) s\s+\S+\s+NFS mount", t)
        r["mount_s"] = float(m.group(1)) if m else None
        fp = os.path.join(d, r["run"] + ".fps.csv")
        if os.path.exists(fp):
            rows = list(csv.DictReader(open(fp)))
            w = [float(x["approx_fps"]) for x in rows if x["phase"] == "world" or x["phase"].startswith("soak")]
            r["world"] = (statistics.median(w), q10(w)) if w else None
        runs.append(r)
    return runs


def med(v):
    v = [x for x in v if x is not None]
    return f"{statistics.median(v):.1f} (n={len(v)}, {min(v):.1f}–{max(v):.1f})" if v else "–"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--names", nargs=2, default=["A", "B"])
    x = ap.parse_args()
    A, B = load(x.a), load(x.b)
    na, nb = x.names
    print(f"| | {na} | {nb} |")
    print("|---|---|---|")
    for lab, fn in (
        ("game boots", lambda R: str(len(R))),
        ("verdicts", lambda R: ", ".join(f"{k}={v}" for k, v in sorted(
            collections.Counter(r["runlog"].get("verdict", "?") for r in R).items()))),
        ("early exits", lambda R: f"{sum(1 for r in R if r['runlog'].get('early_exit'))}/{len(R)}"),
        ("kernel fault rows", lambda R: str(sum(sum(v[0] for v in r["classes"]["kfault"].values()) for r in R))),
        ("QEMU start → launcher (NFS mount) s, median", lambda R: med([r["mount_s"] for r in R])),
        ("boots with that > 60 s", lambda R: f"{sum(1 for r in R if (r['mount_s'] or 0) > 60)}/{len(R)}"),
        ("P8 Venus launch → menu s", lambda R: med([r["runlog"].get("launch_to_menu_s") for r in R if r["kind"] in ("p8v", "win")])),
        ("P8 lavapipe launch → menu s", lambda R: med([r["runlog"].get("launch_to_menu_s") for r in R if r["kind"] == "p8l"])),
        ("fpsprobe 720p world medians", lambda R: ", ".join(f"{r['world'][0]:.1f}" for r in R if r["kind"] == "fp720" and r.get("world")) or "–"),
        ("fpsprobe 720p p10", lambda R: ", ".join(f"{r['world'][1]:.1f}" for r in R if r["kind"] == "fp720" and r.get("world")) or "–"),
        ("fpsprobe 1080p world medians", lambda R: ", ".join(f"{r['world'][0]:.1f}" for r in R if r["kind"] == "fp1080" and r.get("world")) or "–"),
        ("soak world median (p10)", lambda R: ", ".join(f"{r['world'][0]:.1f} ({r['world'][1]:.1f})" for r in R if r["kind"] == "soak" and r.get("world")) or "–"),
    ):
        print(f"| {lab} | {fn(A)} | {fn(B)} |")
    print()
    sig = {}
    for tag, R in (("a", A), ("b", B)):
        for r in R:
            for c in ("kfault", "kstall", "kdiag", "exitpc", "audio", "wine_err", "dxvk"):
                for s_, v in r["classes"].get(c, {}).items():
                    if c in ("wine_err", "dxvk") and benign(s_):
                        continue
                    key = (c, s_.split(":")[0] if c in ("kstall", "kdiag") else s_[:90])
                    e = sig.setdefault(key, {"a": set(), "b": set(), "ra": 0, "rb": 0})
                    e[tag].add(r["run"])
                    e["r" + tag] += v[0]
    print(f"| class | signature | {na} boots (rows) | {nb} boots (rows) |")
    print("|---|---|---|---|")
    for (c, s_), e in sorted(sig.items(), key=lambda kv: (kv[0][0], -len(kv[1]["a"]) - len(kv[1]["b"]))):
        print(f"| {c} | `{s_}` | {len(e['a'])}/{len(A)} ({e['ra']}) | {len(e['b'])}/{len(B)} ({e['rb']}) |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
