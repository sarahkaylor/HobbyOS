#!/usr/bin/env python3
"""tools/cp/hunt/mine_report.py MINE.json [--since YYYY-MM-DDTHH:MM] -- the old
game boots on strix (classify.py --mine output, already deduplicated by
content hash) by signature: k/n game boots, the boots (tree/run), the newest
serial-log mtime (last seen).  Kernel faults / stalls / EXITPC codes / the
REDEngineErrorReporter rows and Wine err lines tagged "new"; the pure-noise
rows (fence_wait2 timeouts, NFSPIPE resend stalls) are counted per boot only.
"""
import datetime
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import benign  # noqa: E402

a = sys.argv[1:]
since = None
if "--since" in a:
    i = a.index("--since")
    since = datetime.datetime.fromisoformat(a[i + 1]).timestamp()
    del a[i:i + 2]
runs = [r for r in json.load(open(a[0])) if r.get("game")]
for r in runs:
    try:
        r["mtime"] = os.path.getmtime(r["run"])
    except OSError:
        r["mtime"] = 0
    p = r["run"].replace(os.path.expanduser("~/cp/"), "")
    p = p.replace("/obj/cp-p8/", "/").replace("/obj/cp-p9/", "/").replace("/serial.txt", "").replace("/serial.log", "")
    r["short"] = p
if since:
    runs = [r for r in runs if r["mtime"] >= since]
n = len(runs)
print(f"== {n} game boots{' since ' + a[a.index('--since') + 1] if False else ''}"
      f"{(' with serial mtime >= ' + datetime.datetime.fromtimestamp(since).isoformat()) if since else ''}")
verd = {}
for r in runs:
    verd[r["runlog"].get("verdict", "?")] = verd.get(r["runlog"].get("verdict", "?"), 0) + 1
print("   verdicts: " + ", ".join(f"{k}={v}" for k, v in sorted(verd.items(), key=lambda kv: -kv[1])))
agg = {}
for r in runs:
    for c in ("kfault", "kstall", "kdiag", "exitpc", "audio", "red", "wine_err", "dxvk"):
        for sig, v in r["classes"].get(c, {}).items():
            if c in ("wine_err", "dxvk") and benign(sig):
                continue
            key = (c, sig)
            e = agg.setdefault(key, {"boots": [], "rows": 0, "last": 0})
            e["boots"].append(r["short"])
            e["rows"] += v[0]
            e["last"] = max(e["last"], r["mtime"])
for c in ("kfault", "kstall", "kdiag", "exitpc", "audio", "red", "wine_err", "dxvk"):
    items = sorted(((k, v) for k, v in agg.items() if k[0] == c), key=lambda kv: (-len(kv[1]["boots"]), -kv[1]["rows"]))
    if not items:
        continue
    print(f"-- {c}")
    for (c_, sig), v in items[:40]:
        last = datetime.datetime.fromtimestamp(v["last"]).strftime("%m-%d %H:%M")
        b = sorted(set(v["boots"]))
        print(f"   {len(b):3d}/{n} rows={v['rows']:<5d} last={last}  {sig[:110]}")
        if len(b) <= 8:
            print(f"            {' '.join(b)}")
