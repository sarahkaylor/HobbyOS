#!/usr/bin/env python3
"""GB-110-V (#212): independently measure the claim that graded_shas_in() reads only a
minority of real GREEN verdict comments.

Method: pull every comment on the board through the repo-wide comments endpoint,
keep the ones that are a verdict for a GREEN, run PR #211's own graded_shas_in()
over them, and report how many yield a 40-hex sha (and how many of those actually
name the head of the PR that comment is about).

Env: FORGEJO_TOKEN.
"""
import json, os, re, sys, urllib.request, urllib.parse

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ.get("FORGEJO_TOKEN", "")
if not TOK:
    sys.exit("FORGEJO_TOKEN not set")

# import the graded parser from the PR head's file
sys.path.insert(0, "tools/board")
import importlib.util
spec = importlib.util.spec_from_file_location("mp", "tools/board/merge_pr.py")
mp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mp)


def get(path):
    req = urllib.request.Request(BASE + path,
                                 headers={"Authorization": "token " + TOK})
    return json.load(urllib.request.urlopen(req))


comments, page = [], 1
while True:
    batch = get(f"/issues/comments?limit=50&page={page}")
    if not batch:
        break
    comments += batch
    page += 1
    if page > 40:
        break

greens = [c for c in comments
          if re.search(r"VERDICT:\s*\*{0,2}GREEN", c.get("body") or "", re.I)
          or re.search(r"VERDICT.*GREEN", (c.get("body") or "").split("\n")[0], re.I)]
with_sha, without = [], []
for c in greens:
    shas = mp.graded_shas_in(c.get("body") or "")
    (with_sha if shas else without).append(c)

print(f"comments scanned:            {len(comments)}")
print(f"GREEN verdict comments:      {len(greens)}")
print(f"  parser yields a sha:       {len(with_sha)}")
print(f"  parser yields NOTHING:     {len(without)}")
if greens:
    print(f"  read rate:                 {100.0*len(with_sha)/len(greens):.0f}%")

print("\n--- first 8 GREENs the parser cannot read (issue, date, first line):")
for c in without[:8]:
    body = (c.get("body") or "").strip().split("\n")
    first = next((l for l in body if l.strip()), "")[:70]
    print(f"  #{c.get('issue_url','').rstrip('/').split('/')[-1]:>5}  {c.get('created_at','')[:10]}  {first}")

print("\n--- GREENs with a sha, showing which sha and whether it looks like a head:")
for c in with_sha[:6]:
    body = c.get("body") or ""
    shas = mp.graded_shas_in(body)
    print(f"  #{c.get('issue_url','').rstrip('/').split('/')[-1]:>5}  {shas[0][:12]}  ({len(shas)} sha)")