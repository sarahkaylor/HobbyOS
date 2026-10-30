"""GB-005-V2 clause 1 (ALLOW arm) + clause 6 on a scratch board.

Production cards are not test fixtures: the ALLOW arm moves a card and the force
path posts a permanent record.  This builds sarah/gaming-program-vgate with three
synthetic cards -- GREEN gate, RED gate, no gate -- and exercises the real
lane-move.sh against it via BOARD_REPO, so the enforcement code path is the one
under test rather than a mock.
"""
import json
import os
import subprocess
import urllib.request

URL = os.environ.get("FORGEJO_URL", "http://192.168.0.234:3000").rstrip("/")
TOK = os.environ["FORGEJO_TOKEN"]
REPO = "sarah/gaming-program-vgate"
ROOT = "/home/sarah/Documents/GitHub/HobbyOS/.worktrees/gb005v2"


def api(path, method="GET", body=None):
    req = urllib.request.Request(URL + "/api/v1" + path, method=method,
        data=json.dumps(body).encode() if body is not None else None)
    req.add_header("Authorization", "token " + TOK)
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


# 1. repo
try:
    api(f"/repos/{REPO}")
    print("repo exists:", REPO)
except Exception:
    api("/user/repos", "POST", {"name": "gaming-program-vgate", "auto_init": True,
                                "private": True, "description": "GB-005-V2 scratch board"})
    print("repo created:", REPO)

# 2. labels
have = {l["name"] for l in api(f"/repos/{REPO}/labels?limit=50")}
for name, color in [("Merge Gate", "#0052cc"), ("In Progress", "#fbca04"),
                    ("Ready", "#0e8a16")]:
    if name not in have:
        api(f"/repos/{REPO}/labels", "POST", {"name": name, "color": color})
print("labels:", sorted(l["name"] for l in api(f"/repos/{REPO}/labels?limit=50")))

# 3. cards
existing = {i["title"] for i in api(f"/repos/{REPO}/issues?state=all&limit=50")}
cards = [
    ("GB-900 scratch card with a GREEN gate", "In Progress"),
    ("GB-901 scratch card with a RED gate", "In Progress"),
    ("GB-902 scratch card with no gate", "In Progress"),
]
nums = {}
for title, lane in cards:
    if title not in existing:
        it = api(f"/repos/{REPO}/issues", "POST", {"title": title, "body": "synthetic GB-005-V2 fixture"})
        api(f"/repos/{REPO}/issues/{it['number']}/labels", "POST",
            {"labels": [l["id"] for l in api(f"/repos/{REPO}/labels?limit=50") if l["name"] == lane]})
    nums[title.split()[0]] = next(i["number"] for i in api(f"/repos/{REPO}/issues?state=all&limit=50")
                                  if i["title"] == title)

gates = [
    ("GB-900-V Gate for GB-900 (synthetic)", "VERDICT: GREEN - synthetic fixture for GB-005-V2 clause 1"),
    ("GB-901-V Gate for GB-901 (synthetic)", "VERDICT: RED - synthetic fixture for GB-005-V2 clause 1"),
]
for title, body in gates:
    if title not in existing:
        g = api(f"/repos/{REPO}/issues", "POST", {"title": title, "body": "synthetic gate"})
        api(f"/repos/{REPO}/issues/{g['number']}/comments", "POST", {"body": body})
    else:
        g = next(i for i in api(f"/repos/{REPO}/issues?state=all&limit=50") if i["title"] == title)
        cs = api(f"/repos/{REPO}/issues/{g['number']}/comments")
        if not any("VERDICT" in c["body"] for c in cs):
            api(f"/repos/{REPO}/issues/{g['number']}/comments", "POST", {"body": body})
print("cards:", nums)

env = dict(os.environ, BOARD_REPO=REPO, BOARD_AGENT="wgames-verify/gate(clause1-test)")
runs = [
    ("ALLOW  GB-900 (newest -V GREEN)", ["GB-900", "Merge Gate"]),
    ("REFUSE GB-901 (newest -V RED)", ["GB-901", "Merge Gate"]),
    ("REFUSE GB-902 (no -V at all)", ["GB-902", "Merge Gate"]),
]
for label, args in runs:
    p = subprocess.run(["bash", f"{ROOT}/tools/board/lane-move.sh"] + args,
                       capture_output=True, text=True, env=env)
    out = (p.stdout + p.stderr).strip()
    print(f"\n$ BOARD_REPO={REPO} lane-move.sh {' '.join(args)}\n[{label}] exit={p.returncode}\n{out}")

# 4. force path on a card that the plain path refuses
p = subprocess.run(["bash", f"{ROOT}/tools/board/lane-move.sh", "GB-902", "Merge Gate",
                    "--force", "GB-005-V2 clause 6: verify the bypass is attributable"],
                   capture_output=True, text=True, env=env)
print(f"\n$ lane-move.sh GB-902 \"Merge Gate\" --force ...\nexit={p.returncode}\n" + (p.stdout + p.stderr).strip())

# 5. read the record back from the API, not from the script's stdout
for c in api(f"/repos/{REPO}/issues/{nums['GB-902']}/comments"):
    if "MERGE-GATE FORCE" in c["body"]:
        print("\nposted record read back from the API:")
        print("  id:", c["id"], "at", c["created_at"])
        print("  " + c["body"].replace("\n", "\n  "))