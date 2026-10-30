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

for n in (44, 62, 74, 136, 159, 161, 197, 200):
    best = None
    for c in comments(n):
        v, line = cv.verdict_line_of(c["body"])
        if v and (best is None or c["created_at"] > best[0]):
            best = (c["created_at"], c["id"], v, line)
    if best:
        print(f"#{n}: newest verdict-shaped {best[2]} at {best[0]} (c{best[1]})")
        print("     ", best[3][:130])