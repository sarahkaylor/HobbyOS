import json, os, re, urllib.request

BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"
TOK = os.environ["FORGEJO_TOKEN"]
PAT = re.compile(r"VERDICT[^\n]{0,40}?\b(GREEN|RED)\b", re.I)

for n in (301, 292):
    r = urllib.request.Request(f"{BASE}/issues/{n}/comments",
                               headers={"Authorization": "token " + TOK})
    for c in json.load(urllib.request.urlopen(r)):
        for m in PAT.finditer(c["body"]):
            span = m.group(0).replace("\n", " ")
            print(f"#{n} c{c['id']} {c['created_at']} -> {m.group(1).upper()}")
            print("    match:", repr(span[:120]))