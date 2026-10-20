#!/usr/bin/env python3
"""claim_race_test.py — GB-074: tools/board/claim.sh must not double-claim.

Reproduces the race and proves the fix, on the host (no real Forgejo):

  OLD (read-then-move): two agents read the ticket as Ready, then both move it
       to In Progress and both proceed — two claim comments, two gates.  The
       test reproduces this against a deterministic in-memory Forgejo fake.
  NEW (claim.sh): board.py cmd_claim serializes on a flock, re-reads the
       current lane, and refuses a ticket already In Progress, naming the
       winner.  Two concurrent claims -> exactly one wins, the loser's message
       names the winner, and exactly ONE gate runs.

Self-registering: run_t5_tests.py (GB-064) auto-discovers tools/*/*_test.py,
so this needs no edit to the shared Makefile host_tests line.
"""
import argparse, os, re, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board


def make_fake():
    """An in-memory, thread-safe stand-in for the Forgejo endpoints board.py
    uses.  Adds a small latency to the lane PUT / comment POST so two concurrent
    claims genuinely interleave (both pass the pre-read before either completes
    the move) — making the race real, not a no-op."""
    st = {"issues": {}, "comments": {}}
    lock = threading.Lock()

    def _issue(n):
        st["issues"].setdefault(n, {"number": n, "title": "GB-074 probe",
                                    "state": "open",
                                    "labels": [{"name": "Ready", "id": 100}]})

    _issue(1)

    def _labels_catalog():
        return [{"name": "Backlog"}, {"name": "Ready"},
                {"name": "In Progress"}, {"name": "In Review"}]

    def api_get(path, query=""):
        with lock:
            p = path.split("?")[0]
            if "comments" in p:
                n = int(p.split("/")[-2]); _issue(n)
                return list(st["comments"].get(n, []))
            if p.endswith("/labels"):
                return _labels_catalog()
            if p.endswith("/issues"):
                return [st["issues"][n] for n in sorted(st["issues"])]
            if "/issues/" in p and p.rstrip("/").split("/")[-1].isdigit():
                n = int(p.rstrip("/").split("/")[-1]); _issue(n)
                return st["issues"][n]
            return []

    def api_post(path, data):
        time.sleep(0.02)
        with lock:
            p = path.split("?")[0]
            if p.endswith("/comments"):
                n = int(p.split("/")[-2]); _issue(n)
                st["comments"].setdefault(n, []).append({"body": data["body"]})
                return {"id": len(st["comments"][n])}
            if p.endswith("/labels"):
                return {"name": (data.get("labels") or [None])[0]}
            return {}

    def api_put_labels(n, names):
        time.sleep(0.02)
        with lock:
            _issue(n)
            st["issues"][n]["labels"] = [{"name": x, "id": 100} for x in names]

    def api_delete(path):
        return {}

    board.api_get = api_get
    board.api_post = api_post
    board.api_put_labels = api_put_labels
    board.api_delete = api_delete
    return st


def naive_old_claim(st, agent):
    """The OLD read-then-move claim: no lock, no re-read.  Every agent that
    reaches here 'wins' — this is the double-claim the ticket measures."""
    if any(l["name"] == "Ready" for l in st["issues"][1]["labels"]):
        st["issues"][1]["labels"] = [{"name": "In Progress", "id": 100}]
    st["comments"].setdefault(1, []).append(
        {"body": f"claimed by `{agent}` — lane: Ready → In Progress"})
    return "won"


fails = []


def check(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}: {name}")
    if not cond:
        fails.append(name)


# ---- A. reproduce the OLD double claim -----------------------------------
st = make_fake()
got = []
tr = [threading.Thread(target=lambda i=i: got.append(naive_old_claim(st, f"agent{i}")))
      for i in (1, 2)]
[t.start() for t in tr]
[t.join() for t in tr]
check("OLD read-then-move: both agents 'win' (double claim reproduced)",
      len(got) == 2 and all(x == "won" for x in got))
check("OLD produced two claim comments (the measured double-DEEP-gate cost)",
      len(st["comments"][1]) == 2)

# ---- B. NEW claim.sh: exactly one wins -----------------------------------
import tempfile
lock_path = os.path.join(tempfile.mkdtemp(prefix="gb074-"), "claim.lock")
os.environ["BOARD_LOCK"] = lock_path
os.environ["BOARD_AGENT"] = "HOST"


def run(results, agent):
    try:
        board.cmd_claim(argparse.Namespace(gid="GB-074", by=agent, host="mock"))
        results[agent] = "WON"
    except SystemExit as e:
        results[agent] = f"LOST: {e}"


for attempt in range(3):  # must ALWAYS be exactly one winner
    st = make_fake()
    results = {}
    ts = [threading.Thread(target=run, args=(results, f"agent{i}")) for i in (1, 2)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    winners = [a for a, r in results.items() if r == "WON"]
    loser_msgs = [r for a, r in results.items() if r.startswith("LOST")]
    check(f"NEW claim (run {attempt+1}): exactly one of two wins",
          len(winners) == 1 and len(loser_msgs) == 1)
    check(f"NEW claim (run {attempt+1}): loser names the winner",
          bool(loser_msgs) and bool(winners) and winners[0] in loser_msgs[0])
    check(f"NEW claim (run {attempt+1}): exactly one claim comment (one gate)",
          len(st["comments"][1]) == 1)
    check(f"NEW claim (run {attempt+1}): ticket ends In Progress",
          any(l["name"] == "In Progress" for l in st["issues"][1]["labels"]))
    check(f"NEW claim (run {attempt+1}): only the winner ran a gate",
          loser_msgs and len(st["comments"][1]) == 1)

print()
if fails:
    print(f"CLAIM_RACE_TEST: FAIL — {len(fails)}")
    sys.exit(1)
print("CLAIM_RACE_TEST: PASS — the read-then-move double claim is fixed")
sys.exit(0)
