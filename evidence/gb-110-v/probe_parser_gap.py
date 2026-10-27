#!/usr/bin/env python3
"""GB-110-V (#212) — why a real GREEN that names the head is unreadable.

Takes the actual GREEN verdict body from ticket #210 (GB-109-V round 2), which
names PR #209's head `b3540e5`, and runs PR #211's own graded_shas_in() over it.
"""
import importlib.util, json, os, re, sys, urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]

spec = importlib.util.spec_from_file_location("mp", "tools/board/merge_pr.py")
mp = importlib.util.module_from_spec(spec); spec.loader.exec_module(mp)

req = urllib.request.Request(BASE + "/issues/210/comments?limit=100",
                             headers={"Authorization": "token " + TOK})
cs = json.load(urllib.request.urlopen(req))
green = [c for c in cs if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", c.get("body") or "", re.I)]
print(f"GREEN verdicts on #210: {len(green)}")
body = green[0]["body"]
print(f"newest GREEN at: {green[0]['created_at']}")

print("\n--- the line that names PR #209's head:")
for ln in body.splitlines():
    if "b3540e5" in ln:
        print("   " + ln.strip()[:160])

shas = mp.graded_shas_in(body)
print(f"\ngraded_shas_in(body) -> {shas}   (len={len(shas)})")

print("\n--- the parser's own regex, against the same line:")
for ln in body.splitlines():
    if "b3540e5" in ln:
        print("   40-hex matches on this line:", re.findall(r"\b[0-9a-f]{40}\b", ln))
        print("   the sha as written:", re.findall(r"\b[0-9a-f]{7,40}\b", ln))

print("\n--- a GREEN naming the same head as a 40-hex sha WOULD parse:")
synth = "VERDICT: GREEN — graded head b3540e58569fbbd1ac9de77680e8dcbfb78d9486"
print("  ", mp.graded_shas_in(synth))
no_kw = "Gate tree ba15b4e = origin/main 4608abe x PR #209 b3540e58569fbbd1ac9de77680e8dcbfb78d9486"
print("   40-hex sha on a NON-keyword line:", mp.graded_shas_in(no_kw))