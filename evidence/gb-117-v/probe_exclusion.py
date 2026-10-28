#!/usr/bin/env python3
"""GB-117-V: does the "non-coordinator comment" exclusion change any live verdict?

`newest_comment_age()` drops comments whose BODY contains the substring "Coordinator".  The DoD says
"the age of its newest comment".  This probe reads the live board (GETs only) and reports every card
where the newest comment's body contains "Coordinator", with the age with and without the exclusion,
and whether the STALE verdict flips.  If nothing flips, the deviation is real in the code but inert on
today's board; if something flips, it is named here with its id.
"""
import json, os, sys, urllib.request
from datetime import datetime, timezone

TOKEN = os.environ["FORGEJO_TOKEN"]
BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"


def api(path):
    r = urllib.request.Request(f"{BASE}{path}", headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(r, timeout=30) as resp:
        return json.load(resp)


def parse(ts):
    return datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


now = datetime.now(timezone.utc)
issues, page = [], 1
while True:
    batch = api(f"/issues?state=open&limit=50&page={page}")
    if not batch:
        break
    issues += batch
    page += 1

flips, hits = [], []
for it in issues:
    cs = api(f"/issues/{it['number']}/comments")
    if not cs:
        continue
    newest = cs[-1]
    body = newest.get("body") or ""
    all_age = (now - parse(newest["created_at"])).total_seconds() / 3600.0
    if "Coordinator" in body:
        filtered = [c for c in cs if "Coordinator" not in (c.get("body") or "")]
        filt_age = ((now - parse(filtered[-1]["created_at"])).total_seconds() / 3600.0
                    if filtered else None)
        rec = (it["number"], round(all_age, 1), None if filt_age is None else round(filt_age, 1),
               body.splitlines()[0][:70] if body else "")
        hits.append(rec)
        stale_all = all_age >= 24
        stale_filt = filt_age is None or filt_age >= 24
        if stale_all != stale_filt:
            flips.append(rec)

print(f"open cards scanned: {len(issues)}")
print(f"cards whose NEWEST comment contains 'Coordinator': {len(hits)}")
for h in hits:
    print(f"  #{h[0]:<4} newest={h[1]:>6.1f}h  excluded-clock={h[2] if h[2] is not None else 'none':>6}h"
          f"  | {h[3]}")
print(f"STALE-verdict flips caused by the exclusion: {len(flips)}")
for f in flips:
    print(f"  FLIP #{f[0]} newest={f[1]}h -> excluded-clock={f[2]}")
sys.exit(0)