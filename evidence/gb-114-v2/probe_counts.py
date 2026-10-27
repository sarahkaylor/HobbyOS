#!/usr/bin/env python3
"""GB-114-V2 clause-5 probe: which verdicts does each reader recognise, on the live
board today?  Read-only.  Prints the count and the per-card list so 50-vs-51 (and the
card's 52) can be located instead of asserted."""
import importlib.util
import json
import os
import re
import sys
import urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOKEN = os.environ.get("FORGEJO_TOKEN", "")


def api(path):
    req = urllib.request.Request(f"{BASE}{path}", headers={"Authorization": f"token {TOKEN}"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OLD = load(sys.argv[1], "groom_old")
NEW = load(sys.argv[2], "groom_new")


def old_verdict_in(body):
    """Pre-GB-114 reader: the inline template-only regex from check_verdicts.py @ 983da99."""
    m = re.search(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", body, re.I)
    return m.group(1).upper() if m else None


def new_verdict_in(body):
    return NEW.verdict_in(body)

issues = []
page = 1
while True:
    batch = api(f"/issues?state=all&limit=50&page={page}")
    if not batch:
        break
    issues += batch
    page += 1

cards = [i for i in issues if (i.get("title") or "").split()[:1] and
         (i["title"].split()[0].startswith("GB-") and "-V" in i["title"].split()[0])]

old_list, new_list = [], []
for it in cards:
    cs = api(f"/issues/{it['number']}/comments")
    o = n = None
    for c in reversed(cs):
        body = (c.get("body") or "").replace("\r", "")
        if o is None and old_verdict_in(body):
            o = old_verdict_in(body)
        if n is None and new_verdict_in(body):
            n = new_verdict_in(body)
    if o:
        old_list.append((it["number"], it["title"].split()[0], o))
    if n:
        new_list.append((it["number"], it["title"].split()[0], n))

print(f"cards scanned: {len(cards)}")
print(f"OLD reader: {len(old_list)} verdicts")
print(f"NEW reader: {len(new_list)} verdicts")
old_nums = {x[0] for x in old_list}
new_nums = {x[0] for x in new_list}
print("\nrecognised ONLY by the new reader:")
for x in new_list:
    if x[0] not in old_nums:
        print("  ", x)
print("recognised ONLY by the old reader:")
for x in old_list:
    if x[0] not in new_nums:
        print("  ", x)