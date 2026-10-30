"""GB-139-V clause 6: census of every verdict-ish line shape on the live board.

Pulls every comment on every issue, finds every line that contains a GREEN or RED
token as a word, normalises it to a shape skeleton, and asks the graded tree's
parser what it makes of the line.  Reports coverage by distinct shape and by
occurrence, so a shape that appears once cannot look like coverage.
"""
import importlib.util
import json
import os
import re
import urllib.request

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb139v/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]
CACHE = "/home/sarah/.hermes/cache/scratch/board_comments.json"


def api(path):
    req = urllib.request.Request(BASE + path, headers={"Authorization": "token " + TOK})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


if os.path.exists(CACHE):
    bodies = json.load(open(CACHE))
    print(f"(using cached board: {len(bodies)} comments)")
else:
    issues = []
    for page in range(1, 9):
        batch = api(f"/issues?state=all&limit=50&page={page}")
        if not batch:
            break
        issues += batch
    bodies = []
    for it in issues:
        for c in api(f"/issues/{it['number']}/comments"):
            bodies.append(c["body"])
    json.dump(bodies, open(CACHE, "w"))
    print(f"fetched {len(issues)} issues, {len(bodies)} comments")

TOKEN = re.compile(r"\b(GREEN|RED)\b", re.I)


def skeleton(line):
    s = line.strip()
    s = TOKEN.sub("<V>", s)
    s = re.sub(r"`[^`]*`", "<code>", s)
    s = re.sub(r"[0-9a-f]{7,40}", "<sha>", s)
    s = re.sub(r"GB-\d+(?:-V\d*)?", "<GB>", s)
    s = re.sub(r"#\d+", "<num>", s)
    s = re.sub(r"\s+", " ", s)
    return s[:70]


shapes = {}
for body in bodies:
    for line in (body or "").replace("\r", "").split("\n"):
        if not TOKEN.search(line):
            continue
        got, _ = cv.verdict_line_of(line)
        key = skeleton(line)
        rec = shapes.setdefault(key, {"n": 0, "parsed": 0, "verdicts": set(), "sample": line.strip()[:110]})
        rec["n"] += 1
        if got:
            rec["parsed"] += 1
            rec["verdicts"].add(got)

total_lines = sum(r["n"] for r in shapes.values())
parsed_lines = sum(r["parsed"] for r in shapes.values())
multi = {k: v for k, v in shapes.items() if v["n"] >= 2}
multi_parsed = sum(1 for v in multi.values() if v["parsed"])
print(f"\ndistinct shapes: {len(shapes)}  (n>=2: {len(multi)})")
print(f"lines considered: {total_lines}   parsed as a verdict: {parsed_lines} "
      f"({100.0*parsed_lines/max(total_lines,1):.1f}%)")
print(f"shapes with n>=2 that parse: {multi_parsed}/{len(multi)}")

print("\n--- every shape with n>=2, unparsed (candidates for the eighth gate) ---")
for k, v in sorted(multi.items(), key=lambda x: -x[1]["n"]):
    if not v["parsed"]:
        print(f"  n={v['n']:3d}  {k}")
        print(f"         e.g. {v['sample']}")

print("\n--- top parsed shapes (the ones the parser covers) ---")
for k, v in sorted([x for x in multi.items() if x[1]["parsed"]], key=lambda x: -x[1]["n"])[:10]:
    print(f"  n={v['n']:3d}  {k}  -> {','.join(sorted(v['verdicts']))}")