#!/usr/bin/env python3
"""GB-112-V replay differential on REAL board records (read-only; every call is a GET).

Clause 1 asks the patched lint to show GB-047 no longer flagged as `unverified-in-review`.  That
cannot be shown live any more: GB-047 was bounced to In Progress at 10:46:34Z when GB-047-V (#223)
RED'd, so no run of either version can flag it.  This harness therefore pulls the real issues
(`state=all`, matching board.py's own `all_issues()`) and real comments from the live Forgejo API
once, and evaluates BOTH versions of groom.verify_lint_findings() on three states:

  A. the board as it is now                      -> live differential (must be identical)
  B. same records, #47's lane set back to In Review -> the false-positive case (10:45Z state)
  C. same records as B, but with the gate's verdict visible -> does the patch still stay silent
     when the minted-after gate has ALREADY posted a non-GREEN verdict?

usage: replay_lint.py <unpatched_repo> <patched_repo>
"""
import importlib.util, json, os, sys, urllib.request

TOKEN = os.environ["FORGEJO_TOKEN"]
BASE = "http://192.168.0.234:3000/api/v1/repos/sarah/gaming-program"


def get(path):
    r = urllib.request.Request(BASE + path, headers={"Authorization": "token " + TOKEN})
    with urllib.request.urlopen(r) as resp:
        return json.load(resp)


def load_groom(repo):
    p = os.path.join(repo, "tools", "board", "groom.py")
    spec = importlib.util.spec_from_file_location("groom_" + str(abs(hash(p))), p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def fetch_board():
    issues, page = [], 1
    while True:
        batch = get(f"/issues?state=all&type=issues&limit=50&page={page}")
        issues.extend(batch)
        if len(batch) < 50:
            break
        page += 1
    cache = {}

    def comments(n):
        if n not in cache:
            cache[n] = get(f"/issues/{n}/comments?limit=100")
        return cache[n]

    return issues, comments


def findings(mod, issues, comments):
    return sorted({(k, g) for k, g, _ in mod.verify_lint_findings(issues, comments)})


def main():
    unpatched, patched = sys.argv[1], sys.argv[2]
    g_old, g_new = load_groom(unpatched), load_groom(patched)
    issues, comments = fetch_board()
    print(f"board: {len(issues)} issues (state=all, as board.py does)\n")

    def show(label, iss, cm=comments):
        f_old, f_new = findings(g_old, iss, cm), findings(g_new, iss, cm)
        print(f"== {label} ==")
        print(f"  unpatched findings: {f_old}")
        print(f"  patched   findings: {f_new}")
        print(f"  identical={f_old == f_new}  cleared_by_patch={sorted(set(f_old) - set(f_new))}"
              f"  added={sorted(set(f_new) - set(f_old))}")
        print(f"  GB-047 unpatched={[x for x in f_old if x[1] == 'GB-047']} "
              f"patched={[x for x in f_new if x[1] == 'GB-047']}")
        return f_old, f_new

    show("A. live board as it is now (GB-047 in In Progress after the #223 RED)", issues)

    iss_b = []
    for it in issues:
        if it["number"] == 47:
            it = dict(it)
            it["labels"] = [{"name": "In Review"} if l["name"] == "In Progress" else l
                            for l in it["labels"]]
        iss_b.append(it)
    print()
    show("B. real records, #47 lane restored to In Review (the 10:45Z state)", iss_b)

    it47 = [i for i in iss_b if i["number"] == 47][0]
    gate223 = [i for i in issues if i["number"] == 223][0]
    print(f"\n  #47 lane_entry_time (newest machine-readable move INTO a review lane) = "
          f"{g_new.lane_entry_time(it47, comments)}")
    print(f"  #223 GB-047-V created_at = {gate223['created_at']}, state={gate223['state']}")
    print(f"  #223 newest verdict (patched groom.newest_verdict) = {g_new.newest_verdict(comments(223))}")
    print(f"  gate_is_pending(#47, #223) = {g_new.gate_is_pending(it47, gate223, comments)}")

    # C: make #223's RED verdict the newest comment, as it was at 10:46:34Z, and re-run.
    cm_red = dict()
    for it in iss_b:
        cm_red[it["number"]] = list(comments(it["number"]))
    cm_red[223] = cm_red[223] + [{"created_at": "2026-10-27T10:46:34Z",
                                  "body": "## VERDICT: RED — GB-047-V (#223), PR #215 head `ed53eca`"}]
    print()
    show("C. same as B, but #223 already posted its RED verdict (the 10:46:34Z state)",
         iss_b, lambda n: cm_red.get(n, []))


if __name__ == "__main__":
    main()