import importlib.util, json, os, urllib.request

SEL = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb136v3/tools/board/check_verdicts.py"
spec = importlib.util.spec_from_file_location("cv", SEL)
cv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cv)

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]

def comments(n):
    r = urllib.request.Request(f"{BASE}/issues/{n}/comments",
                               headers={"Authorization": "token " + TOK})
    return json.load(urllib.request.urlopen(r))

for n, label in ((44, "GB-044"), (136, "GB-085"), (159, "GB-098"), (197, "GB-105")):
    print(f"=== {label} (#{n}) — every verdict-shaped comment, oldest first ===")
    for c in sorted(comments(n), key=lambda x: x["created_at"]):
        v, line = cv.verdict_line_of(c["body"])
        if v:
            print(f"  {c['created_at']} c{c['id']} -> {v}")
            print(f"      {line[:110]}")