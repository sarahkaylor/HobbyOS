"""GB-139-V clause 3: the deciding verdict line for each changed card, from the
graded tree's own parser, over every comment on the card."""
import importlib.util
import json
import os
import urllib.request

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb139v/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]

PARENTS = {44: "GB-044", 62: "GB-060", 74: "GB-066", 136: "GB-085",
           159: "GB-098", 161: "GB-099", 197: "GB-105", 200: "GB-106"}


def comments(n):
    r = urllib.request.Request(f"{BASE}/issues/{n}/comments",
                               headers={"Authorization": "token " + TOK})
    return json.load(urllib.request.urlopen(r))


for n, label in PARENTS.items():
    newest = None
    for c in comments(n):
        v, line = cv.verdict_line_of(c["body"])
        if v and (newest is None or c["created_at"] > newest[0]):
            newest = (c["created_at"], c["id"], v, line)
    if newest:
        print(f"{label} (#{n}): {newest[2]} at {newest[0]} c{newest[1]}")
        print(f"    deciding line: {newest[3][:130]}")
    else:
        print(f"{label} (#{n}): no verdict-shaped line at all on the graded tree")