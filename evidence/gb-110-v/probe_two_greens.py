#!/usr/bin/env python3
"""GB-110-V (#212): why board(209) is None even though #210 carries a GREEN that
names the head.  Examine BOTH GREENs in API order (oldest first) and run the
shipped parser over each — the defect is which one the tool stops at.
"""
import importlib.util, json, os, re, urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]
spec = importlib.util.spec_from_file_location("mp", "tools/board/merge_pr.py")
mp = importlib.util.module_from_spec(spec); spec.loader.exec_module(mp)

req = urllib.request.Request(BASE + "/issues/210/comments?limit=100",
                             headers={"Authorization": "token " + TOK})
cs = json.load(urllib.request.urlopen(req))
greens = [c for c in cs if re.search(r"VERDICT[^\n]{0,50}\bGREEN\b", c.get("body") or "", re.I)]
print(f"GREENs on #210: {len(greens)}  (API order = oldest first)\n")

for i, c in enumerate(greens):
    body = c["body"]
    shas = mp.graded_shas_in(body)
    tag = "OLDEST" if i == 0 else ("NEWEST <- the tool stops here" if i == len(greens) - 1 else "")
    print(f"[{i}] {c['created_at']}  {tag}")
    print(f"    graded_shas_in -> {shas if shas else '[]  <-- no sha, so the tool returns None'}")
    for ln in body.splitlines():
        if re.search(r"b3540e5|gate tree|Tree that would merge", ln, re.I):
            print(f"    names: {ln.strip()[:120]}")

print("\nthe loop in graded_sha_from_board():")
print("    for c in reversed(cs):        # newest first")
print("        if VERDICT...GREEN: shas = graded_shas_in(body); return shas[0] if shas else None")
print("  -> it stops at the NEWEST GREEN and gives up, never falling back to the older")
print("     GREEN that does name the tree, and never accepts a 7-char prefix (GB-109's form).")