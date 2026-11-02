#!/usr/bin/env python3
"""status_test.py — GB-158: board.py status must report OPEN lane occupancy and
must expose closed tickets left outside Done.

Measured defect: cmd_status buckets all_issues("all") by lane, so a ticket that a
PR closed without a lane move keeps counting as live lane work (`Ready 1` with no
claimable card; `In Progress 11` for 2 open cards).  lint() walks open issues
only, so nothing else can see the stale lanes.

Runs against an in-memory board; no Forgejo needed.  Auto-discovered by
tools/t5/run_t5_tests.py (`tools/*/*_test.py`), so no Makefile edit.
"""
import contextlib, io, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board

fails = []


def check(name, cond):
    print(f"  {'PASS' if cond else 'FAIL'}: {name}")
    if not cond:
        fails.append(name)


def issue(n, lane, state):
    return {"number": n, "title": f"GB-{n:03d} probe", "state": state,
            "labels": [{"name": lane}] if lane else []}


ISSUES = [
    issue(1, "Ready", "open"),
    issue(2, "Ready", "open"),
    issue(3, "Ready", "closed"),       # closed by a PR; lane label never moved
    issue(4, "Done", "closed"),
    issue(5, "In Review", "open"),
]

board.all_issues = lambda state=None: list(ISSUES)
board.api_get = lambda path, query="": []

buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    board.cmd_status(None)
out = buf.getvalue()


def row(lane):
    m = re.search(rf"^\s+{re.escape(lane)}\s+(\d+)(.*)$", out, re.M)
    return (int(m.group(1)), m.group(2)) if m else (None, "")


ready_n, _ = row("Ready")
check("Ready row counts only the two OPEN Ready cards (not the closed one)",
      ready_n == 2)
done_n, done_rest = row("Done")
check("Done row reports the closed count and says so",
      done_n == 1 and "closed" in done_rest)
review_n, _ = row("In Review")
check("In Review row is the open count", review_n == 1)
check("header splits open/closed totals", "3 open, 2 closed" in out)
check("closed-outside-Done is named with the ticket id",
      "closed outside Done: 1" in out and "#3" in out)

# A zero lane must still be visible: "Ready 0" is how a worker knows the lane is
# empty; omitting the row is how the pre-fix rollup hid the phantom Ready.
ISSUES[:] = [issue(9, "Done", "closed")]
buf2 = io.StringIO()
with contextlib.redirect_stdout(buf2):
    board.cmd_status(None)
out2 = buf2.getvalue()
check("an empty lane is still printed as 0 (Ready 0)",
      bool(re.search(r"^  Ready\s+0$", out2, re.M)))

print()
if fails:
    print(f"STATUS_TEST: FAIL — {len(fails)}")
    sys.exit(1)
print("STATUS_TEST: PASS — lane rows are open counts, stale closed lanes are named")
sys.exit(0)
