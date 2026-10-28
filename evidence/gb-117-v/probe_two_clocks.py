#!/usr/bin/env python3
"""GB-117-V: the same board, two clocks — DoD-literal ("newest comment") vs the shipped tool's
("newest comment whose body does not contain 'Coordinator'").

Prints the bucket distribution and stale/fresh split under both clocks so the size of the deviation is
a number, not an argument. GETs only.
"""
import json, os, urllib.request
from datetime import datetime, timezone

TOKEN = os.environ["FORGEJO_TOKEN"]
BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"


def api(path):
    r = urllib.request.Request(f"{BASE}{path}", headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(r, timeout=30) as resp:
        return json.load(resp)


def parse(ts):
    return datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def bucket(age):
    if age is None:
        return "nocomments"
    if age < 6:
        return "under6h"
    if age < 12:
        return "6-12h"
    if age < 24:
        return "12-24h"
    return "over24h"


now = datetime.now(timezone.utc)
issues, page = [], 1
while True:
    batch = api(f"/issues?state=open&limit=50&page={page}")
    if not batch:
        break
    issues += batch
    page += 1

res = {"literal": {}, "shipped": {}}
for key in res:
    res[key] = {"under6h": 0, "6-12h": 0, "12-24h": 0, "over24h": 0, "nocomments": 0,
                "stale": 0, "fresh": 0}

for it in issues:
    cs = api(f"/issues/{it['number']}/comments")
    for key, filt in (("literal", lambda c: True),
                      ("shipped", lambda c: "Coordinator" not in (c.get("body") or ""))):
        rows = [c for c in cs if filt(c)]
        age = None if not rows else max(0.0, (now - parse(rows[-1]["created_at"])).total_seconds() / 3600.0)
        b = bucket(age)
        res[key][b] += 1
        if age is None or age >= 24:
            res[key]["stale"] += 1
        else:
            res[key]["fresh"] += 1

print(f"open cards: {len(issues)}")
for key in ("literal", "shipped"):
    r = res[key]
    print(f"{key:8s} buckets: under6h={r['under6h']} 6-12h={r['6-12h']} 12-24h={r['12-24h']} "
          f"over24h={r['over24h']} nocomments={r['nocomments']}  -> split={r['stale']}/{r['fresh']}")
print()
print(f"deviation moves over24h by {res['shipped']['over24h'] - res['literal']['over24h']:+d} cards and "
      f"stale by {res['shipped']['stale'] - res['literal']['stale']:+d}")