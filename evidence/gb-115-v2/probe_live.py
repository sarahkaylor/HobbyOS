#!/usr/bin/env python3
"""GB-115-V2 clause 3 live replay: GB-115 (#231) is the card whose EVIDENCE comment
quotes a lane-move line.  Run the OLD (cb27a8b) and NEW (8d14d88) detectors over that
card's real comments and compare what they call its lane entry."""
import importlib.util
import json
import os
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

card = api("/issues/231")
gate = api("/issues/233")
comments = api("/issues/231/comments")


def cf(_n):
    return comments


print(f"GB-115 #231 lane labels: {[l['name'] for l in card['labels']]}")
print(f"GB-115-V2 #233 created_at: {gate['created_at']}")
print("\ncomments on #231 that mention 'lane':")
for c in comments:
    body = (c.get("body") or "").replace("\r", "")
    if "lane" in body:
        first = body.splitlines()[0][:110]
        print(f"  {c['created_at']}  lines={len(body.splitlines()):2d} len={len(body):5d}  {first!r}")

old_entry = OLD.lane_entry_time(card, cf)
new_entry = NEW.lane_entry_time(card, cf)
print(f"\nlane_entry_time  OLD -> {old_entry}")
print(f"lane_entry_time  NEW -> {new_entry}")
for gn in (232, 233):
    g = api(f"/issues/{gn}")
    print(f"\ngate #{gn} ({g['title'].split()[0]}) created_at {g['created_at']}")
    print(f"  gate_is_pending  OLD -> {OLD.gate_is_pending(card, g, cf)}")
    print(f"  gate_is_pending  NEW -> {NEW.gate_is_pending(card, g, cf)}")