#!/usr/bin/env python3
"""tools/board/poll.py — 60-second board poller (GB-003 fallback for the webhook).

Forgejo 17 has webhooks, but the Coordinator must also survive one being missed:
this polls the issue tracker every 60 s, diffs lane labels + new comments against
the last snapshot, and appends the same event records the webhook produces
(hook_server.py) so both paths feed one inbox: $BOARD_EVENT_LOG.

  poll.py [--interval 60] [--once] [--state ~/.hermes/board-poll-state.json]

Run as a cron entry or systemd unit on swarm-coordinator. Exits non-zero on API
failure so cron mails; never spins faster than --interval.
"""
import argparse
import datetime as dt
import json
import os
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import board  # noqa: E402

LOG = os.path.expanduser(os.environ.get("BOARD_EVENT_LOG", "~/.hermes/board-events.jsonl"))
STATE = os.path.expanduser(os.environ.get("BOARD_POLL_STATE", "~/.hermes/board-poll-state.json"))


def load_state():
    try:
        return json.load(open(STATE))
    except Exception:
        return {"issues": {}, "comments": {}}


def save_state(s):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    tmp = STATE + ".tmp"
    json.dump(s, open(tmp, "w"))
    os.replace(tmp, STATE)


def append(ev):
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(ev, sort_keys=True) + "\n")


def snapshot():
    issues = board.all_issues("all")
    out = {}
    for it in issues:
        lane = board.lane_of(it)
        cms = board.api_get(f"/repos/{board.REPO}/issues/{it['number']}/comments", "?limit=100")
        # keys are strings: the state file is JSON, an int-keyed dict would not round-trip
        out[str(it["number"])] = {
            "title": it["title"], "lane": lane, "state": it["state"],
            "labels": [l["name"] for l in it["labels"]],
            "comments": len(cms), "url": it["html_url"],
            "last": (cms[-1]["body"][:280] if cms else None),
        }
    return out


def tick(state):
    cur = snapshot()
    prev = state.get("issues", {})
    events = []
    ts = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    for key, now in cur.items():
        num = int(key)
        old = prev.get(key)
        if old is None:
            events.append({"ts": ts, "action": "issue_created", "issue": num, "title": now["title"][:120],
                           "lane": now["lane"], "labels": now["labels"], "sender": "poll", "wake": False})
            continue
        if old["lane"] != now["lane"]:
            events.append({"ts": ts, "action": "lane_change", "issue": num, "title": now["title"][:120],
                           "lane": now["lane"], "from": old["lane"], "labels": now["labels"], "sender": "poll",
                           "wake": now["lane"] in ("Merge Gate", "In Review") or "decision" in now["labels"], "url": now["url"]})
        if old["comments"] != now["comments"]:
            events.append({"ts": ts, "action": "comment", "issue": num, "title": now["title"][:120],
                           "lane": now["lane"], "labels": now["labels"], "sender": "poll",
                           "comment": now["last"], "wake": now["lane"] == "Merge Gate" or "decision" in now["labels"],
                           "url": now["url"]})
        if old["state"] != now["state"]:
            events.append({"ts": ts, "action": now["state"], "issue": num, "title": now["title"][:120],
                           "lane": now["lane"], "labels": now["labels"], "sender": "poll",
                           "wake": now["state"] == "closed", "url": now["url"]})
    for e in events:
        append(e)
        print(f"[poll] {e['action']:<13} #{e['issue']} lane={e['lane']} wake={e.get('wake')}", flush=True)
    state["issues"] = cur
    return state, events


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--interval", type=int, default=60)
    p.add_argument("--once", action="store_true")
    p.add_argument("--state", default=STATE)
    a = p.parse_args()
    globals()["STATE"] = a.state
    st = load_state()
    if a.once:
        st, _ = tick(st)
        save_state(st)
        return
    print(f"board poll every {a.interval}s -> {LOG}", flush=True)
    while True:
        try:
            st, _ = tick(st)
            save_state(st)
        except Exception as e:
            print(f"[poll] error: {e}", file=sys.stderr, flush=True)
        time.sleep(a.interval)


if __name__ == "__main__":
    main()
